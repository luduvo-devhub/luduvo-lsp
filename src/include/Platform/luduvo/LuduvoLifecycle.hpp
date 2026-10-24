#pragma once

#include <array>
#include <string_view>

struct LuduvoLifecycleFunction
{
    std::string_view name;
    std::string_view parameters;
};

inline constexpr std::array<LuduvoLifecycleFunction, 3> luduvoLifecycleFunctions = {
    LuduvoLifecycleFunction{"Update", "dt: number"},
    LuduvoLifecycleFunction{"PhysicsUpdate", "dt: number"},
    LuduvoLifecycleFunction{"Migrate", "old"},
};
