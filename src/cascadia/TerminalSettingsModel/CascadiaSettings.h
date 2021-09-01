/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- CascadiaSettings.h

Abstract:
- This class acts as the container for all app settings. It's composed of two
        parts: Globals, which are app-wide settings, and Profiles, which contain
        a set of settings that apply to a single instance of the terminal.
  Also contains the logic for serializing and deserializing this object.

Author(s):
- Mike Griese - March 2019

--*/
#pragma once

#include "CascadiaSettings.g.h"

#include "GlobalAppSettings.h"

#include "Profile.h"
#include "ColorScheme.h"

// fwdecl unittest classes
namespace SettingsModelLocalTests
{
    class SerializationTests;
    class DeserializationTests;
    class ProfileTests;
    class ColorSchemeTests;
    class KeyBindingsTests;
};
namespace TerminalAppUnitTests
{
    class DynamicProfileTests;
    class JsonTests;
};

namespace Microsoft::Terminal::Settings::Model
{
    class SettingsTypedDeserializationException;
};

class Microsoft::Terminal::Settings::Model::SettingsTypedDeserializationException final : public std::runtime_error
{
public:
    SettingsTypedDeserializationException(const std::string_view description) :
        runtime_error(description.data()) {}
};

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    struct ParsedSettings
    {
        ParsedSettings() noexcept = default;
        explicit ParsedSettings(OriginTag origin, const std::string_view& content);

        [[nodiscard]] winrt::Windows::Foundation::Collections::IObservableVector<Model::Profile> fuckyou1() const
        {
            std::vector<Model::Profile> vec;
            vec.reserve(profiles.size());

            for (const auto& p : profiles)
            {
                vec.emplace_back(*p);
            }

            return winrt::single_threaded_observable_vector(std::move(vec));
        }

        winrt::com_ptr<implementation::GlobalAppSettings> globals;
        winrt::com_ptr<implementation::Profile> profileDefaults;
        std::vector<winrt::com_ptr<implementation::Profile>> profiles;
        std::unordered_map<winrt::guid, winrt::com_ptr<implementation::Profile>> profilesByGuid;
    };

    struct CascadiaSettings : CascadiaSettingsT<CascadiaSettings>
    {
    public:
        static Model::CascadiaSettings LoadDefaults();
        static Model::CascadiaSettings LoadAll();
        static Model::CascadiaSettings LoadUniversal();

        static winrt::hstring SettingsPath();
        static winrt::hstring DefaultSettingsPath();
        static winrt::hstring ApplicationDisplayName();
        static winrt::hstring ApplicationVersion();

        CascadiaSettings() noexcept = default;
        CascadiaSettings(const std::string_view& defaultJson, const std::string_view& userJson = {});

        // user settings
        [[nodiscard]] Model::CascadiaSettings Copy() const;
        [[nodiscard]] Model::GlobalAppSettings GlobalSettings() const;
        [[nodiscard]] Windows::Foundation::Collections::IObservableVector<Model::Profile> AllProfiles() const noexcept;
        [[nodiscard]] Windows::Foundation::Collections::IObservableVector<Model::Profile> ActiveProfiles() const noexcept;
        [[nodiscard]] Model::ActionMap ActionMap() const noexcept;
        void WriteSettingsToDisk() const;
        [[nodiscard]] Json::Value ToJson() const;
        [[nodiscard]] Model::Profile ProfileDefaults() const;
        [[nodiscard]] Model::Profile CreateNewProfile();
        [[nodiscard]] Model::Profile FindProfile(const winrt::guid& guid) const noexcept;
        [[nodiscard]] Model::ColorScheme GetColorSchemeForProfile(const Model::Profile& profile) const;
        void UpdateColorSchemeReferences(const winrt::hstring& oldName, const winrt::hstring& newName);
        [[nodiscard]] Model::Profile GetProfileForArgs(const Model::NewTerminalArgs& newTerminalArgs) const;
        [[nodiscard]] Model::Profile DuplicateProfile(const Model::Profile& source);

        // load errors
        [[nodiscard]] Windows::Foundation::Collections::IVectorView<Model::SettingsLoadWarnings> Warnings() const;
        [[nodiscard]] Windows::Foundation::IReference<Model::SettingsLoadErrors> GetLoadingError() const;
        [[nodiscard]] winrt::hstring GetSerializationErrorMessage() const;

        // defterm
        static bool IsDefaultTerminalAvailable() noexcept;
        [[nodiscard]] Windows::Foundation::Collections::IVectorView<Model::DefaultTerminal> DefaultTerminals() const noexcept;
        [[nodiscard]] Model::DefaultTerminal CurrentDefaultTerminal() const noexcept;
        void CurrentDefaultTerminal(const Model::DefaultTerminal& terminal);

    private:
        static const std::filesystem::path& _SettingsPath();
        
        bool _load(const std::wstring_view& defaultJson, const std::wstring_view& userJson = {});
        bool _load(const std::string_view& defaultJson, const std::string_view& userJson = {});

        [[nodiscard]] winrt::com_ptr<Profile> _CreateNewProfile(const std::wstring_view& name) const;
        [[nodiscard]] std::optional<winrt::guid> _GetProfileGuidByName(const winrt::hstring& name) const;
        [[nodiscard]] std::optional<winrt::guid> _GetProfileGuidByIndex(std::optional<int> index) const;

        void _FinalizeSettings();
        void _ResolveDefaultProfile() const;
        void _UpdateActiveProfiles();

        void _ValidateSettings();
        void _ValidateProfilesExist() const;
        void _ValidateDefaultProfileExists();
        void _ValidateNoDuplicateProfiles() const;
        void _ValidateAllSchemesExist();
        void _ValidateMediaResources();
        void _ValidateKeybindings() const;
        void _ValidateColorSchemesInCommands() const;
        bool _HasInvalidColorScheme(const Model::Command& command) const;

        // user settings
        winrt::com_ptr<implementation::GlobalAppSettings> _globals{ winrt::make_self<implementation::GlobalAppSettings>() };
        Windows::Foundation::Collections::IObservableVector<Model::Profile> _allProfiles{ winrt::single_threaded_observable_vector<Model::Profile>() };
        Windows::Foundation::Collections::IObservableVector<Model::Profile> _activeProfiles{ winrt::single_threaded_observable_vector<Model::Profile>() };
        winrt::com_ptr<implementation::Profile> _userDefaultProfileSettings;

        // load errors
        Windows::Foundation::Collections::IVector<Model::SettingsLoadWarnings> _warnings{ winrt::single_threaded_vector<SettingsLoadWarnings>() };
        Windows::Foundation::IReference<Model::SettingsLoadErrors> _loadError;
        winrt::hstring _deserializationErrorMessage;

        // defterm
        Model::DefaultTerminal _currentDefaultTerminal{ nullptr };

        friend class SettingsModelLocalTests::SerializationTests;
        friend class SettingsModelLocalTests::DeserializationTests;
        friend class SettingsModelLocalTests::ProfileTests;
        friend class SettingsModelLocalTests::ColorSchemeTests;
        friend class SettingsModelLocalTests::KeyBindingsTests;
        friend class TerminalAppUnitTests::DynamicProfileTests;
        friend class TerminalAppUnitTests::JsonTests;
    };
}

namespace winrt::Microsoft::Terminal::Settings::Model::factory_implementation
{
    BASIC_FACTORY(CascadiaSettings);
}
