#pragma once

#include <map>
#include "nlohmann/json.hpp"
#include <stdexcept>
#include <optional>
#include <string>
#include <string_view>

namespace settings
{
// A leaf override wins over its parents. Global defaults are supplied by the caller.
template<typename T>
std::optional<T> findOverride(const std::map<std::string, T>& overrides, std::string_view category)
{
    while (!category.empty())
    {
        if (const auto found = overrides.find(std::string(category)); found != overrides.end())
            return found->second;
        const auto dot = category.rfind('.');
        if (dot == std::string_view::npos)
            break;
        category = category.substr(0, dot);
    }
    return std::nullopt;
}
template<typename T>
struct CategoryConfiguration
{
    std::optional<T> defaultValue;
    std::map<std::string, T> categories;

    T resolve(std::string_view id, T fallback) const
    {
        return findOverride(categories, id).value_or(defaultValue.value_or(fallback));
    }
    bool operator==(const CategoryConfiguration& other) const
    {
        return defaultValue == other.defaultValue && categories == other.categories;
    }
    bool operator!=(const CategoryConfiguration& other) const
    {
        return !(*this == other);
    }
};
template<typename T>
void to_json(nlohmann::json& json, const CategoryConfiguration<T>& configuration)
{
    json = {{"categories", configuration.categories}};
    if (configuration.defaultValue)
        json["default"] = *configuration.defaultValue;
}
template<typename T>
void from_json(const nlohmann::json& json, CategoryConfiguration<T>& configuration)
{
    if (!json.is_object())
        throw std::invalid_argument("category settings must be an object containing default and categories");
    CategoryConfiguration<T> result;
    for (const auto& entry : json.items())
        if (entry.key() != "default" && entry.key() != "categories")
            throw std::invalid_argument("unknown category setting '" + entry.key() + "'; use default and categories");
    if (json.contains("default"))
        result.defaultValue = json.at("default").template get<T>();
    if (json.contains("categories"))
        result.categories = json.at("categories").template get<std::map<std::string, T>>();
    configuration = std::move(result);
}
} // namespace settings
