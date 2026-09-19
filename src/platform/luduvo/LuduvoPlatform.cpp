#include "Platform/LuduvoPlatform.hpp"
#include "LuduvoDefinitions.hpp"
#include "LuduvoDocumentation.hpp"

#include <array>

const char* LuduvoPlatform::getBuiltinDefinitions() const
{
    return LUDUVO_DEFINITIONS;
}

const char* LuduvoPlatform::getBuiltinDocumentation() const
{
    return LUDUVO_DOCUMENTATION;
}

bool LuduvoPlatform::isLintIgnored(const Luau::LintWarning& lint) const
{
    if (lint.code != Luau::LintWarning::Code_FunctionUnused)
        return false;

    static constexpr std::array lifecycleFunctions{"Update", "PhysicsUpdate", "Migrate"};
    for (const char* name : lifecycleFunctions)
    {
        if (lint.text == "Function '" + std::string(name) + "' is never used; prefix with '_' to silence")
            return true;
    }
    return false;
}
