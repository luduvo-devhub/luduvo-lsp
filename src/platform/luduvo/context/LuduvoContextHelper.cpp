#include "Platform/luduvo/context/LuduvoContextHelper.hpp"

#include "Platform/luduvo/context/LuduvoContextStore.hpp"
#include "Platform/luduvo/context/LuduvoContextInspection.hpp"
#include "Platform/luduvo/context/LuduvoContextLogProcessor.hpp"
#include "Platform/luduvo/context/LuduvoProcessIdentity.hpp"
#include "Platform/luduvo/context/LuduvoContextReport.hpp"
#include "Platform/luduvo/context/LuduvoWorldDump.hpp"
#include "Analyze/CliConfigurationParser.hpp"
#include "LSP/ClientConfiguration.hpp"
#include "LuauFileUtils.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace luduvo
{

using json = nlohmann::json;

static bool writeTextFile(const std::string& path, const std::string& contents, std::string* error)
{
    const std::filesystem::path outputPath = std::filesystem::u8path(path);
    std::error_code filesystemError;
    if (!outputPath.parent_path().empty())
        std::filesystem::create_directories(outputPath.parent_path(), filesystemError);
    if (filesystemError)
    {
        if (error)
            *error = filesystemError.message();
        return false;
    }
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), std::streamsize(contents.size()));
    if (!output)
    {
        if (error)
            *error = "could not write " + path;
        return false;
    }
    return true;
}

static int printError(const std::string& error)
{
    std::cerr << error << '\n';
    return 1;
}

static std::string argumentOr(const argparse::ArgumentParser& program, const char* argument, std::string fallback)
{
    if (auto value = program.present<std::string>(argument))
        return *value;
    return fallback;
}

static std::optional<ClientLuduvoConfiguration> contextConfiguration(const argparse::ArgumentParser& program, std::string* error)
{
    const auto settingsPath = program.present<std::string>("--settings");
    if (!settingsPath)
        return ClientLuduvoConfiguration{};
    const auto contents = Luau::FileUtils::readFile(*settingsPath);
    if (!contents)
    {
        if (error)
            *error = "could not read LSP settings at " + *settingsPath;
        return std::nullopt;
    }
    try
    {
        return dottedToClientConfiguration(*contents).platform.luduvo;
    }
    catch (const std::exception& exception)
    {
        if (error)
            *error = "could not parse LSP settings at " + *settingsPath + ": " + exception.what();
        return std::nullopt;
    }
}

static std::optional<std::string> contextStateDirectory(const argparse::ArgumentParser& program, std::string* error)
{
    const auto configuration = contextConfiguration(program, error);
    if (!configuration)
        return std::nullopt;
    return argumentOr(program, "--state-directory", defaultContextStateDirectory(configuration->directories.local, configuration->directories.context));
}

static int writeOutput(const argparse::ArgumentParser& program, const json& value, const ReportNode& report)
{
    const std::string output = program.get<std::string>("--format") == "json" ? value.dump(2) + "\n" : renderReportText(report);
    if (const auto outputPath = program.present<std::string>("--output"))
    {
        std::string error;
        if (!writeTextFile(*outputPath, output, &error))
            return printError(error);
    }
    else
        std::cout << output;
    return 0;
}

static int parseCommand(const argparse::ArgumentParser& program)
{
    const std::string input = program.get<std::string>("input");
    const WorldDumpParseResult parsed = parseWorldDumpFile(input);
    if (!parsed.snapshot)
        return printError(parsed.error.value_or("could not parse world dump"));

    json result = *parsed.snapshot;
    if (!parsed.warnings.empty())
        result["warnings"] = parsed.warnings;
    const ContextInspectionResult inspected = inspectContextInput(input);
    const ReportNode report = inspected.report ? buildContextInspectionReport(*inspected.report) : ReportNode{"Parsed world dump"};
    return writeOutput(program, result, report);
}

static int inspectCommand(const argparse::ArgumentParser& program)
{
    std::string configurationError;
    const auto stateDirectory = contextStateDirectory(program, &configurationError);
    if (!stateDirectory)
        return printError(configurationError);
    const std::string kind = program.get<std::string>("kind");
    const auto input = program.present<std::string>("input");
    if (kind != "instance" && !input)
        return printError("context inspect " + kind + " requires an input path");
    if (kind == "instance")
    {
        InstanceInspectionRequest request;
        request.nodeId = program.present<uint64_t>("--node-id");
        request.instanceId = program.present<uint64_t>("--instance-id");
        request.name = program.present<std::string>("--name");
        request.projectName = program.present<std::string>("--project");
        request.projectId = program.present<std::string>("--project-id");
        request.projectDirectory = program.present<std::string>("--project-directory");
        request.scriptPath = program.present<std::string>("--script");
        request.scriptDirectory = program.present<std::string>("--script-directory");
        request.sections = program.get<std::vector<std::string>>("--section");
        const ContextInspectionResult inspected =
            inspectContextInstance(request, *stateDirectory);
        if (!inspected.report)
            return printError(inspected.error.value_or("could not inspect instance"));
        return writeOutput(program, *inspected.report, buildContextInspectionReport(*inspected.report));
    }
    const ContextInspectionResult inspected = inspectContextInput(*input, *stateDirectory);
    if (!inspected.report)
        return printError(inspected.error.value_or("could not inspect input"));
    if ((kind == "dump" && inspected.report->value("kind", "") != "worldDump") ||
        (kind == "project" && inspected.report->value("kind", "") != "project") ||
        (kind == "script" && inspected.report->value("kind", "") != "script"))
        return printError("input is not a " + kind);
    return writeOutput(program, *inspected.report, buildContextInspectionReport(*inspected.report));
}

static int updateCommand(const argparse::ArgumentParser& program)
{
    std::string configurationError;
    const auto stateDirectory = contextStateDirectory(program, &configurationError);
    if (!stateDirectory)
        return printError(configurationError);
    const std::string kind = program.get<std::string>("kind");
    const std::string input = program.get<std::string>("input");
    const auto projectDirectoryOption = program.present<std::string>("--project-directory");
    const auto projectName = program.present<std::string>("--project");

    ContextStore store(*stateDirectory);
    std::optional<std::string> resolvedProjectName = projectName;
    ContextStoreResult result{true, std::nullopt, std::nullopt};
    if (kind == "project")
    {
        result = store.updateProjectFiles(input, projectName);
        if (!result.success)
            return printError(result.error.value_or("could not update project files"));
    }
    else
    {
        if (!resolvedProjectName && !projectDirectoryOption)
            return printError("context update dump requires --project NAME or --project-directory PATH");
        if (!resolvedProjectName && projectDirectoryOption)
        {
            const ProjectFileInspectionResult inspected = inspectProjectFiles(*projectDirectoryOption);
            if (!inspected.project)
                return printError(inspected.error.value_or("could not resolve project"));
            resolvedProjectName = inspected.project->name;
        }
        WorldDumpParseResult parsed = parseWorldDumpFile(input);
        if (!parsed.snapshot)
            return printError(parsed.error.value_or("could not parse world dump"));
        result = store.updateWorld(std::move(*parsed.snapshot), *resolvedProjectName, projectDirectoryOption);
        if (!result.success)
            return printError(result.error.value_or("could not update project context"));
    }

    std::string error;
    const auto catalog = store.load(&error);
    if (!catalog)
        return printError(error);
    ReportNode report{"Updated context", {}, {{"Projects", std::to_string(catalog->projects.size())}, {"State directory", store.directory()}}};
    return writeOutput(program, *catalog, report);
}

struct LogReader
{
    std::string path;
    std::streamoff offset = 0;
    std::string pending;

    LogReader(std::string path, bool startAtEnd = false)
        : path(std::move(path))
    {
        if (startAtEnd)
        {
            std::error_code error;
            const auto size = std::filesystem::file_size(std::filesystem::u8path(this->path), error);
            if (!error)
                offset = std::streamoff(size);
        }
    }

    std::vector<std::string> readLines()
    {
        std::vector<std::string> result;
        std::error_code error;
        const auto size = std::filesystem::file_size(std::filesystem::u8path(path), error);
        if (error)
            return result;
        if (std::streamoff(size) < offset)
        {
            offset = 0;
            pending.clear();
        }

        std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
        if (!input)
            return result;
        input.seekg(offset);
        std::string chunk((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        offset += std::streamoff(chunk.size());
        pending += chunk;
        size_t start = 0;
        while (true)
        {
            const size_t newline = pending.find('\n', start);
            if (newline == std::string::npos)
                break;
            std::string line = pending.substr(start, newline - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            result.push_back(std::move(line));
            start = newline + 1;
        }
        pending.erase(0, start);
        return result;
    }
};

class ContextWatcher
{
    ContextLogProcessor processor;
    LogReader studioLog;
    LogReader commandsLog;

public:
    ContextWatcher(ContextStore store, std::string studioLogPath, std::string commandsLogPath)
        : processor(std::move(store))
        , studioLog(std::move(studioLogPath))
        , commandsLog(std::move(commandsLogPath), true)
    {
    }

    void poll()
    {
        for (const std::string& line : studioLog.readLines())
            processor.ingestStudioLine(line);
        for (const std::string& line : commandsLog.readLines())
            processor.ingestCommandLine(line);
    }
};

static bool acquireHelperOwnership(const ContextStore& store, const ProcessIdentity& identity, std::string* error)
{
    const std::filesystem::path lock = std::filesystem::u8path(store.controlDirectory()) / "lock";
    for (size_t attempt = 0; attempt < 4; ++attempt)
    {
        std::error_code filesystemError;
        if (std::filesystem::create_directory(lock, filesystemError))
        {
            if (writeProcessIdentity(store.ownerPath(), identity, error))
                return true;
            std::filesystem::remove_all(lock, filesystemError);
            return false;
        }

        const auto owner = readProcessIdentity(store.ownerPath());
        if (owner && isProcessAlive(*owner))
            return true;

        // A newly started helper may have created the lock just before writing its identity.
        if (!owner && attempt < 2)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::filesystem::remove_all(lock, filesystemError);
    }

    if (error)
        *error = "could not acquire context helper lock";
    return false;
}

static int runCommand(const argparse::ArgumentParser& program)
{
    std::string configurationError;
    const auto configuration = contextConfiguration(program, &configurationError);
    if (!configuration)
        return printError(configurationError);
    const std::string stateDirectory = argumentOr(
        program, "--state-directory", defaultContextStateDirectory(configuration->directories.local, configuration->directories.context));
    const std::string commandsLog = argumentOr(program, "--commands-log", defaultCommandsLogPath(configuration->directories.roaming));
    const std::string studioLog = argumentOr(program, "--studio-log", defaultStudioLogPath(configuration->directories.local));
    const int pollMilliseconds = program.get<int>("--poll-interval");
    const int idleTimeoutSeconds = program.get<int>("--idle-timeout");
    ContextStore store(stateDirectory);

    std::error_code error;
    std::filesystem::create_directories(std::filesystem::u8path(store.controlDirectory()), error);
    if (error)
        return printError("could not create helper control directory: " + error.message());
    const auto identity = currentProcessIdentity();
    if (!identity)
        return printError("could not identify the context helper process");
    std::string ownershipError;
    if (!acquireHelperOwnership(store, *identity, &ownershipError))
        return printError(ownershipError);
    const auto owner = readProcessIdentity(store.ownerPath());
    if (!owner || owner->processId != identity->processId || owner->startToken != identity->startToken)
        return 0;

    std::filesystem::remove(std::filesystem::u8path(store.stopPath()), error);
    ContextWatcher watcher(store, studioLog, commandsLog);
    std::optional<std::chrono::steady_clock::time_point> withoutClientsSince;
    while (true)
    {
        watcher.poll();
        if (std::filesystem::exists(std::filesystem::u8path(store.stopPath())))
            break;
        if (removeDeadProcessRegistrations(store.leasesDirectory()) > 0)
            withoutClientsSince.reset();
        else if (!withoutClientsSince)
            withoutClientsSince = std::chrono::steady_clock::now();
        else if (idleTimeoutSeconds >= 0 && std::chrono::steady_clock::now() - *withoutClientsSince > std::chrono::seconds(idleTimeoutSeconds))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(50, pollMilliseconds)));
    }
    std::filesystem::remove(std::filesystem::u8path(store.ownerPath()), error);
    std::filesystem::remove_all(std::filesystem::u8path(store.controlDirectory()) / "lock", error);
    return 0;
}

static int statusCommand(const argparse::ArgumentParser& program)
{
    std::string configurationError;
    const auto stateDirectory = contextStateDirectory(program, &configurationError);
    if (!stateDirectory)
        return printError(configurationError);
    const ContextStore store(*stateDirectory);
    std::string error;
    const auto status = store.status(&error);
    if (!status)
        return printError(error);
    const json result = *status;
    return writeOutput(program, result, buildContextStatusReport(result));
}

static int stopCommand(const argparse::ArgumentParser& program)
{
    std::string configurationError;
    const auto stateDirectory = contextStateDirectory(program, &configurationError);
    if (!stateDirectory)
        return printError(configurationError);
    const ContextStore store(*stateDirectory);
    std::string error;
    if (!writeTextFile(store.stopPath(), "stop\n", &error))
        return printError(error);
    return 0;
}

static int mutateCommand(const argparse::ArgumentParser& program, bool forget)
{
    const std::string kind = program.get<std::string>("kind");
    const std::vector<std::string> selectors = program.get<std::vector<std::string>>("selectors");
    const bool all = program.get<bool>("--all");
    if (forget && !program.get<bool>("--yes"))
    {
        std::cout << "This permanently forgets the selected Luduvo context. It does not delete Luduvo projects or source files. Continue? [y/N] ";
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "y" && answer != "Y" && answer != "yes" && answer != "YES")
            return printError("forget cancelled");
    }

    std::string configurationError;
    const auto stateDirectory = contextStateDirectory(program, &configurationError);
    if (!stateDirectory)
        return printError(configurationError);
    const ContextStore store(*stateDirectory);
    const ContextStoreResult result = forget ? store.forget(kind, selectors, all) : store.clean(kind, selectors, all);
    if (!result.success)
        return printError(result.error.value_or(forget ? "could not forget context" : "could not clean context"));
    const json output = {{"success", true}, {"affected", result.affected}, {"kind", kind}, {"all", all}};
    ReportNode report{forget ? "Forgot context" : "Cleaned context"};
    report.fields.push_back({"Affected records", std::to_string(result.affected)});
    return writeOutput(program, output, report);
}

static void addStateDirectoryArgument(argparse::ArgumentParser& parser)
{
    parser.add_argument("--settings").help("path to LSP-style settings used for Luduvo directories").metavar("PATH");
    parser.add_argument("--state-directory").help("context database directory").metavar("DIRECTORY");
}

static void addOutputArguments(argparse::ArgumentParser& parser)
{
    parser.add_argument("--format").help("output format").choices("text", "json").default_value(std::string("text"));
    parser.add_argument("--output").help("write output to a file instead of stdout").metavar("PATH");
}

ContextCli::ContextCli()
    : contextParser("context")
    , runParser("run")
    , statusParser("status")
    , stopParser("stop")
    , parseParser("parse")
    , inspectParser("inspect")
    , updateParser("update")
    , cleanParser("clean")
    , forgetParser("forget")
{
    contextParser.add_description("Inspect and maintain Luduvo project context");

    runParser.add_description("Run the shared Luduvo context helper");
    addStateDirectoryArgument(runParser);
    runParser.add_argument("--commands-log").help("Luduvo Studio commands.log path").metavar("PATH");
    runParser.add_argument("--studio-log").help("Luduvo Studio studio.log path").metavar("PATH");
    runParser.add_argument("--poll-interval").help("log polling interval in milliseconds").scan<'i', int>().default_value(500);
    runParser.add_argument("--idle-timeout")
        .help("seconds without a live LSP client before the helper exits; -1 disables the timeout")
        .scan<'i', int>()
        .default_value(30);

    statusParser.add_description("Show helper, project, and cleanup status");
    addStateDirectoryArgument(statusParser);
    addOutputArguments(statusParser);

    stopParser.add_description("Stop the shared Luduvo context helper");
    addStateDirectoryArgument(stopParser);

    parseParser.add_description("Parse a Luduvo world dump without changing context state");
    parseParser.add_argument("input").help("world dump path").metavar("DUMP");
    addOutputArguments(parseParser);

    inspectParser.add_description("Inspect a world dump, project, script, or captured instance");
    inspectParser.add_argument("kind").choices("dump", "project", "script", "instance").metavar("KIND");
    inspectParser.add_argument("input").help("input path; omitted for instance inspection").metavar("INPUT").nargs(argparse::nargs_pattern::optional);
    inspectParser.add_argument("--node-id").help("match an instance by node ID").scan<'u', uint64_t>().metavar("ID");
    inspectParser.add_argument("--instance-id").help("match an instance by runtime instance ID").scan<'u', uint64_t>().metavar("ID");
    inspectParser.add_argument("--name").help("match instances by name").metavar("NAME");
    inspectParser.add_argument("--project").help("select a project by name").metavar("NAME");
    inspectParser.add_argument("--project-id").help("select a project by stable context ID").metavar("ID");
    inspectParser.add_argument("--project-directory").help("select a project by its directory").metavar("DIRECTORY");
    inspectParser.add_argument("--script").help("select projects containing this script").metavar("PATH");
    inspectParser.add_argument("--script-directory").help("select projects containing scripts below this directory").metavar("DIRECTORY");
    inspectParser.add_argument("--section").help("include only a named report section").append().default_value<std::vector<std::string>>({});
    addStateDirectoryArgument(inspectParser);
    addOutputArguments(inspectParser);

    updateParser.add_description("Update a project index or world dump in the context database");
    updateParser.add_argument("kind").choices("project", "dump").metavar("KIND");
    updateParser.add_argument("input").help("project directory, project.json, or world dump path").metavar("INPUT");
    updateParser.add_argument("--project").help("project name").metavar("NAME");
    updateParser.add_argument("--project-directory").help("project directory associated with a dump").metavar("DIRECTORY");
    addStateDirectoryArgument(updateParser);
    addOutputArguments(updateParser);

    for (argparse::ArgumentParser* parser : {&cleanParser, &forgetParser})
    {
        parser->add_argument("kind")
            .help("context object kind")
            .metavar("KIND")
            .nargs(argparse::nargs_pattern::optional)
            .default_value(std::string{});
        parser->add_argument("selectors")
            .help("IDs, paths, or names to remove")
            .metavar("SELECTOR")
            .nargs(argparse::nargs_pattern::any)
            .default_value<std::vector<std::string>>({});
        parser->add_argument("--all").help("apply to every matching object").default_value(false).implicit_value(true);
        addStateDirectoryArgument(*parser);
        addOutputArguments(*parser);
    }
    cleanParser.add_description("Remove context objects reported as cleanup candidates");
    forgetParser.add_description("Permanently forget selected valid context without deleting Luduvo project files");
    forgetParser.add_argument("--yes").help("skip the confirmation prompt").default_value(false).implicit_value(true);

    contextParser.add_subparser(runParser);
    contextParser.add_subparser(statusParser);
    contextParser.add_subparser(stopParser);
    contextParser.add_subparser(parseParser);
    contextParser.add_subparser(inspectParser);
    contextParser.add_subparser(updateParser);
    contextParser.add_subparser(cleanParser);
    contextParser.add_subparser(forgetParser);
}

argparse::ArgumentParser& ContextCli::parser()
{
    return contextParser;
}

int ContextCli::execute() const
{
    if (contextParser.is_subcommand_used("run"))
        return runCommand(runParser);
    if (contextParser.is_subcommand_used("status"))
        return statusCommand(statusParser);
    if (contextParser.is_subcommand_used("stop"))
        return stopCommand(stopParser);
    if (contextParser.is_subcommand_used("parse"))
        return parseCommand(parseParser);
    if (contextParser.is_subcommand_used("inspect"))
        return inspectCommand(inspectParser);
    if (contextParser.is_subcommand_used("update"))
        return updateCommand(updateParser);
    if (contextParser.is_subcommand_used("clean"))
        return mutateCommand(cleanParser, false);
    if (contextParser.is_subcommand_used("forget"))
        return mutateCommand(forgetParser, true);
    return printError("context requires a command");
}

} // namespace luduvo
