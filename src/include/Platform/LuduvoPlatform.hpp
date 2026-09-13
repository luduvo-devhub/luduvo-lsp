#pragma once
#include "Platform/LSPPlatform.hpp"

class LuduvoPlatform : public LSPPlatform
{
public:
    using LSPPlatform::LSPPlatform;
    const char* getBuiltinDefinitions() const override;
};
