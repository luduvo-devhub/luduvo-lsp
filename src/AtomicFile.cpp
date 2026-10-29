#include "LSP/AtomicFile.hpp"
#include "LuauFileUtils.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <cerrno>
#include <cstring>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <unistd.h>
#endif

std::optional<std::string> readSharedFile(const std::string& path, std::string* error)
{
    std::error_code typeError;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(path), typeError))
    {
        if (error)
            *error = typeError ? typeError.message() : "not a regular file";
        return std::nullopt;
    }
#ifdef _WIN32
    const auto wide = std::filesystem::u8path(path);
    HANDLE file = CreateFileW(
        wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        if (error)
            *error = std::error_code(int(GetLastError()), std::system_category()).message();
        return std::nullopt;
    }
    std::string contents;
    char buffer[8192];
    DWORD count = 0;
    bool success = true;
    while ((success = ReadFile(file, buffer, sizeof(buffer), &count, nullptr)) && count)
        contents.append(buffer, count);
    if (!success && error)
        *error = std::error_code(int(GetLastError()), std::system_category()).message();
    CloseHandle(file);
    return success ? std::optional<std::string>(std::move(contents)) : std::nullopt;
#else
    auto contents = Luau::FileUtils::readFile(path);
    if (!contents && error)
        *error = std::strerror(errno);
    return contents;
#endif
}

bool writeFileAtomically(const std::string& path, const std::string& contents, std::string* errorMessage)
{
    const auto destination = std::filesystem::u8path(path);
    std::error_code error;
    if (!destination.parent_path().empty())
        std::filesystem::create_directories(destination.parent_path(), error);
    auto fail = [&](const std::string& message)
    {
        if (errorMessage)
            *errorMessage = message;
        return false;
    };
    if (error)
        return fail("could not create " + destination.parent_path().generic_u8string() + ": " + error.message());
    static std::atomic<uint64_t> sequence{0};
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    const auto process = getpid();
#endif
    auto temporary = destination;
    temporary += ".tmp." + std::to_string(process) + "." + std::to_string(sequence++);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(contents.data(), std::streamsize(contents.size()));
        output.close();
        if (!output)
        {
            std::filesystem::remove(temporary, error);
            return fail("could not write " + temporary.generic_u8string());
        }
    }
#ifdef _WIN32
    // Antivirus and old readers might deny replacement so
    // failed writes must leave the previous state intact
    for (size_t attempt = 0; attempt < 21; ++attempt)
    {
        if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        const DWORD code = GetLastError();
        error = std::error_code(int(code), std::system_category());
        if (attempt == 20 || (code != ERROR_SHARING_VIOLATION && code != ERROR_ACCESS_DENIED && code != ERROR_LOCK_VIOLATION))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#else
    std::filesystem::rename(temporary, destination, error);
    if (!error)
        return true;
#endif
    const std::string message = "could not replace " + destination.generic_u8string() + ": " + error.message();
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return fail(message);
}
