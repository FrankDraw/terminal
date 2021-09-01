// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "CascadiaSettings.h"

#include <LibraryResources.h>
#include <fmt/chrono.h>
#include <shlobj.h>
#include <til/latch.h>

#include "AzureCloudShellGenerator.h"
#include "PowershellCoreProfileGenerator.h"
#include "WslDistroGenerator.h"

// The following files are generated at build time into the "Generated Files" directory.
// defaults(-universal).h is a file containing the default json settings in a std::string_view.
#include "defaults.h"
#include "defaults-universal.h"
// userDefault.h is like the above, but with a default template for the user's settings.json.
#include <LegacyProfileGeneratorNamespaces.h>

#include "userDefaults.h"

#include "ApplicationState.h"
#include "FileUtils.h"

using namespace winrt::Microsoft::Terminal::Settings::Model::implementation;
using namespace ::Microsoft::Console;
using namespace ::Microsoft::Terminal::Settings::Model;

static constexpr std::wstring_view SettingsFilename{ L"settings.json" };
static constexpr std::wstring_view DefaultsFilename{ L"defaults.json" };

static constexpr std::string_view ProfilesKey{ "profiles" };
static constexpr std::string_view DefaultSettingsKey{ "defaults" };
static constexpr std::string_view ProfilesListKey{ "list" };
static constexpr std::string_view SchemesKey{ "schemes" };
static constexpr std::string_view NameKey{ "name" };
static constexpr std::string_view UpdatesKey{ "updates" };
static constexpr std::string_view GuidKey{ "guid" };

static constexpr std::wstring_view jsonExtension{ L".json" };
static constexpr std::wstring_view FragmentsSubDirectory{ L"\\Fragments" };
static constexpr std::wstring_view FragmentsPath{ L"\\Microsoft\\Windows Terminal\\Fragments" };

static constexpr std::wstring_view AppExtensionHostName{ L"com.microsoft.windows.terminal.settings" };

// make sure this matches defaults.json.
static constexpr winrt::guid DEFAULT_WINDOWS_POWERSHELL_GUID{ 0x61c54bbd, 0xc2c6, 0x5271, { 0x96, 0xe7, 0x00, 0x9a, 0x87, 0xff, 0x44, 0xbf } };
static constexpr winrt::guid DEFAULT_COMMAND_PROMPT_GUID{ 0x0caa0dad, 0x35be, 0x5f56, { 0xa8, 0xff, 0xaf, 0xce, 0xee, 0xaa, 0x61, 0x01 } };

template<typename T>
static void executeGenerator(const std::unordered_set<std::wstring_view>& ignoredNamespaces, std::vector<winrt::com_ptr<Profile>>& generatedProfiles)
{
    T generator;
    const auto generatorNamespace = generator.GetNamespace();

    if (!ignoredNamespaces.count(generatorNamespace))
    {
        try
        {
            generator.GenerateProfiles(generatedProfiles);
        }
        CATCH_LOG_MSG("Dynamic Profile Namespace: \"%s\"", generatorNamespace.data());
    }
}

// Function Description:
// - Extracting the value from an async task (like talking to the app catalog) when we are on the
//   UI thread causes C++/WinRT to complain quite loudly (and halt execution!)
//   This templated function extracts the result from a task with chicanery.
template<typename TTask>
static auto extractValueFromTaskWithoutMainThreadAwait(TTask&& task) -> decltype(task.get())
{
    std::optional<decltype(task.get())> finalVal;
    til::latch latch{ 1 };

    const auto _ = [&]() -> winrt::fire_and_forget {
        co_await winrt::resume_background();
        finalVal.emplace(co_await task);
        latch.count_down();
    }();

    latch.wait();
    return finalVal.value();
}

static std::pair<size_t, size_t> lineAndColumnFromPosition(const std::string_view string, const size_t position)
{
    size_t line = 1;
    size_t pos = 0;

    for (;;)
    {
        const auto p = string.find('\n', pos);
        if (p >= position)
        {
            break;
        }

        pos = p + 1;
        line++;
    }

    return { line, position - pos + 1 };
}

static void rethrowSerializationExceptionWithLocationInfo(const JsonUtils::DeserializationError& e, std::string_view settingsString)
{
    static constexpr std::string_view basicHeader{ "* Line {line}, Column {column}\n{message}" };
    static constexpr std::string_view keyedHeader{ "* Line {line}, Column {column} ({key})\n{message}" };

    std::string jsonValueAsString{ "array or object" };
    try
    {
        jsonValueAsString = e.jsonValue.asString();
        if (e.jsonValue.isString())
        {
            jsonValueAsString = fmt::format("\"{}\"", jsonValueAsString);
        }
    }
    catch (...)
    {
        // discard: we're in the middle of error handling
    }

    auto msg = fmt::format("  Have: {}\n  Expected: {}", jsonValueAsString, e.expectedType);

    auto [l, c] = lineAndColumnFromPosition(settingsString, static_cast<size_t>(e.jsonValue.getOffsetStart()));
    msg = fmt::format((e.key ? keyedHeader : basicHeader),
                      fmt::arg("line", l),
                      fmt::arg("column", c),
                      fmt::arg("key", e.key.value_or("")),
                      fmt::arg("message", msg));
    throw SettingsTypedDeserializationException{ msg };
}

static Json::Value parseJSON(const std::string_view& content)
{
    Json::Value json;
    std::string errs;
    const std::unique_ptr<Json::CharReader> reader{ Json::CharReaderBuilder::CharReaderBuilder().newCharReader() };

    if (!reader->parse(content.data(), content.data() + content.size(), &json, &errs))
    {
        throw winrt::hresult_error(WEB_E_INVALID_JSON_STRING, winrt::to_hstring(errs));
    }

    return json;
}

static const Json::Value& getJSONValue(const Json::Value& json, const std::string_view& key) noexcept
{
    if (json.isObject())
    {
        if (const auto val = json.find(key.data(), key.data() + key.size()))
        {
            return *val;
        }
    }

    return Json::Value::nullSingleton();
}

// Function Description:
// - Given a json serialization of a profile, this function will determine
//   whether it is "well-formed". We introduced a bug (GH#9962, fixed in GH#9964)
//   that would result in one or more nameless, guid-less profiles being emitted
//   into the user's settings file. Those profiles would show up in the list as
//   "Default" later.
static bool isValidProfileObject(const Json::Value& profileJson)
{
    return profileJson.isObject() &&
           (profileJson.isMember(NameKey.data(), NameKey.data() + NameKey.size()) || // has a name (can generate a guid)
            profileJson.isMember(GuidKey.data(), GuidKey.data() + GuidKey.size())); // or has a guid
}

ParsedSettings::ParsedSettings(const OriginTag origin, const std::string_view& content)
{
    const auto json = parseJSON(content);

    globals = GlobalAppSettings::FromJson(json);

    if (const auto& schemes = getJSONValue(json, SchemesKey))
    {
        for (const auto& schemeJson : schemes)
        {
            if (schemeJson.isObject() && ColorScheme::ValidateColorScheme(schemeJson))
            {
                globals->AddColorScheme(*ColorScheme::FromJson(schemeJson));
            }
        }
    }

    if (const auto& profilesObject = getJSONValue(json, ProfilesKey))
    {
        const auto& defaultsObject = getJSONValue(profilesObject, DefaultSettingsKey);
        const auto& profilesArray = profilesObject.isArray() ? profilesObject : getJSONValue(profilesObject, ProfilesListKey);

        {
            profileDefaults = Profile::FromJson(defaultsObject);
            // Remove the `guid` member from the default settings.
            // That'll hyper-explode, so just don't let them do that.
            profileDefaults->ClearGuid();
            profileDefaults->Origin(OriginTag::ProfilesDefaults);
        }

        profilesByGuid.reserve(profilesArray.size());

        for (auto profileJson : profilesArray)
        {
            if (isValidProfileObject(profileJson))
            {
                const auto profile = Profile::FromJson(profileJson);
                profile->Origin(origin);

                // Love it.
                if (!profile->HasGuid())
                {
                    profile->Guid(profile->Guid());
                }

                if (profilesByGuid.emplace(profile->Guid(), profile).second)
                {
                    profiles.emplace_back(std::move(profile));
                }
                else
                {
                    // TODO: should produce a load warning for duplicate profiles
                }
            }
        }
    }
}

bool CascadiaSettings::_load(const std::wstring_view& defaultJson, const std::wstring_view& userJson)
{
    return _load(til::u16u8(defaultJson), til::u16u8(userJson));
}

bool CascadiaSettings::_load(const std::string_view& defaultJson, const std::string_view& userJson)
{
    UNREFERENCED_PARAMETER(defaultJson);
    UNREFERENCED_PARAMETER(userJson);
    return false;
}

winrt::com_ptr<Profile> reproduceProfile(const winrt::com_ptr<Profile>& parent)
{
    auto profile = winrt::make_self<Profile>();
    profile->Origin(parent->Origin());
    profile->Name(parent->Name());
    profile->Guid(parent->Guid());
    profile->Hidden(parent->Hidden());
    profile->Source(parent->Source());
    profile->InsertParent(parent);
    return profile;
}

// Method Description:
// - Creates a CascadiaSettings from whatever's saved on disk, or instantiates
//      a new one with the default values. If we're running as a packaged app,
//      it will load the settings from our packaged localappdata. If we're
//      running as an unpackaged application, it will read it from the path
//      we've set under localappdata.
// - Loads both the settings from the defaults.json and the user's settings.json
// - Also runs and dynamic profile generators. If any of those generators create
//   new profiles, we'll write the user settings back to the file, with the new
//   profiles inserted into their list of profiles.
// Return Value:
// - a unique_ptr containing a new CascadiaSettings object.
winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings CascadiaSettings::LoadAll()
try
{
    const auto settingsString = ReadUTF8FileIfExists(_SettingsPath()).value_or(std::string{});
    const auto settingsStringView = settingsString.empty() ? UserSettingsJson : settingsString;
    bool needToWriteFile = settingsString.empty();

    ParsedSettings defaultSettings{ OriginTag::InBox, DefaultJson };
    ParsedSettings userSettings;

    try
    {
        userSettings = ParsedSettings{ OriginTag::User, settingsStringView };
    }
    catch (const JsonUtils::DeserializationError& e)
    {
        rethrowSerializationExceptionWithLocationInfo(e, settingsStringView);
    }

    const auto userProfileCount = userSettings.profiles.size();

    // TODO: This std::wstring_view should be replaced with a winrt::hstring
    // winrt::hstring is reference counted and the overhead for copying fairly minimal.
    // This code uses std::wstring_view until C++ has P0919R2 heterogeneous lookup,
    // because not all places use a winrt::hstring for lookup.
    std::unordered_set<std::wstring_view> ignoredNamespaces;
    if (const auto sources = userSettings.globals->DisabledProfileSources())
    {
        ignoredNamespaces.reserve(sources.Size());
        for (const auto& id : sources)
        {
            ignoredNamespaces.emplace(id);
        }
    }

    std::vector<winrt::com_ptr<Profile>> generatedProfiles = defaultSettings.profiles;
    executeGenerator<PowershellCoreProfileGenerator>(ignoredNamespaces, generatedProfiles);
    executeGenerator<WslDistroGenerator>(ignoredNamespaces, generatedProfiles);
    executeGenerator<AzureCloudShellGenerator>(ignoredNamespaces, generatedProfiles);

    // A new settings.json gets a special treatment:
    // 1. The default profile is a PowerShell 7+ one, if one was generated,
    //    and falls back to the standard PowerShell 5 profile otherwise.
    // 2. cmd.exe gets a localized name.
    if (settingsString.empty())
    {
        // 1.
        {
            const auto preferredPowershellProfile = PowershellCoreProfileGenerator::GetPreferredPowershellProfileName();
            auto guid = DEFAULT_WINDOWS_POWERSHELL_GUID;

            for (const auto& profile : generatedProfiles)
            {
                if (profile->Name() == preferredPowershellProfile)
                {
                    guid = profile->Guid();
                    break;
                }
            }

            userSettings.globals->DefaultProfile(guid);
        }

        // 2.
        {
            for (const auto& profile : userSettings.profiles)
            {
                if (profile->Guid() == DEFAULT_COMMAND_PROMPT_GUID)
                {
                    profile->Name(RS_(L"CommandPromptDisplayName"));
                    break;
                }
            }
        }
    }

    // Layer generated profiles onto user profiles
    for (const auto& generatedProfile : generatedProfiles)
    {
        const auto guid = generatedProfile->Guid();
        if (const auto [it, inserted] = userSettings.profilesByGuid.emplace(guid, generatedProfile); !inserted)
        {
            // Handle layering generated profiles onto user profiles.
            it->second->InsertParent(generatedProfile);
        }
        else
        {
            // Fallback to creating new user profiles
            userSettings.profiles.emplace_back(reproduceProfile(generatedProfile));
        }
    }

    {
        static constexpr auto parseAndLayerFragmentFiles = [](ParsedSettings& userSettings, const std::filesystem::path& path, const winrt::hstring& source) {
            for (const auto& fragmentExt : std::filesystem::directory_iterator(path))
            {
                if (fragmentExt.path().extension() == jsonExtension)
                {
                    try
                    {
                        const auto content = ReadUTF8File(fragmentExt.path());
                        ParsedSettings settings{ OriginTag::Fragment, content };

                        for (const auto& fragmentProfile : settings.profiles)
                        {
                            if (const auto updates = fragmentProfile->Updates(); updates != winrt::guid{})
                            {
                                if (const auto it = userSettings.profilesByGuid.find(updates); it != userSettings.profilesByGuid.end())
                                {
                                    fragmentProfile->Source(source);
                                    it->second->InsertParent(0, fragmentProfile);
                                }
                            }
                            else
                            {
                                // TODO: GUID uniqueness?
                                fragmentProfile->Source(source);
                                userSettings.profiles.emplace_back(reproduceProfile(fragmentProfile));
                            }
                        }
                    }
                    CATCH_LOG();
                }
            }
        };

        for (const auto& rfid : std::array{ FOLDERID_LocalAppData, FOLDERID_ProgramData })
        {
            wil::unique_cotaskmem_string folder;
            THROW_IF_FAILED(SHGetKnownFolderPath(rfid, 0, nullptr, &folder));

            std::wstring fragmentPath{ folder.get() };
            fragmentPath.append(FragmentsPath);

            if (std::filesystem::exists(fragmentPath))
            {
                for (const auto& fragmentExtFolder : std::filesystem::directory_iterator(fragmentPath))
                {
                    const auto filename = fragmentExtFolder.path().filename();
                    const auto& source = filename.native();

                    if (!ignoredNamespaces.count(std::wstring_view{ source }) && std::filesystem::is_directory(fragmentExtFolder))
                    {
                        parseAndLayerFragmentFiles(userSettings, fragmentExtFolder.path(), winrt::hstring{ source });
                    }
                }
            }
        }

        // Search through app extensions
        // Gets the catalog of extensions with the name "com.microsoft.windows.terminal.settings"
        const auto catalog = Windows::ApplicationModel::AppExtensions::AppExtensionCatalog::Open(AppExtensionHostName);
        const auto extensions = extractValueFromTaskWithoutMainThreadAwait(catalog.FindAllAsync());

        for (const auto& ext : extensions)
        {
            const auto packageName = ext.Package().Id().FamilyName();
            if (ignoredNamespaces.count(std::wstring_view{ packageName }))
            {
                continue;
            }

            // Likewise, getting the public folder from an extension is an async operation.
            auto foundFolder = extractValueFromTaskWithoutMainThreadAwait(ext.GetPublicFolderAsync());
            if (!foundFolder)
            {
                continue;
            }

            // the StorageFolder class has its own methods for obtaining the files within the folder
            // however, all those methods are Async methods
            // you may have noticed that we need to resort to clunky implementations for async operations
            // (they are in extractValueFromTaskWithoutMainThreadAwait)
            // so for now we will just take the folder path and access the files that way
            std::wstring path{ foundFolder.Path() };
            path.append(FragmentsSubDirectory);

            if (std::filesystem::is_directory(path))
            {
                parseAndLayerFragmentFiles(userSettings, path, packageName);
            }
        }
    }

    for (const auto& profile : userSettings.profiles)
    {
        profile->InsertParent(0, userSettings.profileDefaults);
    }

    {
        const auto& state = winrt::get_self<ApplicationState>(ApplicationState::SharedInstance());
        auto generatedProfileIds = state->GeneratedProfiles();
        bool newGeneratedProfiles = false;

        for (const auto& profile : gsl::make_span(userSettings.profiles).subspan(userProfileCount))
        {
            // Let's say a user doesn't know that they need to write `"hidden": true` in
            // order to prevent a profile from showing up (and a settings UI doesn't exist).
            // Naturally they would open settings.json and try to remove the profile object.
            // This section of code recognizes if a profile was seen before and marks it as
            // `"hidden": true` by default and thus ensures the behavior the user expects:
            // Profiles won't show up again after they've been removed from settings.json.
            if (generatedProfileIds.emplace(profile->Guid()).second)
            {
                newGeneratedProfiles = true;
            }
            else
            {
                profile->Deleted(true);
                profile->Hidden(true);
            }
        }

        if (newGeneratedProfiles)
        {
            state->GeneratedProfiles(generatedProfileIds);
        }
    }
    
    // Layer default globals onto user globals
    {
        userSettings.globals->InsertParent(defaultSettings.globals);
        userSettings.globals->_FinalizeInheritance();
    }

    // Layer default profile defaults onto user profile defaults
    {
        userSettings.profileDefaults->InsertParent(defaultSettings.profileDefaults);
        userSettings.profileDefaults->_FinalizeInheritance();
    }

    const auto settings = winrt::make_self<CascadiaSettings>();
    {
        std::vector<Model::Profile> allProfiles;
        std::vector<Model::Profile> activeProfiles;

        allProfiles.reserve(userSettings.profiles.size());

        for (const auto& profile : userSettings.profiles)
        {
            profile->_FinalizeInheritance();
            allProfiles.emplace_back(*profile);
            if (!profile->Hidden())
            {
                activeProfiles.emplace_back(*profile);
            }
        }

        settings->_globals = userSettings.globals;
        settings->_allProfiles = winrt::single_threaded_observable_vector(std::move(allProfiles));
        settings->_userDefaultProfileSettings = userSettings.profileDefaults;
    }

    // If this throws, the app will catch it and use the default settings
    settings->_FinalizeSettings();
    settings->_ValidateSettings();

    // If we created the file, or found new dynamic profiles, write the user
    // settings string back to the file.
    if (needToWriteFile)
    {
        try
        {
            settings->WriteSettingsToDisk();
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            settings->_warnings.Append(SettingsLoadWarnings::FailedToWriteToSettings);
        }
    }

    return *settings;
}
catch (const SettingsException& ex)
{
    auto settings{ winrt::make_self<implementation::CascadiaSettings>() };
    settings->_loadError = ex.Error();
    return *settings;
}
catch (const SettingsTypedDeserializationException& e)
{
    auto settings{ winrt::make_self<implementation::CascadiaSettings>() };
    std::string_view what{ e.what() };
    settings->_deserializationErrorMessage = til::u8u16(what);
    return *settings;
}

// Function Description:
// - Loads a batch of settings curated for the Universal variant of the terminal app
// Arguments:
// - <none>
// Return Value:
// - a unique_ptr to a CascadiaSettings with the connection types and settings for Universal terminal
winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings CascadiaSettings::LoadUniversal()
{
    const ParsedSettings parsed{ OriginTag::InBox, DefaultUniversalJson };

    const auto resultPtr{ winrt::make_self<CascadiaSettings>() };
    resultPtr->_globals = parsed.globals;
    resultPtr->_allProfiles = parsed.fuckyou1();
    resultPtr->_FinalizeSettings();
    return *resultPtr;
}

// Function Description:
// - Creates a new CascadiaSettings object initialized with settings from the
//   hardcoded defaults.json.
// Arguments:
// - <none>
// Return Value:
// - a unique_ptr to a CascadiaSettings with the settings from defaults.json
winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings CascadiaSettings::LoadDefaults()
{
    const ParsedSettings parsed{ OriginTag::InBox, DefaultJson };

    const auto resultPtr{ winrt::make_self<CascadiaSettings>() };
    resultPtr->_globals = parsed.globals;
    resultPtr->_allProfiles = parsed.fuckyou1();
    resultPtr->_FinalizeSettings();
    return *resultPtr;
}

// Method Description:
// - Returns the path of the settings.json file.
// Arguments:
// - <none>
// Return Value:
// - Returns a path in 80% of cases. I measured!
const std::filesystem::path& CascadiaSettings::_SettingsPath()
{
    static const auto path = GetBaseSettingsPath() / SettingsFilename;
    return path;
}

// function Description:
// - Returns the full path to the settings file, either within the application
//   package, or in its unpackaged location. This path is under the "Local
//   AppData" folder, so it _doesn't_ roam to other machines.
// - If the application is unpackaged,
//   the file will end up under e.g. C:\Users\admin\AppData\Local\Microsoft\Windows Terminal\settings.json
// Arguments:
// - <none>
// Return Value:
// - the full path to the settings file
winrt::hstring CascadiaSettings::SettingsPath()
{
    return winrt::hstring{ _SettingsPath().native() };
}

winrt::hstring CascadiaSettings::DefaultSettingsPath()
{
    // Both of these posts suggest getting the path to the exe, then removing
    // the exe's name to get the package root:
    // * https://blogs.msdn.microsoft.com/appconsult/2017/06/23/accessing-to-the-files-in-the-installation-folder-in-a-desktop-bridge-application/
    // * https://blogs.msdn.microsoft.com/appconsult/2017/03/06/handling-data-in-a-converted-desktop-app-with-the-desktop-bridge/
    //
    // This would break if we ever moved our exe out of the package root.
    // HOWEVER, if we try to look for a defaults.json that's simply in the same
    // directory as the exe, that will work for unpackaged scenarios as well. So
    // let's try that.

    std::wstring exePathString;
    THROW_IF_FAILED(wil::GetModuleFileNameW(nullptr, exePathString));

    std::filesystem::path path{ exePathString };
    path.replace_filename(DefaultsFilename);
    return winrt::hstring{ path.wstring() };
}

// Method Description:
// - Write the current state of CascadiaSettings to our settings file
// - Create a backup file with the current contents, if one does not exist
// - Persists the default terminal handler choice to the registry
// Arguments:
// - <none>
// Return Value:
// - <none>
void CascadiaSettings::WriteSettingsToDisk() const
{
    const auto settingsPath = _SettingsPath();

    {
        // create a timestamped backup file
        const auto backupSettingsPath = fmt::format(L"{}.{:%Y-%m-%dT%H-%M-%S}.backup", settingsPath.native(), fmt::localtime(std::time(nullptr)));
        LOG_IF_WIN32_BOOL_FALSE(CopyFileW(settingsPath.c_str(), backupSettingsPath.c_str(), TRUE));
    }

    // write current settings to current settings file
    Json::StreamWriterBuilder wbuilder;
    wbuilder.settings_["indentation"] = "    ";
    wbuilder.settings_["enableYAMLCompatibility"] = true; // suppress spaces around colons

    const auto styledString{ Json::writeString(wbuilder, ToJson()) };
    WriteUTF8FileAtomic(settingsPath, styledString);

    // Persists the default terminal choice
    // GH#10003 - Only do this if _currentDefaultTerminal was actually initialized.
    if (_currentDefaultTerminal)
    {
        Model::DefaultTerminal::Current(_currentDefaultTerminal);
    }
}

// Method Description:
// - Create a new serialized JsonObject from an instance of this class
// Arguments:
// - <none>
// Return Value:
// the JsonObject representing this instance
Json::Value CascadiaSettings::ToJson() const
{
    // top-level json object
    Json::Value json{ _globals->ToJson() };
    json["$help"] = "https://aka.ms/terminal-documentation";
    json["$schema"] = "https://aka.ms/terminal-profiles-schema";

    // "profiles" will always be serialized as an object
    Json::Value profiles{ Json::ValueType::objectValue };
    profiles[JsonKey(DefaultSettingsKey)] = _userDefaultProfileSettings ? _userDefaultProfileSettings->ToJson() :
                                                                          Json::ValueType::objectValue;
    Json::Value profilesList{ Json::ValueType::arrayValue };
    for (const auto& entry : _allProfiles)
    {
        if (!entry.Deleted())
        {
            const auto prof{ winrt::get_self<implementation::Profile>(entry) };
            profilesList.append(prof->ToJson());
        }
    }
    profiles[JsonKey(ProfilesListKey)] = profilesList;
    json[JsonKey(ProfilesKey)] = profiles;

    // TODO GH#8100:
    // "schemes" will be an accumulation of _all_ the color schemes
    // including all of the ones from defaults.json
    Json::Value schemes{ Json::ValueType::arrayValue };
    for (const auto& entry : _globals->ColorSchemes())
    {
        const auto scheme{ winrt::get_self<implementation::ColorScheme>(entry.Value()) };
        schemes.append(scheme->ToJson());
    }
    json[JsonKey(SchemesKey)] = schemes;

    return json;
}

// Runs some final adjustments before LoadDefaults(), or LoadAll(), etc. return.
void CascadiaSettings::_FinalizeSettings()
{
    _UpdateActiveProfiles();
    _ResolveDefaultProfile();
}

// Updates the list of active profiles from the list of all profiles.
// If there are no active profiles (all profiles are hidden), throw a SettingsException.
void CascadiaSettings::_UpdateActiveProfiles()
{
    _activeProfiles.Clear();
    for (auto const& profile : _allProfiles)
    {
        if (!profile.Hidden())
        {
            _activeProfiles.Append(profile);
        }
    }

    // Throw an exception if all profiles are hidden, so the app can use the defaults.
    if (_activeProfiles.Size() == 0)
    {
        throw SettingsException(SettingsLoadErrors::AllProfilesHidden);
    }
}

// Resolves the "defaultProfile", which can be a profile name,
// to a GUID and stores it back to the globals.
void CascadiaSettings::_ResolveDefaultProfile() const
{
    const auto unparsedDefaultProfile{ GlobalSettings().UnparsedDefaultProfile() };
    if (!unparsedDefaultProfile.empty())
    {
        const auto maybeParsedDefaultProfile{ _GetProfileGuidByName(unparsedDefaultProfile) };
        const auto defaultProfileGuid{ til::coalesce_value(maybeParsedDefaultProfile, winrt::guid{}) };
        GlobalSettings().DefaultProfile(defaultProfileGuid);
    }
}

// Method Description:
// - Attempts to validate this settings structure. If there are critical errors
//   found, they'll be thrown as a SettingsLoadError. Non-critical errors, such
//   as not finding the default profile, will only result in an error. We'll add
//   all these warnings to our list of warnings, and the application can chose
//   to display these to the user.
// Arguments:
// - <none>
// Return Value:
// - <none>
void CascadiaSettings::_ValidateSettings()
{
    _ValidateProfilesExist();
    _ValidateNoDuplicateProfiles();
    _ValidateDefaultProfileExists();
    _ValidateAllSchemesExist();
    _ValidateMediaResources();
    _ValidateKeybindings();
    _ValidateColorSchemesInCommands();
}

// Method Description:
// - Checks if the settings contain profiles at all. As we'll need to have some
//   profiles at all, we'll throw an error if there aren't any profiles.
void CascadiaSettings::_ValidateProfilesExist() const
{
    if (_allProfiles.Size() == 0)
    {
        // Throw an exception. This is an invalid state, and we want the app to
        // be able to gracefully use the default settings.

        // We can't add the warning to the list of warnings here, because this
        // object is not going to be returned at any point.

        throw SettingsException(SettingsLoadErrors::NoProfiles);
    }
}

// Method Description:
// - Checks if the "defaultProfile" is set to one of the profiles we
//   actually have. If the value is unset, or the value is set to something that
//   doesn't exist in the list of profiles, we'll arbitrarily pick the first
//   profile to use temporarily as the default.
// - Appends a SettingsLoadWarnings::MissingDefaultProfile to our list of
//   warnings if we failed to find the default.
void CascadiaSettings::_ValidateDefaultProfileExists()
{
    const winrt::guid defaultProfileGuid{ GlobalSettings().DefaultProfile() };
    const bool nullDefaultProfile = defaultProfileGuid == winrt::guid{};
    bool defaultProfileNotInProfiles = true;
    for (const auto& profile : _allProfiles)
    {
        if (profile.Guid() == defaultProfileGuid)
        {
            defaultProfileNotInProfiles = false;
            break;
        }
    }

    if (nullDefaultProfile || defaultProfileNotInProfiles)
    {
        _warnings.Append(SettingsLoadWarnings::MissingDefaultProfile);
        // Use the first profile as the new default

        // _temporarily_ set the default profile to the first profile. Because
        // we're adding a warning, this settings change won't be re-serialized.
        GlobalSettings().DefaultProfile(_allProfiles.GetAt(0).Guid());
    }
}

// Method Description:
// - Checks to make sure there aren't any duplicate profiles in the list of
//   profiles. If so, we'll remove the subsequent entries (temporarily), as they
//   won't be accessible anyways.
// - Appends a SettingsLoadWarnings::DuplicateProfile to our list of warnings if
//   we find any such duplicate.
void CascadiaSettings::_ValidateNoDuplicateProfiles() const
{
    bool foundDupe = false;

    std::vector<uint32_t> indicesToDelete;
    std::unordered_set<winrt::guid> uniqueGuids;

    // Try collecting all the unique guids. If we ever encounter a guid that's
    // already in the set, then we need to delete that profile.
    for (uint32_t i = 0; i < _allProfiles.Size(); i++)
    {
        if (!uniqueGuids.insert(_allProfiles.GetAt(i).Guid()).second)
        {
            foundDupe = true;
            indicesToDelete.push_back(i);
        }
    }

    // Remove all the duplicates we've marked
    // Walk backwards, so we don't accidentally shift any of the elements
    for (auto iter = indicesToDelete.rbegin(); iter != indicesToDelete.rend(); ++iter)
    {
        _allProfiles.RemoveAt(*iter);
    }

    if (foundDupe)
    {
        _warnings.Append(SettingsLoadWarnings::DuplicateProfile);
    }
}

// Method Description:
// - Ensures that every profile has a valid "color scheme" set. If any profile
//   has a colorScheme set to a value which is _not_ the name of an actual color
//   scheme, we'll set the color table of the profile to something reasonable.
// Arguments:
// - <none>
// Return Value:
// - <none>
// - Appends a SettingsLoadWarnings::UnknownColorScheme to our list of warnings if
//   we find any such duplicate.
void CascadiaSettings::_ValidateAllSchemesExist()
{
    bool foundInvalidScheme = false;
    for (auto profile : _allProfiles)
    {
        const auto schemeName = profile.DefaultAppearance().ColorSchemeName();
        if (!_globals->ColorSchemes().HasKey(schemeName))
        {
            // Clear the user set color scheme. We'll just fallback instead.
            profile.DefaultAppearance().ClearColorSchemeName();
            foundInvalidScheme = true;
        }
        if (profile.UnfocusedAppearance())
        {
            const auto unfocusedSchemeName = profile.UnfocusedAppearance().ColorSchemeName();
            if (!_globals->ColorSchemes().HasKey(unfocusedSchemeName))
            {
                profile.UnfocusedAppearance().ClearColorSchemeName();
                foundInvalidScheme = true;
            }
        }
    }

    if (foundInvalidScheme)
    {
        _warnings.Append(SettingsLoadWarnings::UnknownColorScheme);
    }
}

// Method Description:
// - Ensures that all specified images resources (icons and background images) are valid URIs.
//   This does not verify that the icon or background image files are encoded as an image.
// Arguments:
// - <none>
// Return Value:
// - <none>
// - Appends a SettingsLoadWarnings::InvalidBackgroundImage to our list of warnings if
//   we find any invalid background images.
// - Appends a SettingsLoadWarnings::InvalidIconImage to our list of warnings if
//   we find any invalid icon images.
void CascadiaSettings::_ValidateMediaResources()
{
    bool invalidBackground{ false };
    bool invalidIcon{ false };

    for (auto profile : _allProfiles)
    {
        if (!profile.DefaultAppearance().BackgroundImagePath().empty())
        {
            // Attempt to convert the path to a URI, the ctor will throw if it's invalid/unparseable.
            // This covers file paths on the machine, app data, URLs, and other resource paths.
            try
            {
                winrt::Windows::Foundation::Uri imagePath{ profile.DefaultAppearance().ExpandedBackgroundImagePath() };
            }
            catch (...)
            {
                // reset background image path
                profile.DefaultAppearance().BackgroundImagePath(L"");
                invalidBackground = true;
            }
        }

        if (profile.UnfocusedAppearance())
        {
            if (!profile.UnfocusedAppearance().BackgroundImagePath().empty())
            {
                // Attempt to convert the path to a URI, the ctor will throw if it's invalid/unparseable.
                // This covers file paths on the machine, app data, URLs, and other resource paths.
                try
                {
                    winrt::Windows::Foundation::Uri imagePath{ profile.UnfocusedAppearance().ExpandedBackgroundImagePath() };
                }
                catch (...)
                {
                    // reset background image path
                    profile.UnfocusedAppearance().BackgroundImagePath(L"");
                    invalidBackground = true;
                }
            }
        }

        if (!profile.Icon().empty())
        {
            const auto iconPath{ wil::ExpandEnvironmentStringsW<std::wstring>(profile.Icon().c_str()) };
            try
            {
                winrt::Windows::Foundation::Uri imagePath{ iconPath };
            }
            catch (...)
            {
                // Anything longer than 2 wchar_t's _isn't_ an emoji or symbol,
                // so treat it as an invalid path.
                if (iconPath.size() > 2)
                {
                    // reset icon path
                    profile.Icon(L"");
                    invalidIcon = true;
                }
            }
        }
    }

    if (invalidBackground)
    {
        _warnings.Append(SettingsLoadWarnings::InvalidBackgroundImage);
    }

    if (invalidIcon)
    {
        _warnings.Append(SettingsLoadWarnings::InvalidIcon);
    }
}

// Method Description:
// - If there were any warnings we generated while parsing the user's
//   keybindings, add them to the list of warnings here. If there were warnings
//   generated in this way, we'll add a AtLeastOneKeybindingWarning, which will
//   act as a header for the other warnings
// - GH#3522
//   With variable args to keybindings, it's possible that a user
//   set a keybinding without all the required args for an action.
//   Display a warning if an action didn't have a required arg.
//   This will also catch other keybinding warnings, like from GH#4239.
// - TODO: GH#2548 ensure there's at least one key bound.
//   Display a warning if there's _NO_ keys bound to any actions.
//   That's highly irregular, and likely an indication of an error somehow.
// Arguments:
// - <none>
// Return Value:
// - <none>
void CascadiaSettings::_ValidateKeybindings() const
{
    const auto keybindingWarnings = _globals->KeybindingsWarnings();

    if (!keybindingWarnings.empty())
    {
        _warnings.Append(SettingsLoadWarnings::AtLeastOneKeybindingWarning);
        for (auto warning : keybindingWarnings)
        {
            _warnings.Append(warning);
        }
    }
}

// Method Description:
// - Ensures that every "setColorScheme" command has a valid "color scheme" set.
// Arguments:
// - <none>
// Return Value:
// - <none>
// - Appends a SettingsLoadWarnings::InvalidColorSchemeInCmd to our list of warnings if
//   we find any command with an invalid color scheme.
void CascadiaSettings::_ValidateColorSchemesInCommands() const
{
    bool foundInvalidScheme{ false };
    for (const auto& nameAndCmd : _globals->ActionMap().NameMap())
    {
        if (_HasInvalidColorScheme(nameAndCmd.Value()))
        {
            foundInvalidScheme = true;
            break;
        }
    }

    if (foundInvalidScheme)
    {
        _warnings.Append(SettingsLoadWarnings::InvalidColorSchemeInCmd);
    }
}

bool CascadiaSettings::_HasInvalidColorScheme(const Model::Command& command) const
{
    bool invalid{ false };
    if (command.HasNestedCommands())
    {
        for (const auto& nested : command.NestedCommands())
        {
            if (_HasInvalidColorScheme(nested.Value()))
            {
                invalid = true;
                break;
            }
        }
    }
    else if (const auto& actionAndArgs = command.ActionAndArgs())
    {
        if (const auto& realArgs = actionAndArgs.Args().try_as<Model::SetColorSchemeArgs>())
        {
            const auto cmdImpl{ winrt::get_self<Command>(command) };
            // no need to validate iterable commands on color schemes
            // they will be expanded to commands with a valid scheme name
            if (cmdImpl->IterateOn() != ExpandCommandType::ColorSchemes &&
                !_globals->ColorSchemes().HasKey(realArgs.SchemeName()))
            {
                invalid = true;
            }
        }
    }

    return invalid;
}
