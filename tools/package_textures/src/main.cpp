//
// package_textures.exe
//

#include "package_textures.h"

#include <cstring>

#include <iostream>
#include <vector>
#include <filesystem>
#include <memory>
#include <algorithm>

#include <gcpak/gcpak.h>

#include <stb_image.h>

static bool isImage(const std::filesystem::path& path)
{
    auto ext = path.extension().string();
    std::transform(ext.cbegin(), ext.cend(), ext.begin(), tolower);
    return (ext == ".png") || (ext == ".jpg") || (ext == ".jpeg");
}

// empty on failure
std::vector<uint8_t> readImage(const std::filesystem::path& path)
{
    const auto filename = path.filename();
    int32_t x{}, y{}, channels_in_file{};
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> data(stbi_load(path.string().c_str(), &x, &y, &channels_in_file, 4), stbi_image_free);
    if (!data || x <= 0 || y <= 0) {
        return {};
    }

    // Image asset format:
    // first 4 bytes is width, second 4 bytes is height, remaining data is just R8G8B8A8_SRGB
    const size_t bitmap_size = (static_cast<size_t>(x) * static_cast<size_t>(y) * 4ULL);
    const size_t output_size = 4ULL + 4ULL + bitmap_size;

    static_assert(std::endian::native == std::endian::little);

    std::vector<uint8_t> output(output_size);
    size_t dest_offset{};
    std::memcpy(output.data() + dest_offset, &x, sizeof(uint32_t));
    dest_offset += sizeof(uint32_t);
    std::memcpy(output.data() + dest_offset, &y, sizeof(uint32_t));
    dest_offset += sizeof(uint32_t);
    std::memcpy(output.data() + dest_offset, data.get(), bitmap_size);

    return output;
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    std::error_code ec{};

    const auto texture_dir = std::filesystem::path(PACKAGE_TEXTURES_SOURCE_DIRECTORY).parent_path().parent_path() / "content" / "textures";
    if (!std::filesystem::exists(texture_dir, ec) || !std::filesystem::is_directory(texture_dir, ec)) {
        std::cerr << "Failed to find textures directory! error: " << ec.message() << "\n";
        return EXIT_FAILURE;
    }

    const auto gcpak_path = texture_dir.parent_path() / "textures.gcpak";

    // find all image files and add them
    gcpak::GcpakCreator gcpak_creator{};
    for (const auto& dir_entry : std::filesystem::directory_iterator(texture_dir)) {

        if (!dir_entry.is_regular_file()) {
            continue;
        }

        if (!isImage(dir_entry.path())) {
            continue;
        }

        auto data = readImage(dir_entry.path());
        if (data.empty()) {
            std::cerr << "Failed to read image: " << dir_entry.path().filename() << "\n";
            continue;
        }

        std::cout << "Adding image: " << dir_entry.path().filename() << "\n";
        gcpak_creator.addAsset(gcpak::GcpakCreator::Asset{dir_entry.path().filename().string(), data, gcpak::GcpakAssetType::TEXTURE_R8G8B8A8});
    }

    if (!gcpak_creator.saveFile(gcpak_path)) {
        std::cerr << "Failed to save gcpak file " << gcpak_path.filename() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "Saved textures to " << gcpak_path << "\n";

    { // wait for enter before exit
        std::cout << "Press enter to exit\n";
        int c{};
        do {
            c = getchar();
        } while (c != '\n' && c != EOF);
    }

    return EXIT_SUCCESS;
}
