#include "Platform/luduvo/context/LuduvoAutoDump.hpp"

#include <algorithm>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::cerr << "Usage: luduvo-context-editor-smoke LOCAL_DIRECTORY PROJECT_DIRECTORY OUTPUT_DUMP\n";
        return 2;
    }
    const auto result =
        luduvo::runAutomaticWorldDump(
            {"smoke", argv[2], argv[3], luduvo::defaultLuduvoEditorPath(argv[1]), std::chrono::seconds(60), luduvo::DumpMethod::Studio});
    if (!result.success || !result.snapshot)
    {
        std::cerr << result.error << '\n';
        return 1;
    }
    const auto& instances = result.snapshot->instances;
    if (std::none_of(instances.begin(), instances.end(),
            [](const auto& instance)
            {
                return instance.name == "LspSmokeFixture";
            }))
    {
        std::cerr << "The real editor dump did not contain LspSmokeFixture\n";
        return 1;
    }
    std::cout << "Real editor autodump passed: " << instances.size() << " instances, including LspSmokeFixture\n";
}
