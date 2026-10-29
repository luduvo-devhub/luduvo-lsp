#pragma once
#include <optional>
#include <string>

// Readers permit atomic replacement while open on Windows.
std::optional<std::string> readSharedFile(const std::string& path, std::string* error = nullptr);
bool writeFileAtomically(const std::string& path, const std::string& contents, std::string* error = nullptr);
