#include "Platform/luduvo/context/LuduvoAutoDump.hpp"

#include "LuauFileUtils.hpp"
#include "LSP/Logs.hpp"
#include "LSP/AtomicFile.hpp"
#include <mutex>
#include <cerrno>
#include <cstring>
#include "Platform/luduvo/context/LuduvoWorldDump.hpp"

#include <algorithm>
#include <charconv>
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
    logging::write(logging::Level::Debug, "helper.autodump.autosaves", "Recovered autosave to " + destination.generic_u8string());
    return true;
}

class TemporaryDumpProject
{
    std::filesystem::path root;

public:
    ~TemporaryDumpProject()
    {
        if (!root.empty())
        {
            std::error_code error;
            std::filesystem::remove_all(root, error);
            if (error)
                logging::write(logging::Level::Warning, "helper.autodump",
                    "Could not remove disposable project " + root.generic_u8string() + ": " + error.message());
        }
    }
    std::string directory() const
    {
        return root.generic_u8string();
    }
    bool prepare(WorldDumpSource& source, std::string* error)
    {
        std::error_code ec;
        const auto input = std::filesystem::u8path(source.path);
        std::optional<std::string> bytes;
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const auto before = std::filesystem::last_write_time(input, ec);
            if (ec)
                break;
            bytes = readSharedFile(source.path, error);
            const auto after = std::filesystem::last_write_time(input, ec);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto stable = std::filesystem::last_write_time(input, ec);
            if (bytes && !ec && before == after && after == stable && bytes->size() == std::filesystem::file_size(input, ec) && !ec)
            {
                source.modifiedAtUnixMs = std::chrono::time_point_cast<std::chrono::milliseconds>(
                    stable - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now())
                                              .time_since_epoch()
                                              .count();
                break;
            }
            bytes.reset();
        }
        if (!bytes || ec)
        {
            *error = "could not read a stable world file. Try again after Studio finishes writing";
            return false;
        }
        static std::atomic<uint64_t> next{0};
        const auto temporary = std::filesystem::temp_directory_path(ec);
        if (ec)
        {
            *error = ec.message();
            return false;
        }
        for (size_t attempt = 0; attempt < 100; ++attempt)
        {
            auto candidate = temporary / ("luduvo-context-dump-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" +
                                             std::to_string(next++));
            if (std::filesystem::create_directory(candidate, ec))
            {
                root = std::move(candidate);
                break;
            }
            if (ec)
            {
                *error = ec.message();
                return false;
            }
        }
        if (root.empty())
        {
            *error = "could not reserve a disposable project directory";
            return false;
        }
        std::filesystem::permissions(root, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
        if (ec)
        {
            *error = "could not restrict disposable project permissions: " + ec.message();
            return false;
        }
        return writeFileAtomically((root / "world.ldv").generic_u8string(), *bytes, error);
    }
    std::string worldPath() const
    {
        return (root / "world.ldv").generic_u8string();
    }
    bool copyProjectFiles(const std::string& directory, std::string* error, const std::atomic<bool>* cancelled)
    {
        const auto project = std::filesystem::u8path(directory);
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(project, ec), end; it != end && !ec; it.increment(ec))
        {
            if (cancelled && *cancelled)
            {
                *error = "automatic world dump was cancelled";
                return false;
            }
            const auto name = it->path().filename().generic_u8string();
            // A configured system temp directory may itself be inside the project.
            if (it->is_directory(ec) && std::filesystem::equivalent(it->path(), root, ec))
            {
                it.disable_recursion_pending();
                continue;
            }
            if (ec)
                break;
            if (it->is_symlink(ec))
            {
                *error = "cannot copy a project containing symbolic links: " + it->path().generic_u8string();
                return false;
            }
            if (ec)
                break;
            // The copy should open exactly the selected world, without Studio's lock or journal replay.
            const bool studioState = it.depth() == 0 && (name == ".studio.lock" || name == ".journal" || name == ".backups" || name == ".forks");
            if (studioState || name == ".git" || name == ".zed" || name == ".vscode")
            {
                if (it->is_directory(ec))
                    it.disable_recursion_pending();
                continue;
            }
            if (name == "world.ldv" || name.find("world.ldv.autosave") == 0 || it->path().extension() == ".log")
                continue;
            const auto target = root / it->path().lexically_relative(project);
            if (it->is_directory(ec))
                std::filesystem::create_directories(target, ec);
            else if (it->is_regular_file(ec))
            {
                std::filesystem::copy_file(it->path(), target, std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec)
                    std::filesystem::permissions(
                        target, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::add, ec);
            }
            if (ec)
                break;
        }
        if (ec)
        {
            *error = "could not copy disposable project: " + ec.message();
            return false;
        }
        return true;
    }
};

// Drain child pipes continuously. Keep a short diagnostic tail or a bounded JSON document.
class ProcessOutput
{
    std::thread reader;
    std::mutex mutex;
    std::string contents;
    bool overflow = false;
    static constexpr size_t tailLimit = 64 * 1024;
    static constexpr size_t documentLimit = 64 * 1024 * 1024;

public:
#ifdef _WIN32
    HANDLE input = INVALID_HANDLE_VALUE;
    HANDLE output = INVALID_HANDLE_VALUE;
#else
    int input = -1;
    int output = -1;
#endif
    bool open(std::string* error)
    {
#ifdef _WIN32
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        if (!CreatePipe(&input, &output, &security, 0) || !SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0))
#else
        int descriptors[2];
        if (pipe(descriptors) != 0)
#endif
        {
            if (error)
                *error = "could not create dump process output pipe";
            return false;
        }
#ifndef _WIN32
        input = descriptors[0];
        output = descriptors[1];
        fcntl(input, F_SETFD, FD_CLOEXEC);
        fcntl(output, F_SETFD, FD_CLOEXEC);
#endif
        return true;
    }
    void closeWriter()
    {
#ifdef _WIN32
        if (output != INVALID_HANDLE_VALUE)
            CloseHandle(output);
        output = INVALID_HANDLE_VALUE;
#else
        if (output >= 0)
            close(output);
        output = -1;
#endif
    }
    void startReader(bool document = false)
    {
        closeWriter();
        reader = std::thread(
            [this, document]
            {
                char buffer[4096];
                for (;;)
                {
#ifdef _WIN32
                    DWORD count = 0;
                    if (!ReadFile(input, buffer, sizeof(buffer), &count, nullptr) || !count)
                        break;
#else
                    const auto count = read(input, buffer, sizeof(buffer));
                    if (count < 0 && errno == EINTR)
                        continue;
                    if (count <= 0)
                        break;
#endif
                    std::lock_guard guard(mutex);
                    if (document && (overflow || contents.size() + size_t(count) > documentLimit))
                    {
                        overflow = true;
                        continue;
                    }
                    contents.append(buffer, size_t(count));
                    if (!document && contents.size() > tailLimit)
                        contents.erase(0, contents.size() - tailLimit);
                }
            });
    }
    void finish()
    {
        closeWriter();
        if (reader.joinable())
            reader.join();
    }
    std::string text()
    {
        std::lock_guard guard(mutex);
        return nlohmann::json::parse(nlohmann::json(contents).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)).get<std::string>();
    }
    std::string takeDocument()
    {
        std::lock_guard guard(mutex);
        return overflow ? std::string{} : std::move(contents);
    }
    ~ProcessOutput()
    {
        closeWriter();
        if (reader.joinable())
            reader.join();
#ifdef _WIN32
        if (input != INVALID_HANDLE_VALUE)
            CloseHandle(input);
#else
        if (input >= 0)
            close(input);
#endif
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
    ProcessOutput output;
    ProcessOutput document;

public:
    bool start(const std::vector<std::string>& arguments, std::string* error, bool captureDocument = false)
    {
        if (!output.open(error) || (captureDocument && !document.open(error)))
            return false;
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
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        const HANDLE nullInput =
            CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nullInput;
        startup.hStdOutput = captureDocument ? document.output : output.output;
        startup.hStdError = output.output;
        PROCESS_INFORMATION information{};
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !CreateProcessW(
                nullptr, wideCommand.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information) ||
            !AssignProcessToJobObject(job, information.hProcess))
        {
            const DWORD launchError = GetLastError();
            if (information.hThread)
                CloseHandle(information.hThread);
            if (information.hProcess)
                TerminateProcess(information.hProcess, 1), CloseHandle(information.hProcess);
            if (job)
                CloseHandle(job), job = nullptr;
            if (nullInput != INVALID_HANDLE_VALUE)
                CloseHandle(nullInput);
            output.closeWriter();
            if (error)
                *error = "could not start the disposable Luduvo Editor process (Windows error " + std::to_string(launchError) + ")";
            return false;
        }
        if (nullInput != INVALID_HANDLE_VALUE)
            CloseHandle(nullInput);
        output.startReader();
        if (captureDocument)
            document.startReader(true);
        process = information.hProcess;
        logging::write(logging::Level::Debug, "helper.autodump.process", "Started dump process PID " + std::to_string(information.dwProcessId));
        ResumeThread(information.hThread);
        CloseHandle(information.hThread);
        return true;
    }

    std::string takeDocument()
    {
        return document.takeDocument();
    }
    std::string capturedOutput()
    {
        return output.text();
    }
    std::string exitDescription() const
    {
        DWORD code = 0;
        return process && GetExitCodeProcess(process, &code) ? "exit code " + std::to_string(code) : "unknown exit status";
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
        output.finish();
        document.finish();
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
    ProcessOutput output;
    ProcessOutput document;
    std::string exitStatus = "unknown exit status";

public:
    bool start(const std::vector<std::string>& arguments, std::string* error, bool captureDocument = false)
    {
        if (!output.open(error) || (captureDocument && !document.open(error)))
            return false;
        process = fork();
        if (process < 0)
        {
            if (error)
                *error = "could not fork the disposable Luduvo Editor process: " + std::string(std::strerror(errno));
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

                if (nullDescriptor > STDERR_FILENO)
                    close(nullDescriptor);
            }
            dup2(captureDocument ? document.output : output.output, STDOUT_FILENO);
            dup2(output.output, STDERR_FILENO);
            close(output.input);
            close(output.output);
            std::vector<char*> values;
            for (const std::string& argument : arguments)
                values.push_back(const_cast<char*>(argument.c_str()));
            values.push_back(nullptr);
            execv(arguments.front().c_str(), values.data());
            const int code = errno;
            const char* description = std::strerror(code);
            // Intentionally unused since exec failure is best effort before the child exits
            [[maybe_unused]] const auto _ = write(STDERR_FILENO, description, std::strlen(description));
            _exit(127);
        }
        processGroup = process;
        logging::write(logging::Level::Debug, "helper.autodump.process", "Started dump process PID " + std::to_string(process));
        output.startReader();
        if (captureDocument)
            document.startReader(true);
        return true;
    }

    std::string takeDocument()
    {
        return document.takeDocument();
    }
    std::string capturedOutput()
    {
        return output.text();
    }
    std::string exitDescription() const
    {
        return exitStatus;
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
        {
            exitStatus = WIFEXITED(status)     ? "exit code " + std::to_string(WEXITSTATUS(status))
                         : WIFSIGNALED(status) ? "signal " + std::to_string(WTERMSIG(status))
                                               : "unknown exit status";
            process = -1;
        }
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
        output.finish();
        document.finish();
    }

    ~ChildProcess()
    {
        stop();
    }
};
#endif

} // namespace

std::string sceneDumperPath(const std::string& editorPath)
{
    if (editorPath.empty())
        return {};
#ifdef _WIN32
    constexpr auto name = "ldv-dump.exe";
#else
    constexpr auto name = "ldv-dump";
#endif
    return (std::filesystem::u8path(editorPath).parent_path() / name).generic_u8string();
}

std::string defaultLuduvoEditorPath(const std::string& localDirectory)
{
    const auto bundle = std::filesystem::u8path(localDirectory) / "bundle";
#ifdef _WIN32
    return (bundle / "LuduvoEditor.exe").generic_u8string();
#else
    return (bundle / "usr" / "bin" / "LuduvoEditor").generic_u8string();
#endif
}

std::optional<WorldDumpSource> selectWorldDumpSource(const std::string& directory, std::string* error)
{
    const auto root = std::filesystem::u8path(directory);
    std::error_code ec;
    const auto saved = root / "world.ldv";
    std::filesystem::path selected;
    std::filesystem::file_time_type selectedTime;
    uint64_t selectedSequence = 0;
    const auto consider = [&](const std::filesystem::path& path, uint64_t sequence)
    {
        const auto status = sequence ? std::filesystem::symlink_status(path, ec) : std::filesystem::status(path, ec);
        if (std::filesystem::is_regular_file(status))
        {
            const auto modified = std::filesystem::last_write_time(path, ec);
            if (!ec && (selected.empty() || modified > selectedTime ||
                           (modified == selectedTime && selected != saved && sequence > selectedSequence)))
            {
                selected = path;
                selectedTime = modified;
                selectedSequence = sequence;
            }
        }
        if (ec == std::errc::no_such_file_or_directory)
            ec.clear();
        if (ec && error)
            *error = "could not inspect " + path.generic_u8string() + ": " + ec.message();
        return !ec;
    };
    if (!consider(saved, 0) || !consider(root / "world.ldv.autosave", 0))
        return std::nullopt;

    const auto backups = root / ".backups";
    for (std::filesystem::directory_iterator it(backups, ec), end; it != end && !ec; it.increment(ec))
    {
        const auto name = it->path().filename().generic_u8string();
        if (name.size() <= 10 || name.compare(0, 6, "world.") != 0 || name.compare(name.size() - 4, 4, ".ldv") != 0)
            continue;
        const auto first = name.data() + 6;
        const auto last = name.data() + name.size() - 4;
        uint64_t sequence = 0;
        const auto parsed = std::from_chars(first, last, sequence);
        if (parsed.ec == std::errc{} && parsed.ptr == last && sequence && !consider(it->path(), sequence))
            return std::nullopt;
    }
    if (ec && ec != std::errc::no_such_file_or_directory)
    {
        if (error)
            *error = "could not scan Studio backups: " + ec.message();
        return std::nullopt;
    }
    if (selected.empty())
    {
        if (error)
            *error = "project has no saved world, autosave, or numbered Studio backup";
        return std::nullopt;
    }

    const auto unixTime = std::chrono::time_point_cast<std::chrono::milliseconds>(
        selectedTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return WorldDumpSource{selected == saved ? "save" : "autosave", selected.generic_u8string(), unixTime.time_since_epoch().count()};
}

static std::optional<WorldDumpSnapshot> trySceneDump(
    const AutoDumpPlan& plan, const WorldDumpSource& source, const std::string& input, const std::atomic<bool>* cancelled)
{
    const auto tool = sceneDumperPath(plan.editorPath);
    std::error_code ec;
    if (tool.empty() || !std::filesystem::is_regular_file(std::filesystem::u8path(tool), ec))
        return std::nullopt;

    ChildProcess child;
    std::string error;
    if (!child.start({tool, "--json", input}, &error, true))
    {
        logging::write(logging::Level::Debug, "helper.autodump.process", "ldv-dump: " + error);
        return std::nullopt;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::min(plan.timeout, std::chrono::seconds(10));
    while (child.running())
    {
        if ((cancelled && *cancelled) || std::chrono::steady_clock::now() >= deadline)
            return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto status = child.exitDescription();
    child.stop();
    if (status != "exit code 0")
    {
        logging::write(logging::Level::Debug, "helper.autodump.process", "ldv-dump " + status + ": " + child.capturedOutput());
        return std::nullopt;
    }
    const std::string contents = child.takeDocument();
    const std::string path = plan.outputPath + ".scene.json";
    auto parsed = parseWorldDump(contents, path);
    if (!parsed.snapshot || !parsed.snapshot->sceneFormat)
    {
        logging::write(logging::Level::Debug, "helper.autodump.process",
            "ldv-dump returned no usable scene JSON: " + parsed.error.value_or("unexpected dump format"));
        return std::nullopt;
    }
    if (!writeFileAtomically(path, contents, &error))
    {
        logging::write(logging::Level::Debug, "helper.autodump.process", "Could not write ldv-dump output: " + error);
        return std::nullopt;
    }
    parsed.snapshot->inputSource = source;
    parsed.snapshot->capturedAtUnixMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return std::move(parsed.snapshot);
}

static AutoDumpResult executeAutomaticWorldDump(const AutoDumpPlan& plan, const std::atomic<bool>* cancelled)
{
    if (cancelled && *cancelled)
        return {false, "automatic world dump was cancelled"};
    if (plan.projectDirectory.empty())
        return {false, "the selected project has no known directory"};
    std::string error;
    const auto recovered = recoverAutomaticWorldDumpAutosave(plan.projectDirectory);
    if (!recovered.success)
        return recovered;
    auto source = selectWorldDumpSource(plan.projectDirectory, &error);
    if (!source)
        return {false, error};
    logging::write(logging::Level::Debug, "helper.autodump", "Input: " + source->path + " (" + source->kind + ")");

    std::error_code filesystemError;
    const std::filesystem::path output = std::filesystem::u8path(plan.outputPath);
    std::filesystem::create_directories(output.parent_path(), filesystemError);
    if (filesystemError)
        return {false, "could not create the world-dump directory: " + filesystemError.message()};
    TemporaryDumpProject temporary;
    if (!temporary.prepare(*source, &error))
        return {false, error};
    if (cancelled && *cancelled)
        return {false, "automatic world dump was cancelled"};
    if (plan.method != DumpMethod::Studio)
    {
        if (auto snapshot = trySceneDump(plan, *source, temporary.worldPath(), cancelled))
            return {true, {}, std::move(snapshot)};
        if (plan.method == DumpMethod::SavedScene && !(cancelled && *cancelled))
            return {false, "ldv-dump could not read the selected scene or is unavailable"};
    }
    if (cancelled && *cancelled)
        return {false, "automatic world dump was cancelled"};
    if (plan.editorPath.empty() || !std::filesystem::exists(std::filesystem::u8path(plan.editorPath)))
        return {false, "LuduvoEditor was not found at " + plan.editorPath};
    if (!temporary.copyProjectFiles(plan.projectDirectory, &error, cancelled))
        return {false, error};

    std::filesystem::remove(output, filesystemError);
    if (filesystemError)
        return {false, "could not replace the previous world dump: " + filesystemError.message()};

    ChildProcess child;
    std::vector<std::string> arguments = {plan.editorPath, "--headless", temporary.directory(),
        "--exec", "world.dump " + plan.outputPath, "--exec", "file.quit"};
    logging::write(logging::Level::Debug, "helper.autodump.process", "Launching editor: " + nlohmann::json(arguments).dump());
    if (!child.start(arguments, &error))
        return {false, error};

    const auto deadline = std::chrono::steady_clock::now() + plan.timeout;
    std::string lastParseError;
    std::string termination = "timed out";
    uintmax_t lastSize = uintmax_t(-1);
    size_t stableValidReads = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (cancelled && *cancelled)
        {
            child.stop();
            return {false, "automatic world dump was cancelled"};
        }
        const bool running = child.running();
        const bool exitedSuccessfully = !running && child.exitDescription() == "exit code 0";
        if ((running || exitedSuccessfully) && std::filesystem::exists(output))
        {
            WorldDumpParseResult parsed = parseWorldDumpFile(plan.outputPath);
            if (parsed.snapshot)
            {
                const uintmax_t size = std::filesystem::file_size(output, filesystemError);
                if (!filesystemError && size == lastSize)
                    ++stableValidReads;
                else
                    stableValidReads = 0;
                lastSize = size;
                if (exitedSuccessfully || stableValidReads >= 2)
                {
                    // Give Studio time to flush the matching command-log acknowledgement before ending the disposable process.
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    child.stop();
                    parsed.snapshot->inputSource = *source;
                    return {true, {}, std::move(parsed.snapshot)};
                }
            }
            else
            {
                stableValidReads = 0;
                lastParseError = parsed.error.value_or("the dump was incomplete");
            }
        }
        if (!running)
        {
            termination = child.exitDescription();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    logging::write(logging::Level::Warning, "helper.autodump.process", "Editor " + termination + " for " + plan.projectId);
    child.stop();
    logging::write(logging::Level::Debug, "helper.autodump.process", "Editor output tail for " + plan.projectId + ":\n" + child.capturedOutput());
    if (!lastParseError.empty())
        return {false, "LuduvoEditor did not produce a complete world dump: " + lastParseError};
    return {false, "LuduvoEditor " + termination + " before producing a world dump"};
}

AutoDumpResult runAutomaticWorldDump(const AutoDumpPlan& plan, const std::atomic<bool>* cancelled)
{
    const auto started = std::chrono::steady_clock::now();
    logging::write(logging::Level::Info, "helper.autodump", "Starting dump for " + plan.projectId);
    const auto result = executeAutomaticWorldDump(plan, cancelled);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    logging::write(result.success ? logging::Level::Info : logging::Level::Error, "helper.autodump",
        "Dump for " + plan.projectId + (result.success ? " succeeded" : " failed: " + result.error) + " in " + std::to_string(elapsed) + " ms");
    return result;
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
