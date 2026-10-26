#include "Platform/luduvo/context/LuduvoContextReport.hpp"

#include <algorithm>
#include <filesystem>
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
        return "none";
    return value.dump();
}

static void renderNode(std::ostringstream& output, const ReportNode& node, const std::string& indentation, bool last, bool root)
{
    if (!root)
        output << indentation << (last ? u8"\u2514\u2500\u2500 " : u8"\u251c\u2500\u2500 ");
    output << node.label;
    for (const std::string& tag : node.tags)
        output << " [" << tag << ']';
    output << '\n';

    const std::string childIndentation = root ? std::string{} : indentation + (last ? "    " : u8"\u2502   ");
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
        result.fields.push_back(
            {"Position", displayValue(position->value("x", 0.0)) + ", " + displayValue(position->value("y", 0.0)) + ", " +
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

static ReportNode buildScriptInspectionReport(const json& inspection)
{
    ReportNode root{"Script: " + inspection.value("inputPath", "<unknown>")};
    root.fields.push_back({"Side", inspection.value("side", "unspecified")});

    std::vector<json> projects;
    if (const auto values = inspection.find("projects"); values != inspection.end() && values->is_array())
        projects.assign(values->begin(), values->end());
    std::sort(projects.begin(), projects.end(), [](const json& left, const json& right)
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
        if (project.value("active", false))
            projectNode.tags.push_back("active in Studio");
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
        root.fields.push_back({"Tick", displayValue(summary->value("tick", 0))});
        root.fields.push_back({"Command sequence", displayValue(summary->value("commandSequence", 0))});
        root.fields.push_back({"Instances", displayValue(summary->value("instanceCount", 0))});
        root.fields.push_back({"Root instances", displayValue(summary->value("rootInstanceCount", 0))});
        root.fields.push_back({"Script attachments", displayValue(summary->value("scriptAttachmentCount", 0))});
    }
    return root;
}

static ReportNode buildProjectReport(const json& inspection)
{
    ReportNode root{"Project: " + inspection.value("name", "<unnamed>")};
    root.fields.push_back({"Directory", inspection.value("directory", "")});
    root.fields.push_back({"Project file", inspection.value("projectFile", "")});
    if (const auto summary = inspection.find("summary"); summary != inspection.end() && summary->is_object())
    {
        root.fields.push_back({"Assets", displayValue(summary->value("assetCount", 0))});
        root.fields.push_back({"Scripts", displayValue(summary->value("scriptCount", 0))});
        root.fields.push_back({"Missing scripts", displayValue(summary->value("missingScriptCount", 0))});
    }
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
        ReportNode captured{"Captured context"};
        if (context->value("active", false))
            captured.tags.push_back("active in Studio");
        captured.tags.push_back(context->value("fresh", false) ? "fresh" : "stale");
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

static ReportNode buildInstanceInspectionReport(const json& inspection)
{
    const auto matches = inspection.find("projects");
    const size_t projectCount = matches != inspection.end() && matches->is_array() ? matches->size() : 0;
    ReportNode root{"Instance matches (" + std::to_string(inspection.value("matchCount", 0)) + ")"};
    if (projectCount == 0)
        return root;

    for (const json& project : *matches)
    {
        const auto instances = project.find("instances");
        const size_t count = instances != project.end() && instances->is_array() ? instances->size() : 0;
        ReportNode projectNode{project.value("name", "<unnamed>") + " (" + std::to_string(count) + ")"};
        if (project.value("active", false))
            projectNode.tags.push_back("active in Studio");
        if (instances != project.end() && instances->is_array())
            for (const json& instance : *instances)
                projectNode.children.push_back(scriptAttachmentNode(instance));
        root.children.push_back(std::move(projectNode));
    }
    return root;
}

ReportNode buildContextInspectionReport(const json& inspection)
{
    const std::string kind = inspection.value("kind", "unknown");
    if (kind == "script")
        return buildScriptInspectionReport(inspection);
    if (kind == "worldDump")
        return buildWorldDumpReport(inspection);
    if (kind == "project")
        return buildProjectReport(inspection);
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

ReportNode buildContextStatusReport(const json& status)
{
    ReportNode root{"Luduvo context"};
    root.tags.push_back(status.value("running", false) ? "running" : "stopped");
    root.fields.push_back({"State directory", status.value("stateDirectory", "")});
    if (const auto catalog = status.find("catalog"); catalog != status.end() && catalog->is_object())
    {
        root.fields.push_back({"Projects", std::to_string(catalog->value("projects", json::object()).size())});
        std::string activeProject = catalog->value("activeProjectId", "");
        if (!activeProject.empty())
        {
            const auto projects = catalog->find("projects");
            if (projects != catalog->end() && projects->is_object())
                if (const auto project = projects->find(activeProject); project != projects->end())
                    activeProject = project->value("name", activeProject);
        }
        root.fields.push_back({"Active project", activeProject.empty() ? "none" : activeProject});
        const auto projects = catalog->find("projects");
        if (projects != catalog->end() && projects->is_object())
        {
            ReportNode projectList{"Projects (" + std::to_string(projects->size()) + ")"};
            for (const auto& [projectId, project] : projects->items())
            {
                ReportNode item{project.value("name", projectId)};
                if (projectId == catalog->value("activeProjectId", ""))
                    item.tags.push_back("active in Studio");
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
    if (const auto cleanup = status.find("cleanup"); cleanup != status.end() && cleanup->is_object())
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

} // namespace luduvo
