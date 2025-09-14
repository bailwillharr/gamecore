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

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#if 0

static std::string_view svFromVec(const std::vector<std::uint8_t>& vec) { return std::string_view(reinterpret_cast<const char*>(vec.data()), vec.size()); }

static std::fstream openGcpak(std::filesystem::path path)
{
    auto file = std::fstream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        file = std::fstream(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    }
    return file;
}

static void writeEmptyHeader(std::ostream& file)
{
    file.seekp(0);
    GcpakHeader header{};
    header.format_identifier = GCPAK_VALID_IDENTIFIER;
    header.format_version = GCPAK_CURRENT_VERSION;
    header.num_entries = 0;
    file.write(reinterpret_cast<char*>(&header), sizeof(header));
}

// reads file header into structure, does not verify format identifier or version
static GcpakHeader readHeader(std::istream& file)
{
    file.seekg(0);
    GcpakHeader header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (file.gcount() != sizeof(header)) {
        std::cerr << "Failed to read file header!\n";
        std::abort();
    }
    return header;
}

static bool verifyHeader(const GcpakHeader& header)
{
    if (header.format_identifier != GCPAK_VALID_IDENTIFIER) {
        return false;
    }
    if (header.format_version != GCPAK_CURRENT_VERSION) {
        return false;
    }
    return true;
}

static std::vector<GcpakAssetEntry> readEntries(std::istream& file, std::uint32_t num_entries)
{

    std::vector<GcpakAssetEntry> entries(num_entries);

    for (std::uint32_t i = 0; i < num_entries; ++i) {
        const std::streamoff offset = (-1LL - static_cast<std::streamoff>(i)) * sizeof(GcpakAssetEntry);
        file.seekg(offset, std::ios::end);

        file.read(reinterpret_cast<char*>(&entries.at(i)), sizeof(GcpakAssetEntry));
        if (file.gcount() != sizeof(GcpakAssetEntry)) {
            std::cerr << "Failed to read file entry!\n";
            std::abort();
        }
    }

    return entries;
}

static std::vector<std::uint8_t> loadAsset(std::istream& file, const GcpakAssetEntry& entry)
{
    if (entry.size_uncompressed != 0) {
        std::cerr << "Decompression not supported yet!\n";
        std::abort();
    }
    std::vector<std::uint8_t> asset_data(entry.size);
    file.seekg(entry.offset);
    file.read(reinterpret_cast<char*>(asset_data.data()), entry.size);
    if (file.gcount() != entry.size) {
        std::cerr << "Failed to load asset data!\n";
        std::abort();
    }
    return asset_data;
}

// returns entries.end() if not found
static std::vector<GcpakAssetEntry>::iterator findAsset(std::vector<GcpakAssetEntry>& entries, std::uint32_t asset_crc)
{
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->crc32_id == asset_crc) return it;
    }
    return entries.end();
}

// returns stream position at end of new asset data
// overwrites entry table
// num_entries is the number of entries not including this asset.
// The asset's entry is not written, infact the entry table is completely destroyed
static std::streampos writeAssetData(std::ostream& file, const std::vector<std::uint8_t>& data, std::uint32_t num_entries)
{
    // find where the entries start (file data ends) relative to std::ios::end
    std::streamoff entries_start_offset = -static_cast<std::streamoff>(sizeof(GcpakAssetEntry) * num_entries);
    file.seekp(entries_start_offset, std::ios::end);
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    if (file.fail()) {
        std::cerr << "Failed to write to .gcpak file!\n";
        std::abort();
    }
    return file.tellp();
}

// writes the entry table starting from entry_table_offset
static void writeEntryTable(std::ostream& file, const std::vector<GcpakAssetEntry>& entries, std::streampos entry_table_offset)
{
    file.seekp(entry_table_offset);
    file.write(reinterpret_cast<const char*>(entries.data()), entries.size() * sizeof(GcpakAssetEntry));
    if (file.fail()) {
        std::cerr << "Failed to write entry table to .gcpak file!\n";
        std::abort();
    }
    const std::streampos pos = file.tellp();
    file.seekp(0, std::ios::end);
    if (pos != file.tellp()) {
        file.flush();
        std::cerr << "Didn't reach EOF!\n";
        std::abort();
    }
}

static void writeHeader(std::ostream& file, const GcpakHeader& header)
{
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&header), sizeof(GcpakHeader));
    if (file.fail()) {
        std::cerr << "Failed to write header to file!\n";
        std::abort();
    }
}

static std::atomic<bool> g_open_file_callback_finished;
static std::filesystem::path g_open_file_callback_result;
static void SDLCALL openFileCallback(void* userdata, const char* const* filelist, int filter)
{
    (void)userdata;
    (void)filter;

    if (!filelist) {
        std::cerr << "SDL_ShowOpenFileDialog() error: " << SDL_GetError() << "\n";
        std::abort();
    }

    if (!filelist[0]) {
        g_open_file_callback_result.clear();
    }
    else {
        g_open_file_callback_result = std::filesystem::path(filelist[0]);
    }

    std::cout << "TEST!\n";

    g_open_file_callback_finished.store(true);
}

static std::filesystem::path openFileDialog([[maybe_unused]] const std::vector<std::string>& extensions = {})
{
    /* linux requires an event loop to use SDL_ShowOpenFileDialog() */

    if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        std::cerr << "SDL_Init() error: " << SDL_GetError() << "\n";
        std::abort();
    }

    // open file dialog here
    std::vector<SDL_DialogFileFilter> filters{};
    for (const auto& ext : extensions) {
        SDL_DialogFileFilter filter{};
        filter.name = ext.c_str(); // extensions lifetime is longer than filter so this is OK
        filter.pattern = ext.c_str();
    } 

    g_open_file_callback_finished.store(false);

    SDL_ShowOpenFileDialog(openFileCallback, nullptr, nullptr, filters.data(), static_cast<int>(filters.size()), nullptr, false);

    while (g_open_file_callback_finished.load() == false) {
        /*
        SDL_Event e{};
        if (!SDL_WaitEvent(&e)) {
            std::cerr << "SDL_WaitEvent() error: " << SDL_GetError() << "\n";
            std::abort();
        }
        */
        SDL_PumpEvents();
    }

    SDL_QuitSubSystem(SDL_INIT_EVENTS);

    if (g_open_file_callback_result.empty()) {
        std::cerr << "No file chosen\n";
        std::abort();
    }

    return g_open_file_callback_result;
}

static std::map<std::uint32_t, std::string> parseHashFile(std::istream& file)
{
    std::map<std::uint32_t, std::string> map{};
    std::string line{};
    file.seekg(0);
    while (std::getline(file, line)) {
        std::uint32_t hash;
        std::from_chars_result res = std::from_chars(line.data(), line.data() + line.size(), hash, 16);
        if (res.ptr != line.data() + 8) {
            std::cerr << "Error parsing hash file!\n";
            return {};
        }
        std::string_view name(line.begin() + 9, line.end()); // skip over hash and space character
        map.emplace(hash, name);
    }
    return map;
}

static void saveHashFile(std::ostream& file, const std::map<std::uint32_t, std::string>& reverse_crcs)
{
    file.seekp(0, std::ios::beg);
    for (const auto& [hash, name] : reverse_crcs) {
        file << std::setfill('0') << std::setw(8) << std::hex << hash << " " << name << std::endl;
    }
}

#endif

int main(int argc, char* argv[])
{
    // edit .gcpak files

    std::map<std::uint32_t, std::string> reverse_crcs{};

    std::filesystem::path gcpak_path{};
    if (argc >= 2) {
        gcpak_path = argv[1];
    }
    else {
        gcpak_path = openFileDialog({"gcpak"});
    }

    auto file = openGcpak(gcpak_path);

    std::cout << "Opened " << gcpak_path.filename().string() << "\n";

    // check if file is new (size is zero)
    file.seekg(0, std::ios::end);
    if (file.tellg() == 0) {
        // initialise file with header
        std::cout << "Empty file. Writing new header...\n";
        writeEmptyHeader(file);
    }

    std::filesystem::path hash_path = gcpak_path;
    hash_path.replace_extension("txt");

    // attempt to load hash file
    {
        std::ifstream hash_file(hash_path);
        if (hash_file) {
            reverse_crcs = parseHashFile(hash_file);
            std::cout << "Opened hash file " << hash_path.filename().string() << "\n";
        }
        else {
            std::cout << "Will create new hash file on exit.\n";
        }
    }

    // prompt loop
    for (bool quit = false; !quit;) {

        std::cout << "\n";

        // read header data
        GcpakHeader header = readHeader(file);
        if (!verifyHeader(header)) {
            std::cerr << "Invalid file header!\n";
            std::abort();
        }

        std::cout << std::format("Current Gcpak file: {}\n", gcpak_path.filename().string());
        std::cout << std::format("Number of entries: {}\n\n", header.num_entries);

        auto entries = readEntries(file, header.num_entries);

        for (const auto& entry : entries) {
            if (reverse_crcs.contains(entry.crc32_id)) {
                std::cout << "name: " << reverse_crcs[entry.crc32_id] << "\n";
            }
            std::cout << std::format("hash: {:#08x}\noffset: {:#016x}\nsize_uncompressed: {} bytes\nsize: {} bytes\n", entry.crc32_id, entry.offset,
                                     entry.size_uncompressed, entry.size);
            std::cout << "Asset string (first 16 bytes): ";
            auto asset_data = loadAsset(file, entry);
            if (asset_data.size() > 16) {
                asset_data.resize(16);
            }
            std::cout << svFromVec(asset_data) << "\n\n";
        }

        std::cout << "Options: (A)dd asset, (Q)uit: ";
        std::string prompt_buf;
        while (prompt_buf.empty()) {
            std::getline(std::cin, prompt_buf);
        }

        switch (std::toupper(prompt_buf[0])) {
            case 'A': {
                // first ask for file path
                std::filesystem::path asset_path = openFileDialog();

                // attempt to open file path
                std::ifstream asset_file(asset_path, std::ios::binary);
                if (!asset_file.is_open()) {
                    std::cerr << "Failed to open file!\n";
                    break;
                }

                // then ask for asset name
                std::cout << "Enter asset ID: ";
                std::string asset_name;
                std::cin >> asset_name;
                asset_name += "\ntest";
                if (std::string::size_type idx = asset_name.find('\n'); idx != std::string::npos) {
                    // remove newline
                    asset_name.resize(idx);
                }

                // convert to crc32
                const std::uint32_t asset_crc = assetIDRuntime(asset_name);

                if (!reverse_crcs.contains(asset_crc)) {
                    reverse_crcs.emplace(asset_crc, asset_name);
                }

                // see if asset already exists
                if (findAsset(entries, asset_crc) != entries.end()) {
                    std::cerr << "Asset ID already in use!\n";
                    break;
                }

                // ask for compression
                /*
                std::cout << "Use compression? (y/n): ";
                std::string compression_response;
                std::cin >> compression_response;
                bool use_compression = false;
                if (std::toupper(compression_response[0]) == 'Y') {
                    use_compression = true;
                }
                */

                // read file into buffer
                asset_file.seekg(0, std::ios::end);
                std::vector<std::uint8_t> asset_data(asset_file.tellg());
                asset_file.seekg(0);
                asset_file.read(reinterpret_cast<char*>(asset_data.data()), asset_data.size());
                if (asset_file.gcount() != static_cast<std::streamsize>(asset_data.size())) {
                    std::cerr << "Failed to read file!\n";
                    std::abort();
                }

                // append data to gcpak file
                const std::size_t new_entry_table_offset = writeAssetData(file, asset_data, header.num_entries);

                // create new entry
                GcpakAssetEntry asset_entry{};
                asset_entry.offset = new_entry_table_offset - asset_data.size();
                asset_entry.crc32_id = asset_crc;
                asset_entry.asset_type = GcpakAssetType::RAW;
                asset_entry.size_uncompressed = 0;
                asset_entry.size = static_cast<std::uint32_t>(asset_data.size());
                entries.push_back(asset_entry);

                // re-write entry table
                writeEntryTable(file, entries, new_entry_table_offset);

                // update header
                header.num_entries = static_cast<uint32_t>(entries.size());
                writeHeader(file, header);

            } break;
            case 'Q':
                quit = true;
                break;
        }
        std::cin.clear();
    }

    // save reverse crc table
    {
        std::ofstream hash_file(hash_path, std::ios::out | std::ios::trunc);
        if (hash_file) {
            saveHashFile(hash_file, reverse_crcs);
            std::cout << "Saved hash file " << hash_path.filename().string() << "\n";
        }
        else {
            std::cerr << "Failed to open " << hash_path.string() << " for writing!\n";
        }
    }
    return 0;
}
