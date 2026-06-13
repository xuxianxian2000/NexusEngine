#pragma once

#include "nexus/core/types.h"

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nexus {

// ---------------------------------------------------------------------------
// StringId - interned string with fast comparison via FNV-1a hash
// ---------------------------------------------------------------------------
class StringId {
public:
    StringId() = default;

    StringId(const char* str)
        : StringId(std::string_view{str}) {}

    StringId(std::string_view str)
        : hash_(fnv1a_(str)) {
        // Intern the string in the global pool. StringIds are created from many
        // threads (job system, asset loading), so the pool must be guarded.
        std::lock_guard<std::mutex> lock(mutex_());
        pool_().emplace(hash_, std::string(str));
    }

    u64 hash() const { return hash_; }

    const char* c_str() const {
        if (hash_ == 0) return "";
        std::lock_guard<std::mutex> lock(mutex_());
        auto& pool = pool_();
        auto it = pool.find(hash_);
        if (it != pool.end()) return it->second.c_str();
        return "";
    }

    bool operator==(const StringId& other) const { return hash_ == other.hash_; }
    bool operator!=(const StringId& other) const { return hash_ != other.hash_; }
    bool operator<(const StringId& other)  const { return hash_ <  other.hash_; }

private:
    static constexpr u64 fnv1a_(std::string_view str) {
        u64 hash = 14695981039346656037ULL; // FNV offset basis
        for (char c : str) {
            hash ^= static_cast<u64>(static_cast<unsigned char>(c));
            hash *= 1099511628211ULL; // FNV prime
        }
        return hash;
    }

    static std::unordered_map<u64, std::string>& pool_() {
        static std::unordered_map<u64, std::string> instance;
        return instance;
    }

    // Guards pool_(). Pointers/references to the pooled std::strings stay valid
    // across rehashes (node-based container) and entries are never erased, so
    // c_str() may safely return into the pool after releasing the lock.
    static std::mutex& mutex_() {
        static std::mutex instance;
        return instance;
    }

    u64 hash_ = 0;
};

} // namespace nexus

// std::hash specialization
template <>
struct std::hash<nexus::StringId> {
    std::size_t operator()(const nexus::StringId& id) const noexcept {
        return static_cast<std::size_t>(id.hash());
    }
};
