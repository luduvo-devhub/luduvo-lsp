#include "Platform/luduvo/context/LuduvoContextReport.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace luduvo
{

using json = nlohmann::json;

static std::string displayValue(const json& value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_boolean())
        return value.get<bool>() ? "yes" : "no";
    if (value.is_null())
        return "unknown";
    return value.dump();
}

static ReportField reportField(std::string name, const json& value)
{
    return {std::move(name), displayValue(value), value.is_null()};
}

static void renderNode(std::ostringstream& output, const ReportNode& node, const std::string& indentation, bool last, bool root)
{
    if (!root)
        output << indentation << (last ? u8"\u2514\u2500\u2500 " : u8"\u251c\u2500\u2500 ");
    output << node.label;
    for (const std::string& tag : node.tags)
        output << " [" << tag << ']';
    output << '\n';

    const std::string childIndentation = root ? std::string{} : indentation + (last ? "    " : "|   ");
    const size_t itemCount = node.fields.size() + node.children.size();
    size_t itemIndex = 0;
    for (const ReportField& field : node.fields)
    {
        const bool itemLast = ++itemIndex == itemCount;
        output << childIndentation << (itemLast ? u8"\u2514\u2500\u2500 " : u8"\u251c\u2500\u2500 ") << field.name << ": " << field.value << '\n';
    }
    for (const ReportNode& child : node.children)
        renderNode(output, child, childIndentation, ++itemIndex == itemCount, false);
}

std::string renderReportText(const ReportNode& root)
{
    std::ostringstream output;
    renderNode(output, root, {}, true, true);
    return output.str();
}

static std::string sectionSlug(std::string_view label)
{
    std::string result;
    bool separator = false;
    for (unsigned char character : label)
    {
        if (character == '(')
            break;
        if (std::isalnum(character))
        {
            if (separator && !result.empty())
                result.push_back('-');
            result.push_back(char(std::tolower(character)));
            separator = false;
        }
        else
            separator = true;
    }
    return result.empty() ? "section" : result;
}

static void assignReportSections(ReportNode& node, bool root = false)
{
    if (root)
        node.section.clear();

    std::map<std::string, size_t> duplicates;
    for (ReportNode& child : node.children)
    {
        std::string sectionName = sectionSlug(child.label);
        if (!child.section.empty())
        {
            const size_t separator = child.section.find_last_of('.');
            sectionName = child.section.substr(separator == std::string::npos ? 0 : separator + 1);
        }
        const std::string base = node.section.empty() ? sectionName : node.section + "." + sectionName;
        const size_t occurrence = ++duplicates[base];
        child.section = occurrence == 1 ? base : base + "-" + std::to_string(occurrence);
        assignReportSections(child);
    }
}

static json reportNodeJson(const ReportNode& node)
{
    json result = {{"section", node.section}, {"label", node.label}, {"tags", node.tags}, {"fields", json::object()}, {"children", json::array()}};
    for (const ReportField& field : node.fields)
        result["fields"][field.name] = field.unknown ? json(nullptr) : json(field.value);
    for (const ReportNode& child : node.children)
        result["children"].push_back(reportNodeJson(child));
    return result;
}

json reportToJson(const ReportNode& report)
{
    ReportNode root = report;
    assignReportSections(root, true);
    return reportNodeJson(root);
}

static void appendReportSections(const ReportNode& node, std::vector<std::pair<std::string, std::string>>& result)
{
    if (!node.section.empty())
        result.emplace_back(node.section, node.label);
    for (const ReportNode& child : node.children)
        appendReportSections(child, result);
}

std::vector<std::pair<std::string, std::string>> listReportSections(ReportNode root)
{
    assignReportSections(root, true);
    std::vector<std::pair<std::string, std::string>> result;
    appendReportSections(root, result);
    return result;
}

static std::optional<ReportNode> filterReportNode(const ReportNode& node, const std::set<std::string>& selected)
{
    if (selected.count(node.section))
        return node;
    ReportNode result = node;
    result.fields.clear();
    result.children.clear();
    for (const ReportNode& child : node.children)
        if (auto filtered = filterReportNode(child, selected))
            result.children.push_back(std::move(*filtered));
    if (result.children.empty())
        return std::nullopt;
    return result;
}

std::optional<ReportNode> selectReportSections(ReportNode root, const std::vector<std::string>& sections, std::string* error)
{
    assignReportSections(root, true);
    if (sections.empty())
        return root;

    const auto available = listReportSections(root);
    std::set<std::string> availableIds;
    for (const auto& [id, _] : available)
        availableIds.insert(id);
    for (const std::string& section : sections)
    {
        if (availableIds.count(section))
            continue;
        if (error)
            *error = "unknown report section '" + section + "'; use --list-sections to see valid section IDs";
        return std::nullopt;
    }
    return filterReportNode(root, std::set<std::string>(sections.begin(), sections.end()));
}

static ReportNode scriptAttachmentNode(const json& attachment)
{
    ReportNode result{attachment.value("name", "<unnamed>")};
    if (const auto type = attachment.find("type"); type != attachment.end() && type->is_string() && !type->get_ref<const std::string&>().empty())
        result.tags.push_back(type->get<std::string>());
    if (const auto nodeId = attachment.find("nodeId"); nodeId != attachment.end())
        result.tags.push_back("node " + displayValue(*nodeId));
    if (const auto instanceId = attachment.find("instanceId"); instanceId != attachment.end())
        result.fields.push_back({"Instance ID", displayValue(*instanceId)});
    if (const auto position = attachment.find("position"); position != attachment.end() && position->is_object())
        result.fields.push_back({"Position", displayValue(position->value("x", 0.0)) + ", " + displayValue(position->value("y", 0.0)) + ", " +
                                                 displayValue(position->value("z", 0.0))});
    if (const auto parent = attachment.find("parent"); parent != attachment.end() && parent->is_object())
    {
        ReportNode parentNode{"Parent: " + parent->value("name", "<unnamed>")};
        parentNode.tags.push_back(parent->value("type", "<unknown>"));
        parentNode.tags.push_back("node " + std::to_string(parent->value("nodeId", uint64_t(0))));
        result.children.push_back(std::move(parentNode));
    }
    if (const auto children = attachment.find("children"); children != attachment.end() && children->is_array())
    {
        ReportNode childList{"Children (" + std::to_string(children->size()) + ")"};
        for (const json& child : *children)
            childList.children.push_back(scriptAttachmentNode(child));
        result.children.push_back(std::move(childList));
    }
    if (const auto scripts = attachment.find("scripts"); scripts != attachment.end() && scripts->is_array())
    {
        ReportNode scriptList{"Scripts (" + std::to_string(scripts->size()) + ")"};
        for (const json& script : *scripts)
            scriptList.children.push_back({displayValue(script)});
        result.children.push_back(std::move(scriptList));
    }
    return result;
}

static ReportNode buildProjectReport(const json& inspection, bool promoteContextTags);

static ReportNode buildScriptInspectionReport(const json& inspection)
{
    ReportNode root{"Script: " + inspection.value("inputPath", "<unknown>")};
    ReportNode summary{"Summary"};
    summary.section = "summary";
    summary.fields.push_back({"Side", inspection.value("side", "unspecified")});
    summary.fields.push_back({"Matching projects", std::to_string(inspection.value("projectCount", size_t(0)))});
    root.children.push_back(std::move(summary));

    std::vector<json> projects;
    if (const auto values = inspection.find("projects"); values != inspection.end() && values->is_array())
        projects.assign(values->begin(), values->end());
    std::sort(projects.begin(), projects.end(),
        [](const json& left, const json& right)
        {
            const std::string leftName = left.value("name", "");
            const std::string rightName = right.value("name", "");
            if (leftName != rightName)
                return leftName < rightName;
            return left.value("directory", "") < right.value("directory", "");
        });

    ReportNode projectList{"Projects (" + std::to_string(projects.size()) + ")"};
    ReportNode attachments{"Attached instances (0)"};
    size_t attachmentCount = 0;
    for (const json& project : projects)
    {
        ReportNode projectNode{project.value("name", "<unnamed>")};
        if (project.value("selected", false))
            projectNode.tags.push_back("active in LSP");
        projectNode.tags.push_back(project.value("fresh", false) ? "fresh" : "stale");
        projectNode.fields.push_back({"Directory", project.value("directory", "")});
        projectList.children.push_back(std::move(projectNode));

        const auto projectAttachments = project.find("attachments");
        if (projectAttachments == project.end() || !projectAttachments->is_array() || projectAttachments->empty())
            continue;
        ReportNode attachmentProject{project.value("name", "<unnamed>") + " (" + std::to_string(projectAttachments->size()) + ")"};
        for (const json& attachment : *projectAttachments)
            attachmentProject.children.push_back(scriptAttachmentNode(attachment));
        attachmentCount += projectAttachments->size();
        attachments.children.push_back(std::move(attachmentProject));
    }
    attachments.label = "Attached instances (" + std::to_string(attachmentCount) + ")";
    root.children.push_back(std::move(projectList));
    root.children.push_back(std::move(attachments));
    return root;
}

static ReportNode buildWorldDumpReport(const json& inspection)
{
    ReportNode root{"World dump: " + inspection.value("inputPath", "<unknown>")};
    if (const auto summary = inspection.find("summary"); summary != inspection.end() && summary->is_object())
    {
        ReportNode summaryNode{"Summary"};
        summaryNode.section = "summary";
        summaryNode.fields.push_back({"Tick", displayValue(summary->value("tick", 0))});
        summaryNode.fields.push_back({"Command sequence", displayValue(summary->value("commandSequence", 0))});
        summaryNode.fields.push_back({"Instances", displayValue(summary->value("instanceCount", 0))});
        summaryNode.fields.push_back({"Root instances", displayValue(summary->value("rootInstanceCount", 0))});
        summaryNode.fields.push_back({"Script attachments", displayValue(summary->value("scriptAttachmentCount", 0))});
        root.children.push_back(std::move(summaryNode));
    }
    if (const auto instances = inspection.find("instances"); instances != inspection.end() && instances->is_array())
    {
        std::map<uint64_t, std::vector<const json*>> children;
        for (const json& instance : *instances)
            children[instance.value("parentNodeId", uint64_t(0))].push_back(&instance);
        std::function<ReportNode(const json&)> buildInstance = [&](const json& instance)
        {
            ReportNode node = scriptAttachmentNode(instance);
            for (const json* child : children[instance.value("nodeId", uint64_t(0))])
                node.children.push_back(buildInstance(*child));
            return node;
        };
        ReportNode hierarchy{"Instances (" + std::to_string(instances->size()) + ")"};
        hierarchy.section = "instances";
        for (const json* instance : children[0])
            hierarchy.children.push_back(buildInstance(*instance));
        root.children.push_back(std::move(hierarchy));
    }
    if (const auto scripts = inspection.find("scripts"); scripts != inspection.end() && scripts->is_array())
    {
        ReportNode scriptList{"Scripts (" + std::to_string(scripts->size()) + ")"};
        scriptList.section = "scripts";
        for (const json& script : *scripts)
            scriptList.children.push_back({displayValue(script)});
        root.children.push_back(std::move(scriptList));
    }
    if (const auto projects = inspection.find("projects"); projects != inspection.end() && projects->is_array() && !projects->empty())
    {
        ReportNode projectList{"Associated projects (" + std::to_string(projects->size()) + ")"};
        projectList.section = "projects";
        for (const json& project : *projects)
        {
            ReportNode projectNode = buildProjectReport(project, true);
            projectNode.label = project.value("name", "<unnamed>");
            projectList.children.push_back(std::move(projectNode));
        }
        root.children.push_back(std::move(projectList));
    }
    return root;
}

static ReportNode buildProjectReport(const json& inspection, bool promoteContextTags = false)
{
    ReportNode root{"Project: " + inspection.value("name", "<unnamed>")};
    ReportNode summaryNode{"Summary"};
    summaryNode.section = "summary";
    summaryNode.fields.push_back(reportField("Directory", inspection.value("directory", json(nullptr))));
    summaryNode.fields.push_back(reportField("Project file", inspection.value("projectFile", json(nullptr))));
    if (const auto summary = inspection.find("summary"); summary != inspection.end() && summary->is_object())
    {
        summaryNode.fields.push_back(reportField("Assets", summary->value("assetCount", json(nullptr))));
        summaryNode.fields.push_back(reportField("Scripts", summary->value("scriptCount", json(nullptr))));
        summaryNode.fields.push_back(reportField("Missing scripts", summary->value("missingScriptCount", json(nullptr))));
    }
    root.children.push_back(std::move(summaryNode));
    ReportNode scripts{"Project scripts"};
    const auto addScripts = [&](const char* field, const char* label, const char* tag)
    {
        const auto values = inspection.find(field);
        if (values == inspection.end() || !values->is_array() || values->empty())
            return;
        ReportNode group{std::string(label) + " (" + std::to_string(values->size()) + ")"};
        for (const json& value : *values)
        {
            const std::string path = displayValue(value);
            ReportNode script{std::filesystem::u8path(path).filename().generic_u8string()};
            script.tags.push_back(tag);
            script.fields.push_back({"Path", path});
            group.children.push_back(std::move(script));
        }
        scripts.children.push_back(std::move(group));
    };
    addScripts("declaredScripts", "Declared", "project asset");
    addScripts("autorunScripts", "Autorun", "autorun");
    addScripts("missingScripts", "Missing", "missing");
    if (!scripts.children.empty())
        root.children.push_back(std::move(scripts));
    if (const auto context = inspection.find("capturedContext"); context != inspection.end() && context->is_object())
    {
        if (promoteContextTags)
        {
            root.tags.push_back(context->value("fresh", false) ? "fresh" : "stale");
        }
        ReportNode captured{"Captured context"};
        if (!promoteContextTags)
        {
            captured.tags.push_back(context->value("fresh", false) ? "fresh" : "stale");
        }
        captured.fields.push_back({"ID", context->value("id", "")});
        captured.fields.push_back({"Instances", std::to_string(context->value("instanceCount", size_t(0)))});
        captured.fields.push_back({"Script attachments", std::to_string(context->value("scriptAttachmentCount", size_t(0)))});
        if (!context->value("staleReason", "").empty())
            captured.fields.push_back({"Stale reason", context->value("staleReason", "")});
        root.children.push_back(std::move(captured));
    }
    if (const auto attachments = inspection.find("attachments"); attachments != inspection.end() && attachments->is_array() && !attachments->empty())
    {
        ReportNode attachmentList{"Attached instances (" + std::to_string(attachments->size()) + ")"};
        for (const json& attachment : *attachments)
            attachmentList.children.push_back(scriptAttachmentNode(attachment));
        root.children.push_back(std::move(attachmentList));
    }
    return root;
}

static ReportNode buildProjectMatchesReport(const json& inspection)
{
    const auto projects = inspection.find("projects");
    const size_t matchCount = projects != inspection.end() && projects->is_array() ? projects->size() : 0;
    ReportNode root{"Project matches: " + inspection.value("selector", "") + " (" + std::to_string(matchCount) + ")"};
    ReportNode summary{"Summary"};
    summary.section = "summary";
    summary.fields.push_back({"Matches", std::to_string(matchCount)});
    root.children.push_back(std::move(summary));
    if (projects == inspection.end() || !projects->is_array())
        return root;
    for (const json& project : *projects)
    {
        ReportNode child = buildProjectReport(project, true);
        child.label = project.value("name", "<unnamed>");
        root.children.push_back(std::move(child));
    }
    return root;
}

static ReportNode buildInstanceInspectionReport(const json& inspection)
{
    const auto matches = inspection.find("projects");
    const size_t projectCount = matches != inspection.end() && matches->is_array() ? matches->size() : 0;
    ReportNode root{"Instance matches (" + std::to_string(inspection.value("matchCount", 0)) + ")"};
    ReportNode summary{"Summary"};
    summary.section = "summary";
    summary.fields.push_back({"Matches", std::to_string(inspection.value("matchCount", 0))});
    summary.fields.push_back({"Projects", std::to_string(projectCount)});
    root.children.push_back(std::move(summary));
    if (projectCount == 0)
        return root;

    for (const json& project : *matches)
    {
        const auto instances = project.find("instances");
        const size_t count = instances != project.end() && instances->is_array() ? instances->size() : 0;
        ReportNode projectNode{project.value("name", "<unnamed>") + " (" + std::to_string(count) + ")"};
        if (instances != project.end() && instances->is_array())
            for (const json& instance : *instances)
                projectNode.children.push_back(scriptAttachmentNode(instance));
        root.children.push_back(std::move(projectNode));
    }
    return root;
}

static ReportNode buildContextOverviewReport(const json& inspection);

ReportNode buildContextInspectionReport(const json& inspection)
{
    const std::string kind = inspection.value("kind", "unknown");
    if (kind == "context")
        return buildContextOverviewReport(inspection);
    if (kind == "script")
        return buildScriptInspectionReport(inspection);
    if (kind == "worldDump")
        return buildWorldDumpReport(inspection);
    if (kind == "project")
        return buildProjectReport(inspection);
    if (kind == "projects")
        return buildProjectMatchesReport(inspection);
    if (kind == "instance")
        return buildInstanceInspectionReport(inspection);
    return ReportNode{"Context inspection", {}, {{"Data", inspection.dump()}}};
}

static ReportNode candidateGroup(const std::string& label, const json& values)
{
    ReportNode group{label + " (" + std::to_string(values.is_array() ? values.size() : 0) + ")"};
    if (!values.is_array())
        return group;
    for (const json& value : values)
    {
        ReportNode item{value.value("label", value.value("id", "<unknown>"))};
        if (const std::string id = value.value("id", ""); !id.empty() && id != item.label)
            item.fields.push_back({"ID", id});
        if (const std::string path = value.value("path", ""); !path.empty())
            item.fields.push_back({"Path", path});
        group.children.push_back(std::move(item));
    }
    return group;
}

static ReportNode buildContextOverviewReport(const json& inspection)
{
    ReportNode root{"Project context"};
    ReportNode summary{"Summary"};
    summary.section = "summary";
    summary.fields.push_back({"State directory", inspection.value("stateDirectory", "")});
    if (const auto catalog = inspection.find("catalog"); catalog != inspection.end() && catalog->is_object())
    {
        summary.fields.push_back({"Projects", std::to_string(catalog->value("projects", json::object()).size())});
        const auto projects = catalog->find("projects");
        if (projects != catalog->end() && projects->is_object())
        {
            ReportNode projectList{"Projects (" + std::to_string(projects->size()) + ")"};
            for (const auto& entry : projects->items())
            {
                const auto& projectId = entry.key();
                const auto& project = entry.value();
                ReportNode item{project.value("name", projectId)};
                item.tags.push_back(project.value("fresh", false) ? "fresh" : "stale");
                item.fields.push_back({"ID", projectId});
                item.fields.push_back({"Directory", project.value("directory", "")});
                if (!project.value("staleReason", "").empty())
                    item.fields.push_back({"Stale reason", project.value("staleReason", "")});
                if (!project.value("dumpPath", "").empty())
                    item.fields.push_back({"World dump", project.value("dumpPath", "")});
                item.fields.push_back({"Instances", std::to_string(project.value("instanceCount", size_t(0)))});
                item.fields.push_back({"Script attachments", std::to_string(project.value("scriptAttachmentCount", size_t(0)))});

                const auto addScripts = [&](const char* field, const char* label)
                {
                    const json values = project.value(field, json::array());
                    if (!values.is_array() || values.empty())
                        return;
                    ReportNode group{std::string(label) + " (" + std::to_string(values.size()) + ")"};
                    for (const json& value : values)
                    {
                        const std::string path = displayValue(value);
                        ReportNode script{std::filesystem::u8path(path).filename().generic_u8string()};
                        script.fields.push_back({"Path", path});
                        group.children.push_back(std::move(script));
                    }
                    item.children.push_back(std::move(group));
                };
                addScripts("declaredScripts", "Declared scripts");
                addScripts("autorunScripts", "Autorun scripts");
                addScripts("worldScripts", "World scripts");
                addScripts("attachedScripts", "Inferred scripts");
                projectList.children.push_back(std::move(item));
            }
            root.children.push_back(std::move(projectList));
        }
    }
    root.children.insert(root.children.begin(), std::move(summary));
    if (const auto cleanup = inspection.find("cleanup"); cleanup != inspection.end() && cleanup->is_object())
    {
        size_t total = 0;
        for (const auto& [_, values] : cleanup->items())
            if (values.is_array())
                total += values.size();
        ReportNode candidates{"Cleanup candidates (" + std::to_string(total) + ")"};
        candidates.children.push_back(candidateGroup("Dead clients", cleanup->value("clients", json::array())));
        candidates.children.push_back(candidateGroup("Missing projects", cleanup->value("projects", json::array())));
        candidates.children.push_back(candidateGroup("Orphaned snapshots", cleanup->value("snapshots", json::array())));
        root.children.push_back(std::move(candidates));
    }
    return root;
}

ReportNode buildContextStatusReport(const json& status)
{
    ReportNode root{"Luduvo context helper"};
    root.tags.push_back(status.value("running", false) ? "running" : "stopped");
    root.fields.push_back({"State directory", status.value("stateDirectory", "")});
    root.fields.push_back({"Catalog", status.value("catalogPath", "")});
    root.fields.push_back({"Catalog generation", std::to_string(status.value("catalogGeneration", uint64_t(0)))});
    root.fields.push_back({"Known projects", std::to_string(status.value("projectCount", size_t(0)))});
    if (const auto configuration = status.find("configuration"); configuration != status.end() && configuration->is_object())
    {
        ReportNode settings{"Configuration"};
        settings.fields.push_back({"Enabled", configuration->value("enabled", false) ? "yes" : "no"});
        settings.fields.push_back({"Automatic dumps", configuration->value("autoDump", true) ? "enabled" : "disabled"});
        settings.fields.push_back({"Lazy refresh", configuration->value("lazyRefresh", true) ? "enabled" : "disabled"});
        root.children.push_back(std::move(settings));
    }
    if (status.value("running", false))
        root.fields.push_back({"Helper process", "PID " + std::to_string(status.value("helperProcessId", uint64_t(0)))});
    else
        root.fields.push_back({"Helper process", "none"});

    const json clients = status.value("clients", json::array());
    ReportNode clientList{"LSP clients (" + std::to_string(clients.is_array() ? clients.size() : 0) + ")"};
    if (clients.is_array())
        for (const json& client : clients)
        {
            ReportNode clientNode{"PID " + std::to_string(client.value("processId", uint64_t(0)))};
            clientNode.tags.push_back(client.value("live", false) ? "live" : "dead");
            const json projects = client.value("projects", json::array());
            ReportNode projectList{"Projects (" + std::to_string(projects.is_array() ? projects.size() : 0) + ")"};
            if (projects.is_array())
                for (const json& project : projects)
                {
                    ReportNode projectNode{project.value("name", project.value("id", "<unknown>"))};
                    projectNode.fields.push_back({"ID", project.value("id", "")});
                    if (!project.value("directory", "").empty())
                        projectNode.fields.push_back({"Directory", project.value("directory", "")});
                    projectList.children.push_back(std::move(projectNode));
                }
            clientNode.children.push_back(std::move(projectList));
            clientList.children.push_back(std::move(clientNode));
        }
    root.children.push_back(std::move(clientList));

    const size_t deadClients = status.value("deadClientCount", size_t(0));
    const size_t missingProjects = status.value("missingProjectCount", size_t(0));
    const size_t orphanedSnapshots = status.value("orphanedSnapshotCount", size_t(0));
    ReportNode cleanup{"Cleanup candidates (" + std::to_string(deadClients + missingProjects + orphanedSnapshots) + ")"};
    cleanup.fields.push_back({"Dead clients", std::to_string(deadClients)});
    cleanup.fields.push_back({"Missing projects", std::to_string(missingProjects)});
    cleanup.fields.push_back({"Orphaned snapshots", std::to_string(orphanedSnapshots)});
    root.children.push_back(std::move(cleanup));
    return root;
}

} // namespace luduvo
