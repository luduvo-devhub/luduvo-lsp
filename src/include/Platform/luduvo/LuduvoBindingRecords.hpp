#pragma once
#include <vector>

struct LuduvoBuiltinFieldBinding
{
    const char* symbol;
    const char* target;
    const char* group;
};
extern const std::vector<LuduvoBuiltinFieldBinding> luduvoBuiltinFieldBindings;

#include "Platform/ColorPresentation.hpp"

#include <string>
#include <string_view>
#include <vector>

struct LuduvoBuiltinResourceBinding
{
    std::string_view symbol;
    std::string_view parameter;
    std::string_view kind;
    bool variadic = false;
    bool pluralResourceNoun = false;
};

struct LuduvoColorBinding
{
    std::string symbol;
    std::vector<std::string> parameters;
    ColorPresentationEncoding encoding = ColorPresentationEncoding::Rgb;
};

extern const std::vector<LuduvoBuiltinResourceBinding> luduvoBuiltinResourceBindings;
extern const std::vector<LuduvoColorBinding> luduvoBuiltinColorBindings;
