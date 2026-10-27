#include "Platform/luduvo/context/LuduvoAutoDump.hpp"

#include "LuauFileUtils.hpp"
#include "Platform/luduvo/context/LuduvoWorldDump.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace luduvo
{

namespace
{

static bool renameAutosaveWithRetry(const std::filesystem::path& from, const std::filesystem::path& to, std::string* error)
{
    for (size_t attempt = 0; attempt < 10; ++attempt)
    {
        std::error_code filesystemError;
        std::filesystem::rename(from, to, filesystemError);
        if (!filesystemError)
            return true;
        if (attempt == 9)
        {
            if (error)
                *error = filesystemError.message();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

static bool inspectAutosaves(
    const std::filesystem::path& original, const std::filesystem::path& held, bool* originalExists, bool* heldExists, std::string* error)
{
    std::error_code filesystemError;
    *originalExists = std::filesystem::exists(original, filesystemError);
    if (filesystemError)
    {
        if (error)
            *error = "could not inspect the Luduvo autosave: " + filesystemError.message();
        return false;
    }
    *heldExists = std::filesystem::exists(held, filesystemError);
    if (filesystemError)
    {
        if (error)
            *error = "could not inspect the held Luduvo autosave: " + filesystemError.message();
        return false;
    }
    return true;
}

static std::optional<std::string> autosaveTimestamp(std::chrono::system_clock::time_point time)
{
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(time);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(time - seconds).count();
    const std::time_t epochSeconds = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &epochSeconds) != 0)
#else
    if (!gmtime_r(&epochSeconds, &utc))
#endif
        return std::nullopt;
    std::ostringstream formatted;
    formatted << std::put_time(&utc, "%Y%m%d-%H%M%S") << '-' << std::setfill('0') << std::setw(3) << milliseconds << 'Z';
    return formatted.str();
}

static bool restoreHeldAutosave(const std::filesystem::path& original, const std::filesystem::path& held, std::string* error)
{
    bool originalExists = false;
    bool heldExists = false;
    if (!inspectAutosaves(original, held, &originalExists, &heldExists, error))
        return false;
    if (!heldExists)
        return true;

    std::filesystem::path destination = original;
    if (originalExists)
    {
        // Preserved copies are never selected for automatic recovery.
        auto preservedAt = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
        for (;; preservedAt += std::chrono::milliseconds(1))
        {
            const auto timestamp = autosaveTimestamp(preservedAt);
            if (!timestamp)
            {
                if (error)
                    *error = "could not format the preserved autosave timestamp";
                return false;
            }
            destination = held;
            destination += "-" + *timestamp;
            std::error_code filesystemError;
            const bool occupied = std::filesystem::exists(destination, filesystemError);
            if (filesystemError)
            {
                if (error)
                    *error = "could not inspect the preserved autosave path: " + filesystemError.message();
                return false;
            }
            if (!occupied)
                break;
        }
    }
    std::string renameError;
    if (!renameAutosaveWithRetry(held, destination, &renameError))
    {
        if (error)
            *error = "could not restore or preserve the held Luduvo autosave: " + renameError;
        return false;
    }
    return true;
}

class AutosaveHold
{
    std::filesystem::path original;
    std::filesystem::path held;
    bool holding = false;

public:
    explicit AutosaveHold(const std::string& projectDirectory)
        : original(std::filesystem::u8path(projectDirectory) / "world.ldv.autosave")
        , held(std::filesystem::u8path(projectDirectory) / "world.ldv.autosave.luduvo-lsp-hostage")
    {
    }

    bool acquire(std::string* error)
    {
        if (!restoreHeldAutosave(original, held, error))
            return false;
        std::error_code filesystemError;
        const bool originalExists = std::filesystem::exists(original, filesystemError);
        if (filesystemError)
        {
            if (error)
                *error = "could not inspect the Luduvo autosave: " + filesystemError.message();
            return false;
        }
        if (!originalExists)
            return true;

        std::string renameError;
        if (!renameAutosaveWithRetry(original, held, &renameError))
        {
            if (error)
                *error = "could not temporarily hold the Luduvo autosave: " + renameError;
            return false;
        }
        holding = true;
        return true;
    }

    bool restore(std::string* error)
    {
        if (!holding)
            return true;
        if (!restoreHeldAutosave(original, held, error))
            return false;
        holding = false;
        return true;
    }

    ~AutosaveHold()
    {
        std::string ignored;
        restore(&ignored);
    }
};

#ifdef _WIN32
static std::string quoteWindowsArgument(const std::string& argument)
{
    std::string result = "\"";
    size_t backslashes = 0;
    for (char character : argument)
    {
        if (character == '\\')
            ++backslashes;
        else if (character == '"')
        {
            result.append(backslashes * 2 + 1, '\\');
            result.push_back('"');
            backslashes = 0;
        }
        else
        {
            result.append(backslashes, '\\');
            backslashes = 0;
            result.push_back(character);
        }
    }
    result.append(backslashes * 2, '\\');
    result.push_back('"');
    return result;
}

class ChildProcess
{
    HANDLE process = nullptr;
    HANDLE job = nullptr;

public:
    bool start(const std::vector<std::string>& arguments, std::string* error)
    {
        std::string commandLine;
        for (const std::string& argument : arguments)
        {
            if (!commandLine.empty())
                commandLine.push_back(' ');
            commandLine += quoteWindowsArgument(argument);
        }
        std::wstring wideCommand = Luau::FileUtils::fromUtf8(commandLine);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION information{};
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !CreateProcessW(nullptr, wideCommand.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                &information) ||
            !AssignProcessToJobObject(job, information.hProcess))
        {
            if (information.hThread)
                CloseHandle(information.hThread);
            if (information.hProcess)
                TerminateProcess(information.hProcess, 1), CloseHandle(information.hProcess);
            if (job)
                CloseHandle(job), job = nullptr;
            if (error)
                *error = "could not start the disposable Luduvo Editor process (Windows error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        process = information.hProcess;
        ResumeThread(information.hThread);
        CloseHandle(information.hThread);
        return true;
    }

    bool running() const
    {
        return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    }

    void stop()
    {
        if (job)
            TerminateJobObject(job, 0);
        if (process)
            WaitForSingleObject(process, 5000);
    }

    ~ChildProcess()
    {
        stop();
        if (process)
            CloseHandle(process);
        if (job)
            CloseHandle(job);
    }
};
#else
class ChildProcess
{
    pid_t process = -1;
    pid_t processGroup = -1;

public:
    bool start(const std::vector<std::string>& arguments, std::string* error)
    {
        process = fork();
        if (process < 0)
        {
            if (error)
                *error = "could not fork the disposable Luduvo Editor process";
            return false;
        }
        if (process == 0)
        {
            if (setsid() < 0)
                _exit(127);
            const int nullDescriptor = open("/dev/null", O_RDWR);
            if (nullDescriptor >= 0)
            {
                dup2(nullDescriptor, STDIN_FILENO);
                dup2(nullDescriptor, STDOUT_FILENO);
                dup2(nullDescriptor, STDERR_FILENO);
                if (nullDescriptor > STDERR_FILENO)
                    close(nullDescriptor);
            }
            std::vector<char*> values;
            for (const std::string& argument : arguments)
                values.push_back(const_cast<char*>(argument.c_str()));
            values.push_back(nullptr);
            execv(arguments.front().c_str(), values.data());
            _exit(127);
        }
        processGroup = process;
        return true;
    }

    bool running()
    {
        if (process < 0)
            return false;
        int status = 0;
        const pid_t result = waitpid(process, &status, WNOHANG);
        if (result == 0)
            return true;
        if (result == process)
            process = -1;
        return false;
    }

    void stop()
    {
        if (processGroup < 0)
            return;
        kill(-processGroup, SIGTERM);
        for (size_t attempt = 0; attempt < 20; ++attempt)
        {
            if (!running())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        kill(-processGroup, SIGKILL);
        if (process >= 0)
            waitpid(process, nullptr, 0);
        process = -1;
        processGroup = -1;
    }

    ~ChildProcess()
    {
        stop();
    }
};
#endif

} // namespace

AutoDumpResult runAutomaticWorldDump(const AutoDumpPlan& plan, const std::atomic<bool>* cancelled)
{
    if (plan.projectDirectory.empty())
        return {false, "the selected project has no known directory"};
    if (plan.editorPath.empty() || !std::filesystem::exists(std::filesystem::u8path(plan.editorPath)))
        return {false, "LuduvoEditor was not found at " + plan.editorPath};

    AutosaveHold autosave(plan.projectDirectory);
    std::string error;
    if (!autosave.acquire(&error))
        return {false, error};

    std::error_code filesystemError;
    const std::filesystem::path output = std::filesystem::u8path(plan.outputPath);
    std::filesystem::create_directories(output.parent_path(), filesystemError);
    if (filesystemError)
        return {false, "could not create the world-dump directory: " + filesystemError.message()};
    std::filesystem::remove(output, filesystemError);
    if (filesystemError)
        return {false, "could not replace the previous world dump: " + filesystemError.message()};

    ChildProcess child;
    const std::vector<std::string> arguments = {
        plan.editorPath, plan.projectDirectory, "-platform", "offscreen", "--exec", "world.dump " + plan.outputPath};
    if (!child.start(arguments, &error))
        return {false, error};

    const auto deadline = std::chrono::steady_clock::now() + plan.timeout;
    std::string lastParseError;
    uintmax_t lastSize = uintmax_t(-1);
    size_t stableValidReads = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (cancelled && *cancelled)
        {
            child.stop();
            if (!autosave.restore(&error))
                return {false, error};
            return {false, "automatic world dump was cancelled"};
        }
        if (std::filesystem::exists(output))
        {
            const WorldDumpParseResult parsed = parseWorldDumpFile(plan.outputPath);
            if (parsed.snapshot)
            {
                const uintmax_t size = std::filesystem::file_size(output, filesystemError);
                if (!filesystemError && size == lastSize)
                    ++stableValidReads;
                else
                    stableValidReads = 0;
                lastSize = size;
                if (stableValidReads >= 2)
                {
                    // Give Studio time to flush the matching command-log acknowledgement before ending the disposable process.
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    child.stop();
                    if (!autosave.restore(&error))
                        return {false, error};
                    return {true, {}, std::move(parsed.snapshot)};
                }
            }
            else
            {
                stableValidReads = 0;
                lastParseError = parsed.error.value_or("the dump was incomplete");
            }
        }
        if (!child.running())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    child.stop();
    if (!autosave.restore(&error))
        return {false, error};
    if (!lastParseError.empty())
        return {false, "LuduvoEditor did not produce a complete world dump: " + lastParseError};
    return {false, "LuduvoEditor exited or timed out before producing a world dump"};
}

AutoDumpResult recoverAutomaticWorldDumpAutosave(const std::string& projectDirectory)
{
    const std::filesystem::path original = std::filesystem::u8path(projectDirectory) / "world.ldv.autosave";
    const std::filesystem::path held = std::filesystem::u8path(projectDirectory) / "world.ldv.autosave.luduvo-lsp-hostage";
    std::string error;
    if (!restoreHeldAutosave(original, held, &error))
        return {false, error};
    return {true, {}, std::nullopt};
}

} // namespace luduvo
