#pragma once

#include "Platform/LSPPlatform.hpp"

#include <string>
#include <cstddef>
#include <vector>

class WorkspaceFolder;

struct PlatformDefinitionLoadResult
{
    bool platformDefinitionsActive = false;
    size_t configuredDefinitionFilesLoaded = 0;
    std::vector<std::string> platformWarnings;
};

/// Applies configured and platform definition sources according to one policy.
/// Source selection and conflict resolution live here; platforms remain
/// responsible for explaining platform-specific failures to users.
class PlatformDefinitionLoader
{
    WorkspaceFolder& workspace;

public:
    explicit PlatformDefinitionLoader(WorkspaceFolder& workspace);

    PlatformDefinitionLoadResult load(const PlatformDefinitionConfiguration& configuration, std::vector<PlatformDefinitionEnvironment> environments);
};
