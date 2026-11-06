// Android adapter for the unmodified, pinned innoextract sources.
#include "loader/offsets.hpp"
#include "setup/info.hpp"
#include "setup/data.hpp"
#include "setup/file.hpp"
#include "cli/gog.hpp"
#include "cli/goggalaxy.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

int inno_main(int, char **);

static std::string app_path(std::string name)
{
    std::replace(name.begin(), name.end(), '\\', '/');
    if (!name.compare(0, 6, "{app}/")) name.erase(0, 6);
    else if (!name.empty() && name.front() == '{') return {};
    if (name.empty()) return {};
    if (name.front() == '/' || name.find("../") != std::string::npos ||
        name.find("/..") != std::string::npos || name.find(':') != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw std::runtime_error("Invalid installer path");
    return name;
}

static bool needed(const std::string &name)
{
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    size_t slash = lower.find('/');
    if (slash != std::string::npos) {
        const std::string directory = lower.substr(0, slash);
        return directory == "aiv" || directory == "binks" || directory == "fx" || directory == "gfx" ||
               directory == "gfx8" || directory == "gm" || directory == "maps" || directory == "mapsextreme";
    }
    if (lower == "stronghold.exe" || lower == "stronghold crusader.exe" || lower == "stronghold_crusader_extreme.exe" ||
        lower == "mss32.dll" || lower == "binkw32.dll") return true;
    const size_t dot = lower.rfind('.');
    const std::string extension = dot == std::string::npos ? "" : lower.substr(dot);
    return extension == ".tex" || extension == ".ani" || extension == ".cur" || extension == ".bmp" ||
           extension == ".csv" || extension == ".mlb";
}

static int inspect(const char *path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Installer is unreadable");
    loader::offsets offsets; offsets.load(input); input.seekg(offsets.header_offset);
    setup::info info;
    info.load(input, setup::info::Files | setup::info::DataEntries | setup::info::RegistryEntries | setup::info::Languages | setup::info::NoUnknownVersion);
    gog::parse_galaxy_files(info, false);
    const std::string id = gog::get_game_id(info);
    if (id != "1207658712" && id != "1207658713") throw std::runtime_error("This is not a supported Stronghold GOG installer");
    if (info.files.size() > 100000) throw std::runtime_error("Installer contains too many files");
    uint64_t bytes = 0; size_t files = 0, compressed = 0;
    std::set<stream::chunk> chunks;
    std::set<stream::chunk> compressed_chunks;
    std::set<std::string> roots;
    for (const auto &file : info.files) {
        const std::string name = app_path(file.destination);
        if (name.empty() || !needed(name)) continue;
        if (file.location >= info.data_entries.size()) throw std::runtime_error("External game file in installer");
        const auto &data = info.data_entries[file.location];
        if (data.chunk.encryption != stream::Plaintext || data.chunk.compression == stream::UnknownCompression)
            throw std::runtime_error("Unsupported installer compression or encryption");
        bytes += data.uncompressed_size; files++; chunks.insert(data.chunk);
        if (data.chunk.compression != stream::Stored || data.file.filter == stream::ZlibFilter) compressed_chunks.insert(data.chunk);
        for (const auto location : file.additional_locations) {
            if (location >= info.data_entries.size()) throw std::runtime_error("Invalid GOG Galaxy file part");
            const auto &part = info.data_entries[location];
            if (part.chunk.encryption != stream::Plaintext || part.chunk.compression == stream::UnknownCompression)
                throw std::runtime_error("Unsupported GOG Galaxy file part");
            bytes += part.uncompressed_size; chunks.insert(part.chunk);
            if (part.chunk.compression != stream::Stored || part.file.filter == stream::ZlibFilter) compressed_chunks.insert(part.chunk);
        }
        if (bytes > uint64_t(8) * 1024 * 1024 * 1024) throw std::runtime_error("Installer exceeds the 8 GiB game data limit");
        if (name.find('/') == std::string::npos) roots.insert(name);
    }
    compressed = compressed_chunks.size();
    if (!files) {
        std::string sample;
        for (const auto &file : info.files) if (!file.destination.empty()) { sample = file.destination; break; }
        throw std::runtime_error("Game files not found: " + sample);
    }
    std::cout << "GOG1\t" << id << '\t' << files << '\t' << bytes << '\t' << chunks.size() << '\t' << compressed
              << '\t' << (offsets.data_offset == 0) << '\n';
    for (const auto &name : roots) std::cout << "ROOT\t" << name << '\n';
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3 || std::string(argv[1]) != "--port-info") return inno_main(argc, argv);
    try { return inspect(argv[2]); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 2; }
}
