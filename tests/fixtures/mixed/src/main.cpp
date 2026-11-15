#include "twice.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    std::printf("%s %d\n", APP_GREETING, twice(21));
    for (int i = 1; i < argc; ++i) {
        std::printf("arg:%s\n", argv[i]);
    }
    // Exit with the value of the first argument so the test can check that
    // `luma run` forwards exit codes.
    return argc > 1 ? std::atoi(argv[1]) : 0;
}
