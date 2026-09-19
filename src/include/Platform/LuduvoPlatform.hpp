#pragma once
#include "Platform/LSPPlatform.hpp"

class LuduvoPlatform : public LSPPlatform
{
public:
    using LSPPlatform::LSPPlatform;
    const char* getBuiltinDefinitions() const override;
    const char* getBuiltinDocumentation() const override;
    bool isLintIgnored(const Luau::LintWarning& lint) const override;
};
