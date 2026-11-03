#include <string>
#include <vector>

#include "cli.hpp"

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index) args.emplace_back(argv[index]);
    return keyloom::run_cli(args);
}
