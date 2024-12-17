#include <cstdint>

#include <array>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <format>

#ifdef _WIN32
#include <windows.h>
#define WIN_MAX_PATH 260
#endif

static constexpr std::array<uint32_t, 256> crc_table = {
    0x00000000L, 0x77073096L, 0xee0e612cL, 0x990951baL, 0x076dc419L, 0x706af48fL, 0xe963a535L, 0x9e6495a3L, 0x0edb8832L, 0x79dcb8a4L, 0xe0d5e91eL, 0x97d2d988L,
    0x09b64c2bL, 0x7eb17cbdL, 0xe7b82d07L, 0x90bf1d91L, 0x1db71064L, 0x6ab020f2L, 0xf3b97148L, 0x84be41deL, 0x1adad47dL, 0x6ddde4ebL, 0xf4d4b551L, 0x83d385c7L,
    0x136c9856L, 0x646ba8c0L, 0xfd62f97aL, 0x8a65c9ecL, 0x14015c4fL, 0x63066cd9L, 0xfa0f3d63L, 0x8d080df5L, 0x3b6e20c8L, 0x4c69105eL, 0xd56041e4L, 0xa2677172L,
    0x3c03e4d1L, 0x4b04d447L, 0xd20d85fdL, 0xa50ab56bL, 0x35b5a8faL, 0x42b2986cL, 0xdbbbc9d6L, 0xacbcf940L, 0x32d86ce3L, 0x45df5c75L, 0xdcd60dcfL, 0xabd13d59L,
    0x26d930acL, 0x51de003aL, 0xc8d75180L, 0xbfd06116L, 0x21b4f4b5L, 0x56b3c423L, 0xcfba9599L, 0xb8bda50fL, 0x2802b89eL, 0x5f058808L, 0xc60cd9b2L, 0xb10be924L,
    0x2f6f7c87L, 0x58684c11L, 0xc1611dabL, 0xb6662d3dL, 0x76dc4190L, 0x01db7106L, 0x98d220bcL, 0xefd5102aL, 0x71b18589L, 0x06b6b51fL, 0x9fbfe4a5L, 0xe8b8d433L,
    0x7807c9a2L, 0x0f00f934L, 0x9609a88eL, 0xe10e9818L, 0x7f6a0dbbL, 0x086d3d2dL, 0x91646c97L, 0xe6635c01L, 0x6b6b51f4L, 0x1c6c6162L, 0x856530d8L, 0xf262004eL,
    0x6c0695edL, 0x1b01a57bL, 0x8208f4c1L, 0xf50fc457L, 0x65b0d9c6L, 0x12b7e950L, 0x8bbeb8eaL, 0xfcb9887cL, 0x62dd1ddfL, 0x15da2d49L, 0x8cd37cf3L, 0xfbd44c65L,
    0x4db26158L, 0x3ab551ceL, 0xa3bc0074L, 0xd4bb30e2L, 0x4adfa541L, 0x3dd895d7L, 0xa4d1c46dL, 0xd3d6f4fbL, 0x4369e96aL, 0x346ed9fcL, 0xad678846L, 0xda60b8d0L,
    0x44042d73L, 0x33031de5L, 0xaa0a4c5fL, 0xdd0d7cc9L, 0x5005713cL, 0x270241aaL, 0xbe0b1010L, 0xc90c2086L, 0x5768b525L, 0x206f85b3L, 0xb966d409L, 0xce61e49fL,
    0x5edef90eL, 0x29d9c998L, 0xb0d09822L, 0xc7d7a8b4L, 0x59b33d17L, 0x2eb40d81L, 0xb7bd5c3bL, 0xc0ba6cadL, 0xedb88320L, 0x9abfb3b6L, 0x03b6e20cL, 0x74b1d29aL,
    0xead54739L, 0x9dd277afL, 0x04db2615L, 0x73dc1683L, 0xe3630b12L, 0x94643b84L, 0x0d6d6a3eL, 0x7a6a5aa8L, 0xe40ecf0bL, 0x9309ff9dL, 0x0a00ae27L, 0x7d079eb1L,
    0xf00f9344L, 0x8708a3d2L, 0x1e01f268L, 0x6906c2feL, 0xf762575dL, 0x806567cbL, 0x196c3671L, 0x6e6b06e7L, 0xfed41b76L, 0x89d32be0L, 0x10da7a5aL, 0x67dd4accL,
    0xf9b9df6fL, 0x8ebeeff9L, 0x17b7be43L, 0x60b08ed5L, 0xd6d6a3e8L, 0xa1d1937eL, 0x38d8c2c4L, 0x4fdff252L, 0xd1bb67f1L, 0xa6bc5767L, 0x3fb506ddL, 0x48b2364bL,
    0xd80d2bdaL, 0xaf0a1b4cL, 0x36034af6L, 0x41047a60L, 0xdf60efc3L, 0xa867df55L, 0x316e8eefL, 0x4669be79L, 0xcb61b38cL, 0xbc66831aL, 0x256fd2a0L, 0x5268e236L,
    0xcc0c7795L, 0xbb0b4703L, 0x220216b9L, 0x5505262fL, 0xc5ba3bbeL, 0xb2bd0b28L, 0x2bb45a92L, 0x5cb36a04L, 0xc2d7ffa7L, 0xb5d0cf31L, 0x2cd99e8bL, 0x5bdeae1dL,
    0x9b64c2b0L, 0xec63f226L, 0x756aa39cL, 0x026d930aL, 0x9c0906a9L, 0xeb0e363fL, 0x72076785L, 0x05005713L, 0x95bf4a82L, 0xe2b87a14L, 0x7bb12baeL, 0x0cb61b38L,
    0x92d28e9bL, 0xe5d5be0dL, 0x7cdcefb7L, 0x0bdbdf21L, 0x86d3d2d4L, 0xf1d4e242L, 0x68ddb3f8L, 0x1fda836eL, 0x81be16cdL, 0xf6b9265bL, 0x6fb077e1L, 0x18b74777L,
    0x88085ae6L, 0xff0f6a70L, 0x66063bcaL, 0x11010b5cL, 0x8f659effL, 0xf862ae69L, 0x616bffd3L, 0x166ccf45L, 0xa00ae278L, 0xd70dd2eeL, 0x4e048354L, 0x3903b3c2L,
    0xa7672661L, 0xd06016f7L, 0x4969474dL, 0x3e6e77dbL, 0xaed16a4aL, 0xd9d65adcL, 0x40df0b66L, 0x37d83bf0L, 0xa9bcae53L, 0xdebb9ec5L, 0x47b2cf7fL, 0x30b5ffe9L,
    0xbdbdf21cL, 0xcabac28aL, 0x53b39330L, 0x24b4a3a6L, 0xbad03605L, 0xcdd70693L, 0x54de5729L, 0x23d967bfL, 0xb3667a2eL, 0xc4614ab8L, 0x5d681b02L, 0x2a6f2b94L,
    0xb40bbe37L, 0xc30c8ea1L, 0x5a05df1bL, 0x2d02ef8dL};

/* uses crc_table (gc_crc_table.h) to generate a unique 32-bit hash */
static constexpr std::uint32_t crc32(std::string_view id)
{
    uint32_t crc = 0xffffffffu;
    for (char c : id) crc = (crc >> 8) ^ crc_table[(crc ^ c) & 0xff];
    return crc ^ 0xffffffff;
}

/* Get the compile-time hash of the Asset ID. */
/* Use AssetIDRuntime() for runtime equivalent. */
static consteval std::uint32_t assetID(std::string_view id) { return crc32(id); }

/* get the runtime hash of the Asset ID */
static std::uint32_t assetIDRuntime(std::string_view id) { return crc32(id); }

/* see gamecore/include/gamecore/gc_gcpak.h */

struct GcpakHeader {
    std::array<std::uint8_t, 6> format_identifier; // null-terminated "GCPAK"
    std::uint16_t format_version;                  // currently 1
    std::uint32_t num_entries;
};

struct GcpakAssetEntry {
    std::size_t offset; // absolute positition of start of asset data in the file
    std::uint32_t crc32_id;
    std::uint32_t reserved;          // leave as zero for now
    std::uint32_t size_uncompressed; // set to zero for no compression
    std::uint32_t size;              // size of data in file (compressed size if compression enabled)
};

constexpr std::array<std::uint8_t, 6> GCPAK_VALID_IDENTIFIER = {'G', 'C', 'P', 'A', 'K', '\0'};
constexpr std::uint16_t GCPAK_CURRENT_VERSION = 1;

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
    std::streamoff entries_start_offset = -(sizeof(GcpakAssetEntry) * num_entries);
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
        std::cerr << "Failed to write header to file!";
        std::abort();
    }
}

static std::filesystem::path openFileDialog(const std::vector<std::string>& extensions = {})
{
#ifdef _WIN32

    // build the filter string
    std::string filter{};
    if (extensions.empty()) {
        filter = "All Files\0*.*\0\0";
    }
    else {
        std::string wildcards{};
        for (const std::string& ext : extensions) {
            wildcards += "*." + ext + ";";
        }
        wildcards.pop_back(); // remove the last semicolon
        filter.push_back('(');
        filter.append(wildcards);
        filter.push_back(')');
        filter.push_back('\0');
        filter.append(wildcards);
        filter.push_back('\0');
        filter.append("All Files");
        filter.push_back('\0');
        filter.append("*.*");
        filter.push_back('\0');
        filter.push_back('\0');
    }

    OPENFILENAMEA ofn{};             // common dialog box structure
    CHAR szFile[WIN_MAX_PATH] = {0}; // if using TCHAR macros, use TCHAR array

    // Initialize OPENFILENAME
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.lpstrFile[0] = '\0';
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = filter.c_str();
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    // Display the Open dialog box
    if (GetOpenFileNameA(&ofn) == TRUE) {
        return std::filesystem::path(std::string(ofn.lpstrFile));
    }
    else {
        return std::filesystem::path{}; // User cancelled the dialog
    }
#else
    // only Windows dialogs supported at the moment
    std::cerr << "Open file dialog not supported on this platform";
    std::cout << "Enter file path: ";
    std::string file_path_str;
    std::getline(std::cin, file_path_str);

    return std::filesystem::path(file_path_str);
#endif
}

static std::unordered_map<std::uint32_t, std::string> parseHashFile(std::istream& file)
{
    std::unordered_map<std::uint32_t, std::string> map{};
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

static void saveHashFile(std::ostream& file, const std::unordered_map<std::uint32_t, std::string>& reverse_crcs)
{
    file.seekp(0, std::ios::beg);
    for (const auto& crc : reverse_crcs) {
        file << std::setfill('0') << std::setw(8) << std::hex << crc.first << " " << crc.second << std::endl;
    }
}

int main()
{

    std::unordered_map<std::uint32_t, std::string> reverse_crcs{};

    // edit .gcpak files
    const std::filesystem::path gcpak_path = openFileDialog({"gcpak"});
    auto file = openGcpak(gcpak_path);

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
            std::cout << "Loaded hash file!\n";
        }
        else {
            std::cout << "Will create new hash file on exit.\n";
        }
    }

    // prompt loop
    bool quit = false;
    while (!quit) {

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
                std::cout << "Use compression? (y/n): ";
                std::string compression_response;
                std::cin >> compression_response;
                bool use_compression = false;
                if (std::toupper(compression_response[0]) == 'Y') {
                    use_compression = true;
                }

                // read file into buffer
                asset_file.seekg(0, std::ios::end);
                std::vector<std::uint8_t> asset_data(asset_file.tellg());
                asset_file.seekg(0);
                asset_file.read(reinterpret_cast<char*>(asset_data.data()), asset_data.size());
                if (asset_file.gcount() != asset_data.size()) {
                    std::cerr << "Failed to read file!\n";
                    std::abort();
                }

                // append data to gcpak file
                const std::size_t new_entry_table_offset = writeAssetData(file, asset_data, header.num_entries);

                // create new entry
                GcpakAssetEntry asset_entry{};
                asset_entry.offset = new_entry_table_offset - asset_data.size();
                asset_entry.crc32_id = asset_crc;
                asset_entry.reserved = 0;
                asset_entry.size_uncompressed = 0;
                asset_entry.size = asset_data.size();
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
        }
        else {
            std::cerr << "Failed to open " << hash_path.string() << " for writing!\n";
        }
    }
    return 0;
}
