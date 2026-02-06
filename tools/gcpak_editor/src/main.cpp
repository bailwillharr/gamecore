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

#include <stb_image.h>

#include <gcpak/gcpak.h>
#include <gcpak/gcpak_prefab.h>

int main() { std::cout << gcpak::PrefabComponentTransform::getSerializedSize(); }