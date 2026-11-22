#include "Platform/luduvo/context/LuduvoProcessIdentity.hpp"
#include "LSP/AtomicFile.hpp"

#include "nlohmann/json.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <limits>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#include <unistd.h>
#elif defined(__linux__)
#include <signal.h>
#include <unistd.h>
#else
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#endif

namespace luduvo
{

using json = nlohmann::json;

#ifdef _WIN32
static std::optional<uint64_t> processStartToken(uint64_t processId)
{
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, DWORD(processId));
    if (!process)
        return std::nullopt;
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    const bool read = GetProcessTimes(process, &creation, &exit, &kernel, &user);
    // Another process can keep a handle open after this process has exited.
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    if (!read || !running)
        return std::nullopt;
    return (uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}
#elif defined(__APPLE__)
static std::optional<uint64_t> processStartToken(uint64_t processId)
{
    struct proc_bsdinfo info{};
    if (proc_pidinfo(int(processId), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info))
        return std::nullopt;
    return uint64_t(info.pbi_start_tvsec) * 1000000 + uint64_t(info.pbi_start_tvusec);
}
#elif defined(__linux__)
static std::optional<uint64_t> processStartToken(uint64_t processId)
{
    std::ifstream input("/proc/" + std::to_string(processId) + "/stat");
    std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const size_t commandEnd = source.rfind(')');
    if ((!input && source.empty()) || commandEnd == std::string::npos)
        return std::nullopt;
    std::istringstream fields(source.substr(commandEnd + 2));
    std::string value;
    for (size_t index = 0; index <= 19; ++index)
        if (!(fields >> value))
            return std::nullopt;
    try
    {
        return std::stoull(value);
    }
    catch (...)
    {
        return std::nullopt;
    }
}
#else
static std::optional<uint64_t> processStartToken(uint64_t processId)
{
    errno = 0;
    return kill(pid_t(processId), 0) == 0 || errno == EPERM ? std::optional<uint64_t>(0) : std::nullopt;
}
#endif

std::optional<ProcessIdentity> currentProcessIdentity()
{
#ifdef _WIN32
    const uint64_t processId = GetCurrentProcessId();
#else
    const uint64_t processId = uint64_t(getpid());
#endif
    return processIdentityForPid(processId);
}

std::optional<ProcessIdentity> processIdentityForPid(uint64_t processId)
{
    if (!processId || processId > uint64_t(std::numeric_limits<int>::max()))
        return std::nullopt;
    const auto token = processStartToken(processId);
    if (!token)
        return std::nullopt;
    return ProcessIdentity{processId, *token};
}

std::optional<uint64_t> processStartUnixMs(const ProcessIdentity& identity)
{
#ifdef _WIN32
    constexpr uint64_t unixEpoch = 116444736000000000ULL;
    if (identity.startToken >= unixEpoch)
        return (identity.startToken - unixEpoch) / 10000;
#elif defined(__APPLE__)
    return identity.startToken / 1000;
#elif defined(__linux__)
    const auto ticksPerSecond = sysconf(_SC_CLK_TCK);
    if (ticksPerSecond <= 0)
        return std::nullopt;
    std::ifstream input("/proc/stat");
    for (std::string line; std::getline(input, line);)
    {
        if (line.compare(0, 6, "btime ") != 0)
            continue;
        uint64_t bootTime = 0;
        std::istringstream value(line.substr(6));
        if (value >> bootTime)
            return bootTime * 1000 + identity.startToken * 1000 / uint64_t(ticksPerSecond);
        break;
    }
#else
    (void)identity;
#endif
    return std::nullopt;
}

bool isProcessAlive(const ProcessIdentity& identity)
{
    const auto token = processStartToken(identity.processId);
    return token && *token == identity.startToken;
}

std::optional<ProcessIdentity> readProcessIdentity(const std::string& path)
{
    const auto source = readSharedFile(path);
    if (!source)
        return std::nullopt;
    try
    {
        const json value = json::parse(*source);
        if (!value.contains("processId") || !value["processId"].is_number_unsigned() || !value.contains("startToken") ||
            !value["startToken"].is_number_unsigned())
            return std::nullopt;
        return ProcessIdentity{value["processId"].get<uint64_t>(), value["startToken"].get<uint64_t>()};
    }
    catch (const json::exception&)
    {
        return std::nullopt;
    }
}

bool writeProcessIdentity(const std::string& path, const ProcessIdentity& identity, std::string* error)
{
    const std::filesystem::path destination = std::filesystem::u8path(path);
    std::error_code filesystemError;
    std::filesystem::create_directories(destination.parent_path(), filesystemError);
    if (filesystemError)
    {
        if (error)
            *error = filesystemError.message();
        return false;
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output << json{{"processId", identity.processId}, {"startToken", identity.startToken}}.dump() << '\n';
    if (!output)
    {
        if (error)
            *error = "could not write process identity " + path;
        return false;
    }
    return true;
}

size_t removeDeadProcessRegistrations(const std::string& directory)
{
    std::error_code error;
    const std::filesystem::path registrations = std::filesystem::u8path(directory);
    if (!std::filesystem::is_directory(registrations, error))
        return 0;

    size_t liveProcesses = 0;
    for (std::filesystem::directory_iterator iterator(registrations, error), end; iterator != end && !error; iterator.increment(error))
    {
        if (!iterator->is_regular_file(error) || error)
            continue;
        const auto identity = readProcessIdentity(iterator->path().u8string());
        if (identity && isProcessAlive(*identity))
            ++liveProcesses;
        else
        {
            error.clear();
            std::filesystem::remove(iterator->path(), error);
        }
    }
    return liveProcesses;
}

} // namespace luduvo
