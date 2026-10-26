#pragma once

#include "argparse/argparse.hpp"

namespace luduvo
{

class ContextCli
{
    argparse::ArgumentParser contextParser;
    argparse::ArgumentParser runParser;
    argparse::ArgumentParser statusParser;
    argparse::ArgumentParser stopParser;
    argparse::ArgumentParser parseParser;
    argparse::ArgumentParser inspectParser;
    argparse::ArgumentParser updateParser;
    argparse::ArgumentParser cleanParser;
    argparse::ArgumentParser forgetParser;

public:
    ContextCli();

    argparse::ArgumentParser& parser();
    int execute() const;
};

} // namespace luduvo
