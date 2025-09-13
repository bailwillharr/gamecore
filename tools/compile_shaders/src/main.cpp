#include <cstdint>

#include <atomic>
#include <array>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <map>
#include <vector>
#include <format>
#include <span>

int main(int argc, char* argv[])
{
    std::cout << "Bonjour le monde!\n";
    for (auto arg : std::span(argv, argc)) {
        std::cout << "Argument: " << arg << "\n";
    }
    return 0;
}
