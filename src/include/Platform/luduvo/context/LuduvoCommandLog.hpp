#pragma once
#include <string>
#include <vector>

namespace luduvo
{
struct CommandLogChanges
{
    std::vector<std::string> lines;
    bool rewritten = false;
};
// Studio processes can overwrite earlier bytes without shrinking the journal.
// Retain the observed contents, not an assumed append-only byte offset.
class CommandLogReader
{
    std::string path;
    std::string previous;
    bool unavailable = false;

public:
    explicit CommandLogReader(std::string path, bool startAtEnd = false);
    CommandLogChanges readLines();
};
} // namespace luduvo
