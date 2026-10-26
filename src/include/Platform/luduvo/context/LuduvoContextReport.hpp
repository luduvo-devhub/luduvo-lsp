#pragma once

#include "nlohmann/json.hpp"

#include <string>
#include <vector>

namespace luduvo
{

struct ReportField
{
    std::string name;
    std::string value;
};

struct ReportNode
{
    std::string label;
    std::vector<std::string> tags;
    std::vector<ReportField> fields;
    std::vector<ReportNode> children;
};

std::string renderReportText(const ReportNode& root);
ReportNode buildContextInspectionReport(const nlohmann::json& inspection);
ReportNode buildContextStatusReport(const nlohmann::json& status);

} // namespace luduvo
