// Tiny isolated CLI for pipeline_library_loader_test.py. Link with pipeline_library.cpp.
#define NOMINMAX
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include "pipeline_library.h"

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: loader_test LIBRARY PRIMARY PROFILE EXPECT_OPEN_0_OR_1\n";
        return 2;
    }
    if (_putenv_s("ISS_PIPELINE_LIBRARY", argv[1]) != 0) {
        std::cerr << "could not set ISS_PIPELINE_LIBRARY\n";
        return 2;
    }
    std::ifstream file{std::filesystem::path{argv[3]}, std::ios::binary};
    if (!file) {
        std::cerr << "could not open profile\n";
        return 2;
    }
    std::vector<u8> profile{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    const bool actual = iss::pipeline_library::Open(profile, std::filesystem::path{argv[2]});
    const bool expected = std::string{argv[4]} == "1";
    std::cout << "opened=" << actual << " expected=" << expected << '\n';
    if (actual) iss::pipeline_library::Finish(0, 0);
    return actual == expected ? 0 : 1;
}
