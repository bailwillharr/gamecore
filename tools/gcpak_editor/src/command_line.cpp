#define _CRT_SECURE_NO_WARNINGS

#include "command_line.h"

#include <cstdio>
#include <cstdlib>

#include <charconv>
#include <filesystem>
#include <format>
#include <system_error>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include <gcpak/gcpak.h>

#include <gclog/gclog.h>

#include <gamecore/gc_units.h>

#include "asset_compiler.h"
#include "prefab_shadow_baker.h"

static const char* const USAGE =
    "usage:\n"
    "  gcpak_editor                      open the editor\n"
    "  gcpak_editor FILE.gcpak           open the editor with a package file\n"
    "  gcpak_editor --pack PATH... [--out FILE.gcpak] [--append] [--no-recurse]\n"
    "                                    compile source files, or every source file in a directory, into a package file\n"
    "  gcpak_editor --list FILE.gcpak    list the assets in a package file\n"
    "  gcpak_editor --bake-shadows PREFAB FILE.gcpak... [--resolution N] [--no-cast ENTITY]...\n"
    "                                    bake the shadow map of a prefab that is in one of the package files\n"
    "See README for more.";

static const char* getAssetTypeName(gcpak::GcpakAssetType type)
{
    switch (type) {
    case gcpak::GcpakAssetType::SPIRV_SHADER:
        return "shader";
    case gcpak::GcpakAssetType::TEXTURE_R8G8B8A8:
        return "texture";
    case gcpak::GcpakAssetType::TEXTURE_R8G8B8A8_SRGB:
        return "texture (sRGB)";
    case gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16:
        return "mesh";
    case gcpak::GcpakAssetType::PREFAB:
        return "prefab";
    case gcpak::GcpakAssetType::MATERIAL:
        return "material";
    case gcpak::GcpakAssetType::SHADOW_MAP_R16:
        return "shadow map";
    default:
        return "invalid";
    }
}

// The editor is a windowed program, so on Windows it has no console of its own. If it was started from one, write to that.
static void attachToParentConsole()
{
#ifdef _WIN32
    const HANDLE existing = GetStdHandle(STD_OUTPUT_HANDLE);
    if (existing != nullptr && existing != INVALID_HANDLE_VALUE) {
        return; // output is redirected to a file or a pipe
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        return;
    }
    const HANDLE console = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (console != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_OUTPUT_HANDLE, console);
        SetStdHandle(STD_ERROR_HANDLE, console);
    }
    (void)std::freopen("CONOUT$", "w", stdout);
    (void)std::freopen("CONOUT$", "w", stderr);
#endif
}

template <typename... Args>
static void print(std::format_string<Args...> fmt, Args&&... args)
{
    const std::string line = std::format(fmt, std::forward<Args>(args)...) + "\n";
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
}

static int listFile(const std::filesystem::path& path)
{
    gcpak::GcpakCreator creator{};
    std::error_code ec{};
    if (!creator.loadFile(path, ec)) {
        print("Failed to load {} (or the .txt file that goes with it): {}", path.string(), ec.message());
        return EXIT_FAILURE;
    }
    size_t total_size{};
    for (const Asset& asset : creator.getAssets()) {
        print("{:08x}  {:<14}  {:>10}  {}", getAssetId(asset), getAssetTypeName(asset.type), gc::bytesToHumanReadable(asset.data.size()), asset.name);
        total_size += asset.data.size();
    }
    print("{} assets, {}", creator.getAssets().size(), gc::bytesToHumanReadable(total_size));
    return EXIT_SUCCESS;
}

static int pack(const std::vector<std::filesystem::path>& inputs, std::filesystem::path output, bool append, bool recursive)
{
    if (inputs.empty()) {
        print("--pack needs at least one file or directory\n{}", USAGE);
        return EXIT_FAILURE;
    }

    std::error_code ec{};
    if (output.empty()) {
        // content/textures becomes content/textures.gcpak
        std::filesystem::path directory = std::filesystem::absolute(inputs[0], ec).lexically_normal();
        if (!directory.has_filename()) {
            directory = directory.parent_path(); // the path ended with a separator
        }
        if (inputs.size() != 1 || !std::filesystem::is_directory(directory, ec)) {
            print("--out is needed unless a single directory is packed\n{}", USAGE);
            return EXIT_FAILURE;
        }
        output = directory;
        output += ".gcpak";
    }

    std::vector<Asset> assets{};
    if (append && std::filesystem::exists(output, ec)) {
        gcpak::GcpakCreator existing{};
        if (!existing.loadFile(output, ec)) {
            print("Failed to load {} (or the .txt file that goes with it): {}", output.string(), ec.message());
            return EXIT_FAILURE;
        }
        assets.assign(existing.getAssets().begin(), existing.getAssets().end());
    }

    CompileStats total{};
    for (const auto& input : inputs) {
        const CompileStats stats = compilePath(input, recursive, assets);
        total.files_compiled += stats.files_compiled;
        total.files_failed += stats.files_failed;
        total.assets_added += stats.assets_added;
        total.assets_replaced += stats.assets_replaced;
    }

    if (assets.empty()) {
        print("Nothing to package");
        return EXIT_FAILURE;
    }

    gcpak::GcpakCreator creator{};
    size_t total_size{};
    for (const Asset& asset : assets) {
        creator.addAsset(asset);
        total_size += asset.data.size();
    }
    if (!creator.saveFile(output)) {
        print("Failed to save {}", output.string());
        return EXIT_FAILURE;
    }

    print("Saved {} assets ({}) to {}", assets.size(), gc::bytesToHumanReadable(total_size), output.string());
    print("{} files compiled, {} failed. {} assets added, {} replaced", total.files_compiled, total.files_failed, total.assets_added, total.assets_replaced);
    return (total.files_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

// Bakes the shadow map of a prefab that is already in a package file, and saves that file again with the shadow map in it.
// The first input is the name of the prefab. The rest are package files: the prefab is in one of them, and the meshes,
// materials and textures that it uses can be in any of them.
static int bakeShadows(const std::vector<std::filesystem::path>& inputs, const PrefabShadowOptions& options)
{
    if (inputs.size() < 2) {
        print("--bake-shadows needs the name of a prefab and at least one package file\n{}", USAGE);
        return EXIT_FAILURE;
    }
    const std::string prefab_name = inputs[0].string();
    const gc::Name prefab_id(gc::crc32_impl(prefab_name.c_str()));

    struct Package {
        std::filesystem::path path{};
        std::vector<Asset> assets{};
    };
    std::vector<Package> packages{};
    for (size_t i = 1; i < inputs.size(); ++i) {
        gcpak::GcpakCreator creator{};
        std::error_code ec{};
        if (!creator.loadFile(inputs[i], ec)) {
            print("Failed to load {} (or the .txt file that goes with it): {}", inputs[i].string(), ec.message());
            return EXIT_FAILURE;
        }
        packages.push_back(Package{inputs[i], std::vector<Asset>(creator.getAssets().begin(), creator.getAssets().end())});
    }

    // If an asset is in several of the files, the first one is used
    Package* prefab_package{};
    Asset* prefab{};
    std::unordered_map<gc::Name, const Asset*> assets_by_id{};
    for (Package& package : packages) {
        for (Asset& asset : package.assets) {
            const gc::Name id(getAssetId(asset));
            if (assets_by_id.try_emplace(id, &asset).second && id == prefab_id && asset.type == gcpak::GcpakAssetType::PREFAB) {
                prefab_package = &package;
                prefab = &asset;
            }
        }
    }
    if (!prefab) {
        print("There is no prefab named {} in the package file{}", prefab_name, packages.size() == 1 ? "" : "s");
        return EXIT_FAILURE;
    }

    const AssetLookup find_asset = [&](gc::Name id) -> const Asset* {
        const auto it = assets_by_id.find(id);
        return (it != assets_by_id.end()) ? it->second : nullptr;
    };
    Asset shadow_map{};
    switch (bakePrefabShadows(prefab_name, prefab->data, find_asset, options, shadow_map)) {
    case PrefabShadowResult::BAKED:
        break;
    case PrefabShadowResult::NO_LIGHT:
        print("{} has no directional light, so it has no shadows to bake", prefab_name);
        return EXIT_FAILURE;
    case PrefabShadowResult::NO_CASTERS:
        print("Nothing in {} casts a shadow. Are its meshes in the package files that were given?", prefab_name);
        return EXIT_FAILURE;
    case PrefabShadowResult::CORRUPT:
        print("{} is corrupt", prefab_name);
        return EXIT_FAILURE;
    }

    const std::string shadow_map_name = shadow_map.name;
    const size_t shadow_map_size = shadow_map.data.size();
    CompileStats stats{};
    std::vector<Asset> new_assets{};
    new_assets.push_back(std::move(shadow_map));
    mergeAssets(prefab_package->assets, std::move(new_assets), stats); // (the pointers into the package's assets are no use after this)

    gcpak::GcpakCreator creator{};
    for (const Asset& asset : prefab_package->assets) {
        creator.addAsset(asset);
    }
    if (!creator.saveFile(prefab_package->path)) {
        print("Failed to save {}", prefab_package->path.string());
        return EXIT_FAILURE;
    }
    print("Baked {} ({}) and saved it and {} to {}", shadow_map_name, gc::bytesToHumanReadable(shadow_map_size), prefab_name,
          prefab_package->path.string());
    return EXIT_SUCCESS;
}

bool isCommandLineRequest(std::span<const std::string> args) { return !args.empty() && args[0].starts_with("-"); }

int runCommandLine(std::span<const std::string> args)
{
    attachToParentConsole();

    enum class Mode { NONE, PACK, LIST, BAKE_SHADOWS } mode{Mode::NONE};
    std::vector<std::filesystem::path> inputs{};
    std::filesystem::path output{};
    bool append{false};
    bool recursive{true};
    PrefabShadowOptions shadow_options{};

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--pack") {
            mode = Mode::PACK;
        }
        else if (arg == "--list") {
            mode = Mode::LIST;
        }
        else if (arg == "--bake-shadows") {
            mode = Mode::BAKE_SHADOWS;
        }
        else if (arg == "--resolution") {
            // the size of the shadow map, in texels. A 16384x16384 shadow map is 512 MB
            const std::string value = (i + 1 < args.size()) ? args[++i] : std::string{};
            uint32_t resolution{};
            const char* const last = value.data() + value.size();
            if (value.empty() || std::from_chars(value.data(), last, resolution).ptr != last || resolution < 16 || resolution > 16384) {
                print("{} needs a number from 16 to 16384\n{}", arg, USAGE);
                return EXIT_FAILURE;
            }
            shadow_options.resolution = resolution;
        }
        else if (arg == "--no-cast") {
            if (i + 1 >= args.size()) {
                print("{} needs the name of an entity\n{}", arg, USAGE);
                return EXIT_FAILURE;
            }
            shadow_options.excluded_entities.emplace_back(gc::crc32_impl(args[++i].c_str()));
        }
        else if (arg == "--out" || arg == "-o") {
            if (i + 1 >= args.size()) {
                print("{} needs a file name\n{}", arg, USAGE);
                return EXIT_FAILURE;
            }
            output = args[++i];
        }
        else if (arg == "--append") {
            append = true;
        }
        else if (arg == "--no-recurse") {
            recursive = false;
        }
        else if (arg == "--help" || arg == "-h") {
            print("{}", USAGE);
            return EXIT_SUCCESS;
        }
        else if (arg.starts_with("-")) {
            print("Unknown option {}\n{}", arg, USAGE);
            return EXIT_FAILURE;
        }
        else {
            inputs.emplace_back(arg);
        }
    }

    switch (mode) {
    case Mode::PACK:
        return pack(inputs, output, append, recursive);
    case Mode::LIST:
        if (inputs.size() != 1) {
            print("--list needs one package file\n{}", USAGE);
            return EXIT_FAILURE;
        }
        return listFile(inputs[0]);
    case Mode::BAKE_SHADOWS:
        return bakeShadows(inputs, shadow_options);
    default:
        print("{}", USAGE);
        return EXIT_FAILURE;
    }
}
