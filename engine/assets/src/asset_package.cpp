#include "nexus/assets/asset_package.h"
#include "nexus/core/log.h"
#include <algorithm>
#include <filesystem>
#include <cstring>

namespace nexus::assets {

// ── CRC32 lookup table ──────────────────────────────────────────────────────

static u32 make_crc32_entry(u32 i) {
    u32 crc = i;
    for (u32 j = 0; j < 8; j++) {
        crc = (crc >> 1) ^ (0xEDB88320 & (~(crc & 1) + 1));
    }
    return crc;
}

static const auto& crc32_table() {
    static u32 table[256] = {};
    static bool init = false;
    if (!init) {
        for (u32 i = 0; i < 256; i++) {
            table[i] = make_crc32_entry(i);
        }
        init = true;
    }
    return table;
}

u32 AssetPackage::crc32(const u8* data, u64 size) {
    const auto& table = crc32_table();
    u32 crc = 0xFFFFFFFF;
    for (u64 i = 0; i < size; i++) {
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}

// ── AssetPackage ────────────────────────────────────────────────────────────

bool AssetPackage::create(const std::string& output_path) {
    stream_.open(output_path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!stream_.is_open()) {
        NX_ERROR("AssetPackage: failed to create {}", output_path);
        return false;
    }

    file_path_ = output_path;
    is_open_ = true;
    is_writing_ = true;
    entries_.clear();
    id_lookup_.clear();
    path_lookup_.clear();
    data_offset_ = sizeof(PackageHeader);

    // Write placeholder header (will be overwritten in finalize)
    PackageHeader header;
    stream_.write(reinterpret_cast<const char*>(&header), sizeof(header));

    return true;
}

bool AssetPackage::open(const std::string& path) {
    stream_.open(path, std::ios::in | std::ios::binary);
    if (!stream_.is_open()) {
        NX_ERROR("AssetPackage: failed to open {}", path);
        return false;
    }

    file_path_ = path;
    is_open_ = true;
    is_writing_ = false;

    return read_toc();
}

void AssetPackage::close() {
    if (stream_.is_open()) {
        stream_.close();
    }
    is_open_ = false;
    is_writing_ = false;
}

bool AssetPackage::add_file(const std::string& virtual_path,
                             const std::string& source_path,
                             AssetType type) {
    if (!is_writing_) return false;

    std::ifstream file(source_path, std::ios::binary);
    if (!file.is_open()) {
        NX_ERROR("AssetPackage: failed to read source file {}", source_path);
        return false;
    }

    std::vector<u8> data{std::istreambuf_iterator<char>(file),
                         std::istreambuf_iterator<char>{}};

    if (type == AssetType::Unknown) {
        std::filesystem::path p(source_path);
        if (p.has_extension()) {
            type = asset_type_from_extension(p.extension().string());
        }
    }

    return add_data(virtual_path, data, type);
}

bool AssetPackage::add_data(const std::string& virtual_path,
                             const std::vector<u8>& data,
                             AssetType type) {
    if (!is_writing_) return false;

    PackageEntry entry;
    entry.id = AssetId(virtual_path);
    entry.type = type;
    entry.path = virtual_path;
    entry.offset = data_offset_;
    entry.size = data.size();
    entry.compressed_size = 0; // No compression in this version
    entry.checksum = crc32(data.data(), data.size());

    // Write data at current position
    stream_.seekp(static_cast<std::streamoff>(data_offset_));
    if (!data.empty()) {
        stream_.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
    }
    data_offset_ += data.size();

    u32 index = static_cast<u32>(entries_.size());
    entries_.push_back(entry);
    id_lookup_[entry.id] = index;
    path_lookup_[virtual_path] = index;

    return true;
}

bool AssetPackage::finalize() {
    if (!is_writing_) return false;

    // Write TOC
    u64 toc_offset = data_offset_;
    stream_.seekp(static_cast<std::streamoff>(toc_offset));

    for (auto& entry : entries_) {
        // Write path length + path
        u32 path_len = static_cast<u32>(entry.path.size());
        stream_.write(reinterpret_cast<const char*>(&path_len), sizeof(path_len));
        stream_.write(entry.path.data(), path_len);

        // Write entry fields
        stream_.write(reinterpret_cast<const char*>(&entry.id.value), sizeof(entry.id.value));
        stream_.write(reinterpret_cast<const char*>(&entry.type), sizeof(entry.type));
        stream_.write(reinterpret_cast<const char*>(&entry.offset), sizeof(entry.offset));
        stream_.write(reinterpret_cast<const char*>(&entry.size), sizeof(entry.size));
        stream_.write(reinterpret_cast<const char*>(&entry.compressed_size), sizeof(entry.compressed_size));
        stream_.write(reinterpret_cast<const char*>(&entry.checksum), sizeof(entry.checksum));
    }

    // Write header
    PackageHeader header;
    header.entry_count = static_cast<u32>(entries_.size());
    header.toc_offset = toc_offset;

    stream_.seekp(0);
    stream_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    stream_.flush();

    is_writing_ = false;
    return true;
}

std::vector<u8> AssetPackage::read_entry(const std::string& virtual_path) const {
    auto it = path_lookup_.find(virtual_path);
    if (it == path_lookup_.end()) return {};
    return read_entry_impl(entries_[it->second]);
}

std::vector<u8> AssetPackage::read_entry(AssetId id) const {
    auto it = id_lookup_.find(id);
    if (it == id_lookup_.end()) return {};
    return read_entry_impl(entries_[it->second]);
}

bool AssetPackage::has_entry(const std::string& virtual_path) const {
    return path_lookup_.count(virtual_path) > 0;
}

bool AssetPackage::has_entry(AssetId id) const {
    return id_lookup_.count(id) > 0;
}

const PackageEntry* AssetPackage::find_entry(const std::string& virtual_path) const {
    auto it = path_lookup_.find(virtual_path);
    if (it == path_lookup_.end()) return nullptr;
    return &entries_[it->second];
}

const PackageEntry* AssetPackage::find_entry(AssetId id) const {
    auto it = id_lookup_.find(id);
    if (it == id_lookup_.end()) return nullptr;
    return &entries_[it->second];
}

u64 AssetPackage::total_data_size() const {
    u64 total = 0;
    for (auto& entry : entries_) {
        total += entry.size;
    }
    return total;
}

std::vector<u8> AssetPackage::read_entry_impl(const PackageEntry& entry) const {
    if (!is_open_ || entry.size == 0) return {};

    std::vector<u8> data(entry.size);
    stream_.seekg(static_cast<std::streamoff>(entry.offset));
    stream_.read(reinterpret_cast<char*>(data.data()),
                 static_cast<std::streamsize>(entry.size));

    if (!stream_.good()) {
        NX_ERROR("AssetPackage: failed to read entry '{}'", entry.path);
        return {};
    }

    return data;
}

bool AssetPackage::validate() const {
    if (!is_open_) return false;

    // Check header
    stream_.seekg(0);
    PackageHeader header;
    stream_.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (header.magic != 0x4E585041) return false;
    if (header.version != 1) return false;
    if (header.entry_count != entries_.size()) return false;

    return true;
}

bool AssetPackage::write_header() {
    PackageHeader header;
    header.entry_count = static_cast<u32>(entries_.size());
    header.toc_offset = data_offset_;

    stream_.seekp(0);
    stream_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    return stream_.good();
}

bool AssetPackage::write_toc() {
    return true; // Done in finalize
}

bool AssetPackage::read_toc() {
    // Determine the file size up front so untrusted TOC fields can be validated
    // against it before they drive any allocation.
    stream_.seekg(0, std::ios::end);
    const auto file_size = static_cast<u64>(stream_.tellg());

    // Read header
    stream_.seekg(0);
    PackageHeader header;
    stream_.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (header.magic != 0x4E585041) {
        NX_ERROR("AssetPackage: invalid magic number");
        return false;
    }
    if (header.version != 1) {
        NX_ERROR("AssetPackage: unsupported version {}", header.version);
        return false;
    }

    // Read TOC
    if (header.toc_offset > file_size) {
        NX_ERROR("AssetPackage: TOC offset {} beyond file size {}", header.toc_offset, file_size);
        return false;
    }
    // Each entry needs at least one length field plus the fixed-size fields, so
    // entry_count can never exceed the bytes available for the TOC.
    const u64 toc_bytes = file_size - header.toc_offset;
    if (header.entry_count > toc_bytes / sizeof(u32)) {
        NX_ERROR("AssetPackage: entry_count {} implausible for {} TOC bytes",
                 header.entry_count, toc_bytes);
        return false;
    }
    stream_.seekg(static_cast<std::streamoff>(header.toc_offset));

    entries_.clear();
    id_lookup_.clear();
    path_lookup_.clear();

    for (u32 i = 0; i < header.entry_count; i++) {
        PackageEntry entry;

        u32 path_len = 0;
        stream_.read(reinterpret_cast<char*>(&path_len), sizeof(path_len));
        if (!stream_.good() || path_len > file_size - static_cast<u64>(stream_.tellg())) {
            NX_ERROR("AssetPackage: TOC entry path length {} exceeds remaining file", path_len);
            return false;
        }
        entry.path.resize(path_len);
        stream_.read(entry.path.data(), path_len);

        stream_.read(reinterpret_cast<char*>(&entry.id.value), sizeof(entry.id.value));
        stream_.read(reinterpret_cast<char*>(&entry.type), sizeof(entry.type));
        stream_.read(reinterpret_cast<char*>(&entry.offset), sizeof(entry.offset));
        stream_.read(reinterpret_cast<char*>(&entry.size), sizeof(entry.size));
        stream_.read(reinterpret_cast<char*>(&entry.compressed_size), sizeof(entry.compressed_size));
        stream_.read(reinterpret_cast<char*>(&entry.checksum), sizeof(entry.checksum));

        u32 index = static_cast<u32>(entries_.size());
        entries_.push_back(entry);
        id_lookup_[entry.id] = index;
        path_lookup_[entry.path] = index;
    }

    return stream_.good();
}

// ── AssetHotReload ──────────────────────────────────────────────────────────

void AssetHotReload::watch(const std::string& directory) {
    // Check not already watching
    for (auto& w : watches_) {
        if (w.directory == directory) return;
    }

    WatchEntry entry;
    entry.directory = directory;

    // Scan initial state
    std::error_code ec;
    if (std::filesystem::exists(directory, ec)) {
        for (auto& p : std::filesystem::recursive_directory_iterator(directory, ec)) {
            if (p.is_regular_file()) {
                auto ftime = std::filesystem::last_write_time(p, ec);
                if (!ec) {
                    entry.file_times[p.path().string()] =
                        static_cast<u64>(ftime.time_since_epoch().count());
                }
            }
        }
    }

    watches_.push_back(std::move(entry));
}

void AssetHotReload::unwatch(const std::string& directory) {
    watches_.erase(
        std::remove_if(watches_.begin(), watches_.end(),
            [&](const WatchEntry& w) { return w.directory == directory; }),
        watches_.end());
}

std::vector<std::string> AssetHotReload::poll_changes() {
    std::vector<std::string> changed;

    for (auto& watch : watches_) {
        std::error_code ec;
        if (!std::filesystem::exists(watch.directory, ec)) continue;

        for (auto& p : std::filesystem::recursive_directory_iterator(watch.directory, ec)) {
            if (!p.is_regular_file()) continue;

            auto path = p.path().string();
            auto ftime = std::filesystem::last_write_time(p, ec);
            if (ec) continue;

            u64 current = static_cast<u64>(ftime.time_since_epoch().count());
            auto it = watch.file_times.find(path);

            if (it == watch.file_times.end()) {
                // New file
                watch.file_times[path] = current;
                changed.push_back(path);
            } else if (it->second != current) {
                // Modified file
                it->second = current;
                changed.push_back(path);
            }
        }
    }

    return changed;
}

u32 AssetHotReload::tracked_file_count() const {
    u32 count = 0;
    for (auto& w : watches_) {
        count += static_cast<u32>(w.file_times.size());
    }
    return count;
}

} // namespace nexus::assets
