#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace luduvo
{

struct ProcessIdentity
{
    uint64_t processId = 0;
    uint64_t startToken = 0;
};

std::optional<ProcessIdentity> currentProcessIdentity();
bool isProcessAlive(const ProcessIdentity& identity);
std::optional<ProcessIdentity> readProcessIdentity(const std::string& path);
bool writeProcessIdentity(const std::string& path, const ProcessIdentity& identity, std::string* error = nullptr);
size_t removeDeadProcessRegistrations(const std::string& directory);

} // namespace luduvo
