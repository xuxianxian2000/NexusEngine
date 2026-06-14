#include "nexus/core/types.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <string>
#include <optional>
#include <mutex>

namespace nexus {

class VirtualFileSystem {
public:
    static VirtualFileSystem& instance() {
        static VirtualFileSystem vfs;
        return vfs;
    }

    void mount(const std::string& virtual_path, const std::string& physical_path) {
        std::lock_guard lock(mutex_);
        mount_points_[virtual_path] = physical_path;
    }

    void unmount(const std::string& virtual_path) {
        std::lock_guard lock(mutex_);
        mount_points_.erase(virtual_path);
    }

    std::optional<std::string> resolve(const std::string& virtual_path) const {
        std::lock_guard lock(mutex_);

        // Collect every mount whose virtual path is a prefix of the request at a
        // path-separator boundary (so "/assets" does not match "/assetsX/..."),
        // then try them longest-first so the most specific mount wins
        // deterministically regardless of hash-map iteration order.
        std::vector<const std::pair<const std::string, std::string>*> candidates;
        for (const auto& entry : mount_points_) {
            if (prefix_matches(virtual_path, entry.first)) {
                candidates.push_back(&entry);
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto* a, const auto* b) {
                      return a->first.size() > b->first.size();
                  });

        for (const auto* entry : candidates) {
            auto relative = lstrip_separators(virtual_path.substr(entry->first.size()));
            auto resolved = std::filesystem::path(entry->second) / relative;
            if (std::filesystem::exists(resolved)) {
                return resolved.string();
            }
        }
        return std::nullopt;
    }

    std::optional<std::vector<u8>> read_binary(const std::string& virtual_path) const {
        auto resolved = resolve(virtual_path);
        if (!resolved) return std::nullopt;

        std::ifstream file(*resolved, std::ios::binary | std::ios::ate);
        if (!file) return std::nullopt;

        auto size = file.tellg();
        file.seekg(0);
        std::vector<u8> data(static_cast<size_t>(size));
        file.read(reinterpret_cast<char*>(data.data()), size);
        return data;
    }

    std::optional<std::string> read_text(const std::string& virtual_path) const {
        auto resolved = resolve(virtual_path);
        if (!resolved) return std::nullopt;

        std::ifstream file(*resolved);
        if (!file) return std::nullopt;

        return std::string{std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>()};
    }

private:
    VirtualFileSystem() = default;

    // True if `vpath` is a prefix of `path` ending on a separator boundary.
    static bool prefix_matches(const std::string& path, const std::string& vpath) {
        if (!path.starts_with(vpath)) return false;
        if (path.size() == vpath.size()) return true;              // exact mount
        if (!vpath.empty() && vpath.back() == '/') return true;    // vpath = ".../"
        return path[vpath.size()] == '/';                          // next char is a sep
    }

    static std::string lstrip_separators(std::string s) {
        std::size_t i = 0;
        while (i < s.size() && s[i] == '/') ++i;
        return s.substr(i);
    }

    std::unordered_map<std::string, std::string> mount_points_;
    mutable std::mutex mutex_;
};

} // namespace nexus
