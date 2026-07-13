#include "luma/cli/commands.hpp"

int main(int argc, char** argv) {
    return luma::cli::run(argc, argv).exit_code;
}
