#pragma once

#include "nlohmann/json.hpp"

#include <string>
#include <optional>
#include <vector>

namespace luduvo
{

struct ReportField
{
    std::string name;
    std::string value;
    bool unknown = false;
    std::string language;
};

struct ReportNode
{
    std::string label;
    std::vector<std::string> tags;
    std::vector<ReportField> fields;
    std::vector<ReportNode> children;
    std::string section;
};

std::string renderReportText(const ReportNode& root);
std::string renderReportMarkdown(const ReportNode& root);
nlohmann::json reportToJson(const ReportNode& root);
std::vector<std::pair<std::string, std::string>> listReportSections(ReportNode root);
std::optional<ReportNode> selectReportSections(ReportNode root, const std::vector<std::string>& sections, std::string* error = nullptr);
ReportNode buildContextInspectionReport(const nlohmann::json& inspection);
ReportNode buildContextStatusReport(const nlohmann::json& status);
ReportNode buildProjectSchemaReport(const nlohmann::json& schema);

} // namespace luduvo
