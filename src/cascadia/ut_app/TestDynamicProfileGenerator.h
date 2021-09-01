/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- TestDynamicProfileGenerator.hpp

Abstract:
- This is a helper class for writing tests using dynamic profiles. Lets you
  easily set a arbitrary namespace and generation function for the profiles.

Author(s):
- Mike Griese - August 2019
--*/

#include "../TerminalSettingsModel/IDynamicProfileGenerator.h"

namespace TerminalAppUnitTests
{
    class TestDynamicProfileGenerator;
};

class TerminalAppUnitTests::TestDynamicProfileGenerator final :
    public winrt::Microsoft::Terminal::Settings::Model::IDynamicProfileGenerator
{
public:
    TestDynamicProfileGenerator(const std::wstring_view& ns, std::function<void(std::vector<winrt::Microsoft::Terminal::Settings::Model::Profile>&)> pfnGenerate) :
        _namespace{ ns },
        _pfnGenerate{ std::move(_pfnGenerate) },
    {
    }

    std::wstring_view GetNamespace() override { return _namespace; };

    void GenerateProfiles(std::vector<winrt::Microsoft::Terminal::Settings::Model::implementation::Profile>& profiles) override
    {
        if (pfnGenerate)
        {
            return pfnGenerate(profiles);
        }
    }

    std::wstring _namespace;
    std::function<void(std::vector<winrt::Microsoft::Terminal::Settings::Model::Profile>&)> _pfnGenerate;
};
