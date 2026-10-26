#include "Platform/luduvo/context/LuduvoContextInspection.hpp"

#include "LSP/Utils.hpp"
#include "LuauFileUtils.hpp"
#include "Platform/luduvo/context/LuduvoContextStore.hpp"
#include "Platform/luduvo/context/LuduvoWorldDump.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>

namespace luduvo
{

using json = nlohmann::json;

static bool isLuauFile(const std::filesystem::path& path)
{
    std::string extension = path.extension().generic_u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char character)
        {
            return char(std::tolower(character));
        });
    return extension == ".lua" || extension == ".luau";
}

ProjectFileInspectionResult inspectProjectFiles(const std::string& path, std::optional<std::string> configuredProjectName)
{
    std::filesystem::path input = std::filesystem::u8path(path);
    std::error_code filesystemError;
    if (std::filesystem::is_regular_file(input, filesystemError))
        input = input.parent_path();
    const std::filesystem::path directory = std::filesystem::u8path(normalizeContextPath(input.generic_u8string()));
    const std::filesystem::path projectFile = directory / "project.json";
    const auto source = Luau::FileUtils::readFile(projectFile.generic_u8string());

    ProjectFileInspection result;
    result.directory = directory.generic_u8string();
    result.projectFile = projectFile.generic_u8string();
    if (!source && !configuredProjectName)
        return {std::nullopt, "could not read " + projectFile.generic_u8string()};
    if (source)
    {
        try
        {
            const json project = json::parse(*source);
            if (!project.contains("name") || !project["name"].is_string())
            {
                if (!configuredProjectName)
                    return {std::nullopt, projectFile.generic_u8string() + " does not contain a string name"};
            }
            else
            {
                result.name = project["name"].get<std::string>();
                if (project.contains("assets"))
                {
                    if (!project["assets"].is_object())
                        return {std::nullopt, projectFile.generic_u8string() + " contains a non-object assets field"};
                    for (const auto& [_, asset] : project["assets"].items())
                    {
                        ++result.assetCount;
                        if (!asset.is_object())
                        {
                            ++result.assetTypes["<invalid>"];
                            continue;
                        }
                        const std::string type = asset.value("type", "<unknown>");
                        ++result.assetTypes[type];
                        if (type != "script" || !asset.contains("path") || !asset["path"].is_string())
                            continue;
                        ++result.scriptAssetCount;
                        const std::string script =
                            normalizeContextPath((directory / std::filesystem::u8path(asset["path"].get<std::string>())).generic_u8string());
                        result.declaredScripts.push_back(script);
                        if (!std::filesystem::is_regular_file(std::filesystem::u8path(script), filesystemError))
                            result.missingScripts.push_back(script);
                        filesystemError.clear();
                    }
                }
            }
        }
        catch (const json::exception& exception)
        {
            if (!configuredProjectName)
                return {std::nullopt, "could not parse " + projectFile.generic_u8string() + ": " + exception.what()};
        }
    }
    if (configuredProjectName)
        result.name = *configuredProjectName;

    const std::filesystem::path autorun = directory / "scripts" / "autorun";
    if (std::filesystem::is_directory(autorun, filesystemError))
    {
        for (std::filesystem::recursive_directory_iterator iterator(autorun, filesystemError), end; iterator != end && !filesystemError;
             iterator.increment(filesystemError))
        {
            if (!iterator->is_regular_file(filesystemError) || filesystemError || !isLuauFile(iterator->path()))
                continue;
            ++result.autorunScriptCount;
            result.autorunScripts.push_back(normalizeContextPath(iterator->path().generic_u8string()));
        }
    }
    result.scripts = result.declaredScripts;
    result.scripts.insert(result.scripts.end(), result.autorunScripts.begin(), result.autorunScripts.end());
    std::sort(result.scripts.begin(), result.scripts.end());
    result.scripts.erase(std::unique(result.scripts.begin(), result.scripts.end()), result.scripts.end());
    std::sort(result.missingScripts.begin(), result.missingScripts.end());
    result.missingScripts.erase(std::unique(result.missingScripts.begin(), result.missingScripts.end()), result.missingScripts.end());
    return {std::move(result), std::nullopt};
}

static ContextInspectionResult inspectWorldDump(const std::string& path)
{
    const WorldDumpParseResult parsed = parseWorldDumpFile(path);
    if (!parsed.snapshot)
        return {std::nullopt, parsed.error.value_or("could not parse world dump")};

    size_t roots = 0;
    size_t scriptAttachments = 0;
    std::set<std::string> uniqueScripts;
    std::map<std::string, size_t> instanceTypes;
    for (const WorldInstance& instance : parsed.snapshot->instances)
    {
        if (instance.parentNodeId == 0)
            ++roots;
        scriptAttachments += instance.scripts.size();
        uniqueScripts.insert(instance.scripts.begin(), instance.scripts.end());
        ++instanceTypes[instance.type];
    }

    json summary = {
        {"tick", parsed.snapshot->tick},
        {"commandSequence", parsed.snapshot->commandSequence},
        {"undoCount", parsed.snapshot->undoCount},
        {"redoCount", parsed.snapshot->redoCount},
        {"instanceCount", parsed.snapshot->instances.size()},
        {"rootInstanceCount", roots},
        {"scriptAttachmentCount", scriptAttachments},
        {"uniqueScriptCount", uniqueScripts.size()},
        {"instanceTypes", instanceTypes},
    };
    return {json{{"kind", "worldDump"},
                {"inputPath", normalizeContextPath(path)},
                {"sourcePath", parsed.snapshot->sourcePath},
                {"summary", std::move(summary)},
                {"warnings", parsed.warnings},
                {"scripts", uniqueScripts}},
        std::nullopt};
}

static std::string scriptSide(const std::string& path)
{
    std::string filename = std::filesystem::u8path(path).filename().generic_u8string();
    std::transform(filename.begin(), filename.end(), filename.begin(),
        [](unsigned char character)
        {
            return char(std::tolower(character));
        });
    if (filename.find(".client.") != std::string::npos)
        return "client";
    if (filename.find(".server.") != std::string::npos)
        return "server";
    return "unspecified";
}

static bool containsPath(const std::vector<std::string>& paths, const std::string& target)
{
    return std::find(paths.begin(), paths.end(), target) != paths.end();
}

static ContextInspectionResult inspectScript(const std::string& path, const std::string& stateDirectory)
{
    const std::string normalizedPath = normalizeContextPath(path);
    const ContextStore store(stateDirectory.empty() ? defaultContextStateDirectory() : stateDirectory);
    std::string error;
    const auto catalog = store.load(&error);
    if (!catalog)
        return {std::nullopt, error};

    std::vector<json> candidates;
    const auto indexed = catalog->scriptProjects.find(normalizedPath);
    std::map<std::string, std::string> workspaceSelections;
    for (const auto& [workspace, selections] : catalog->workspaceScriptSelections)
        if (const auto selected = selections.find(normalizedPath); selected != selections.end())
            workspaceSelections[workspace] = selected->second;
    for (const auto& [projectId, project] : catalog->projects)
    {
        std::set<std::string> sources;
        if (indexed != catalog->scriptProjects.end() && std::find(indexed->second.begin(), indexed->second.end(), projectId) != indexed->second.end())
            sources.insert("catalog");

        if (!project.directory.empty())
        {
            const ProjectFileInspectionResult projectFiles = inspectProjectFiles(project.directory, project.name);
            if (projectFiles.project)
            {
                if (containsPath(projectFiles.project->declaredScripts, normalizedPath))
                    sources.insert("projectAsset");
                if (containsPath(projectFiles.project->autorunScripts, normalizedPath))
                    sources.insert("autorun");
            }
        }

        std::vector<json> attachments;
        std::optional<WorldDumpSnapshot> world = store.loadWorld(projectId);
        if (world && !project.directory.empty())
        {
            for (const WorldInstance& instance : world->instances)
            {
                for (const std::string& script : instance.scripts)
                {
                    if (script.compare(0, 7, "core://") == 0)
                        continue;
                    const std::string attachedPath = normalizeContextPath(
                        (std::filesystem::u8path(project.directory) / std::filesystem::u8path(script)).generic_u8string());
                    if (attachedPath != normalizedPath)
                        continue;
                    sources.insert("worldDump");
                    attachments.push_back({{"nodeId", instance.nodeId},
                        {"instanceId", instance.instanceId},
                        {"parentNodeId", instance.parentNodeId},
                        {"name", instance.name},
                        {"type", instance.type}});
                }
            }
        }
        if (sources.empty())
            continue;

        json candidate = {{"id", projectId},
            {"name", project.name},
            {"directory", project.directory},
            {"active", catalog->activeProjectId == projectId},
            {"fresh", project.fresh},
            {"staleReason", project.staleReason},
            {"sources", sources},
            {"attachments", attachments}};
        candidate["selected"] = !workspaceSelections.empty() &&
                                std::any_of(workspaceSelections.begin(), workspaceSelections.end(), [&](const auto& selection)
                                    {
                                        return selection.second == projectId;
                                    });
        if (world)
            candidate["snapshot"] = {{"tick", world->tick},
                {"commandSequence", world->commandSequence},
                {"capturedAtUnixMs", world->capturedAtUnixMs},
                {"sourcePath", world->sourcePath}};
        candidates.push_back(std::move(candidate));
    }

    return {json{{"kind", "script"},
                {"inputPath", normalizedPath},
                {"exists", std::filesystem::is_regular_file(std::filesystem::u8path(path))},
                {"side", scriptSide(path)},
                {"workspaceSelections", workspaceSelections},
                {"projectCount", candidates.size()},
                {"projects", candidates}},
        std::nullopt};
}

static bool pathWithin(const std::string& path, const std::string& directory)
{
    const std::string normalizedPath = normalizeContextPath(path);
    std::string normalizedDirectory = normalizeContextPath(directory);
    if (!normalizedDirectory.empty() && normalizedDirectory.back() != '/')
        normalizedDirectory.push_back('/');
    return normalizedPath.compare(0, normalizedDirectory.size(), normalizedDirectory) == 0;
}

static bool projectMatches(const ContextProject& project, const InstanceInspectionRequest& request)
{
    if (request.projectId && project.id != *request.projectId)
        return false;
    if (request.projectName && project.name != *request.projectName)
        return false;
    if (request.projectDirectory && normalizeContextPath(project.directory) != normalizeContextPath(*request.projectDirectory))
        return false;
    if (request.scriptPath && !containsPath(project.scripts, normalizeContextPath(*request.scriptPath)))
        return false;
    if (request.scriptDirectory && !std::any_of(project.scripts.begin(), project.scripts.end(), [&](const std::string& script)
                                       {
                                           return pathWithin(script, *request.scriptDirectory);
                                       }))
        return false;
    return true;
}

static bool includesSection(const InstanceInspectionRequest& request, std::string_view section)
{
    return request.sections.empty() || std::find(request.sections.begin(), request.sections.end(), section) != request.sections.end();
}

ContextInspectionResult inspectContextInstance(const InstanceInspectionRequest& request, const std::string& stateDirectory)
{
    if (!request.nodeId && !request.instanceId && !request.name)
        return {std::nullopt, "instance inspection requires --node-id, --instance-id, or --name"};
    static const std::set<std::string> validSections{"hierarchy", "position", "scripts"};
    for (const std::string& section : request.sections)
        if (!validSections.count(section))
            return {std::nullopt, "unknown instance inspection section: " + section};

    const ContextStore store(stateDirectory.empty() ? defaultContextStateDirectory() : stateDirectory);
    std::string error;
    const auto catalog = store.load(&error);
    if (!catalog)
        return {std::nullopt, error};

    json projects = json::array();
    size_t matchCount = 0;
    for (const auto& [projectId, project] : catalog->projects)
    {
        if (!projectMatches(project, request))
            continue;
        const auto world = store.loadWorld(projectId);
        if (!world)
            continue;

        std::map<uint64_t, const WorldInstance*> instancesByNode;
        std::map<uint64_t, std::vector<const WorldInstance*>> childrenByParent;
        for (const WorldInstance& instance : world->instances)
        {
            instancesByNode[instance.nodeId] = &instance;
            childrenByParent[instance.parentNodeId].push_back(&instance);
        }

        json matches = json::array();
        for (const WorldInstance& instance : world->instances)
        {
            if (request.nodeId && instance.nodeId != *request.nodeId)
                continue;
            if (request.instanceId && instance.instanceId != *request.instanceId)
                continue;
            if (request.name && instance.name != *request.name)
                continue;

            json value = {{"nodeId", instance.nodeId}, {"instanceId", instance.instanceId}, {"name", instance.name}, {"type", instance.type}, {"shape", instance.shape}};
            if (includesSection(request, "position"))
                value["position"] = instance.position;
            if (includesSection(request, "scripts"))
                value["scripts"] = instance.scripts;
            if (includesSection(request, "hierarchy"))
            {
                if (const auto parent = instancesByNode.find(instance.parentNodeId); parent != instancesByNode.end())
                    value["parent"] = {{"nodeId", parent->second->nodeId}, {"name", parent->second->name}, {"type", parent->second->type}};
                else
                    value["parent"] = nullptr;
                value["children"] = json::array();
                for (const WorldInstance* child : childrenByParent[instance.nodeId])
                    value["children"].push_back({{"nodeId", child->nodeId}, {"instanceId", child->instanceId}, {"name", child->name}, {"type", child->type}});
            }
            matches.push_back(std::move(value));
            ++matchCount;
        }
        if (matches.empty())
            continue;
        projects.push_back({{"id", projectId},
            {"name", project.name},
            {"directory", project.directory},
            {"active", catalog->activeProjectId == projectId},
            {"fresh", project.fresh},
            {"instances", std::move(matches)}});
    }

    std::sort(projects.begin(), projects.end(), [](const json& left, const json& right)
        {
            if (left.value("name", "") != right.value("name", ""))
                return left.value("name", "") < right.value("name", "");
            return left.value("directory", "") < right.value("directory", "");
        });
    return {json{{"kind", "instance"}, {"matchCount", matchCount}, {"projects", std::move(projects)}}, std::nullopt};
}

ContextInspectionResult inspectContextInput(const std::string& path, const std::string& stateDirectory)
{
    const std::filesystem::path input = std::filesystem::u8path(path);
    std::error_code error;
    const bool directory = std::filesystem::is_directory(input, error);
    if (error)
        return {std::nullopt, "could not inspect " + path + ": " + error.message()};
    if (directory || input.filename() == "project.json")
    {
        ProjectFileInspectionResult inspected = inspectProjectFiles(path);
        if (!inspected.project)
            return {std::nullopt, inspected.error.value_or("could not inspect project")};
        const ProjectFileInspection& project = *inspected.project;
        json report = {{"kind", "project"},
            {"inputPath", normalizeContextPath(path)},
            {"projectFile", project.projectFile},
            {"name", project.name},
            {"directory", project.directory},
            {"summary",
                {{"assetCount", project.assetCount},
                    {"scriptAssetCount", project.scriptAssetCount},
                    {"autorunScriptCount", project.autorunScriptCount},
                    {"scriptCount", project.scripts.size()},
                    {"missingScriptCount", project.missingScripts.size()},
                    {"assetTypes", project.assetTypes}}},
            {"declaredScripts", project.declaredScripts},
            {"autorunScripts", project.autorunScripts},
            {"scripts", project.scripts},
            {"missingScripts", project.missingScripts},
            {"attachments", json::array()}};

        const ContextStore store(stateDirectory.empty() ? defaultContextStateDirectory() : stateDirectory);
        const auto catalog = store.load();
        if (catalog)
        {
            for (const auto& [projectId, captured] : catalog->projects)
            {
                if (normalizeContextPath(captured.directory) != normalizeContextPath(project.directory))
                    continue;
                report["capturedContext"] = captured;
                report["capturedContext"]["active"] = catalog->activeProjectId == projectId;
                const auto world = store.loadWorld(projectId);
                if (world)
                {
                    report["capturedContext"]["snapshot"] = {{"tick", world->tick},
                        {"commandSequence", world->commandSequence},
                        {"capturedAtUnixMs", world->capturedAtUnixMs},
                        {"sourcePath", world->sourcePath}};
                    for (const WorldInstance& instance : world->instances)
                    {
                        if (instance.scripts.empty())
                            continue;
                        report["attachments"].push_back({{"nodeId", instance.nodeId},
                            {"instanceId", instance.instanceId},
                            {"name", instance.name},
                            {"type", instance.type},
                            {"scripts", instance.scripts}});
                    }
                }
                break;
            }
        }
        return {std::move(report), std::nullopt};
    }
    if (isLuauFile(input))
        return inspectScript(path, stateDirectory);
    if (!std::filesystem::is_regular_file(input, error))
        return {std::nullopt, "input is not a file or project directory: " + path};
    return inspectWorldDump(path);
}

} // namespace luduvo
