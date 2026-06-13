#pragma once

#include "nexus/core/types.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

namespace nexus {

// ---------------------------------------------------------------------------
// SlotKey - generational index for SlotMap
// ---------------------------------------------------------------------------
struct SlotKey {
    u32 index      = 0;
    u32 generation = 0;

    bool operator==(const SlotKey& other) const {
        return index == other.index && generation == other.generation;
    }
    bool operator!=(const SlotKey& other) const { return !(*this == other); }
};

// ---------------------------------------------------------------------------
// SlotMap<T> - generational index container for stable handles
// ---------------------------------------------------------------------------
template <typename T>
class SlotMap {
public:
    SlotMap() = default;

    SlotKey insert(T value) {
        u32 idx;
        if (!free_list_.empty()) {
            idx = free_list_.back();
            free_list_.pop_back();
            values_[idx] = std::move(value);
            alive_[idx] = true;
        } else {
            idx = static_cast<u32>(values_.size());
            values_.push_back(std::move(value));
            generations_.push_back(0);
            alive_.push_back(true);
        }
        ++size_;
        return SlotKey{idx, generations_[idx]};
    }

    bool remove(SlotKey key) {
        if (!valid(key)) return false;
        alive_[key.index] = false;
        ++generations_[key.index];
        free_list_.push_back(key.index);
        --size_;
        return true;
    }

    T* get(SlotKey key) {
        if (!valid(key)) return nullptr;
        return &values_[key.index];
    }

    const T* get(SlotKey key) const {
        if (!valid(key)) return nullptr;
        return &values_[key.index];
    }

    bool valid(SlotKey key) const {
        return key.index < generations_.size()
            && generations_[key.index] == key.generation
            && alive_[key.index];
    }

    std::size_t size() const { return size_; }

    void clear() {
        values_.clear();
        generations_.clear();
        alive_.clear();
        free_list_.clear();
        size_ = 0;
    }

    // -- Iterator over valid elements ---------------------------------------
    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = T;
        using difference_type   = std::ptrdiff_t;
        using pointer           = T*;
        using reference         = T&;

        iterator(SlotMap* map, u32 idx) : map_(map), idx_(idx) { skip_dead(); }

        reference operator*() const { return map_->values_[idx_]; }
        pointer   operator->() const { return &map_->values_[idx_]; }

        iterator& operator++() { ++idx_; skip_dead(); return *this; }
        iterator  operator++(int) { auto tmp = *this; ++(*this); return tmp; }

        bool operator==(const iterator& o) const { return idx_ == o.idx_; }
        bool operator!=(const iterator& o) const { return idx_ != o.idx_; }

    private:
        void skip_dead() {
            while (idx_ < static_cast<u32>(map_->alive_.size()) && !map_->alive_[idx_])
                ++idx_;
        }
        SlotMap* map_;
        u32 idx_;
    };

    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = T;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const T*;
        using reference         = const T&;

        const_iterator(const SlotMap* map, u32 idx) : map_(map), idx_(idx) { skip_dead(); }

        reference operator*() const { return map_->values_[idx_]; }
        pointer   operator->() const { return &map_->values_[idx_]; }

        const_iterator& operator++() { ++idx_; skip_dead(); return *this; }
        const_iterator  operator++(int) { auto tmp = *this; ++(*this); return tmp; }

        bool operator==(const const_iterator& o) const { return idx_ == o.idx_; }
        bool operator!=(const const_iterator& o) const { return idx_ != o.idx_; }

    private:
        void skip_dead() {
            while (idx_ < static_cast<u32>(map_->alive_.size()) && !map_->alive_[idx_])
                ++idx_;
        }
        const SlotMap* map_;
        u32 idx_;
    };

    iterator       begin()        { return iterator(this, 0); }
    iterator       end()          { return iterator(this, static_cast<u32>(alive_.size())); }
    const_iterator begin()  const { return const_iterator(this, 0); }
    const_iterator end()    const { return const_iterator(this, static_cast<u32>(alive_.size())); }

private:
    std::vector<T>    values_;
    std::vector<u32>  generations_;
    std::vector<bool> alive_;
    std::vector<u32>  free_list_;
    std::size_t       size_ = 0;
};

// ---------------------------------------------------------------------------
// SparseSet<T> - sparse-dense set for ECS
// ---------------------------------------------------------------------------
template <typename T>
class SparseSet {
public:
    SparseSet() = default;

    void add(u32 entity, T component) {
        auto it = sparse_.find(entity);
        if (it != sparse_.end()) {
            dense_components_[it->second] = std::move(component);
            return;
        }
        sparse_[entity] = static_cast<u32>(dense_entities_.size());
        dense_entities_.push_back(entity);
        dense_components_.push_back(std::move(component));
    }

    void remove(u32 entity) {
        auto it = sparse_.find(entity);
        if (it == sparse_.end()) return;

        u32 removed_dense = it->second;
        u32 last_entity   = dense_entities_.back();

        // Swap-and-pop
        dense_entities_[removed_dense]   = last_entity;
        dense_components_[removed_dense] = std::move(dense_components_.back());
        sparse_[last_entity]             = removed_dense;

        dense_entities_.pop_back();
        dense_components_.pop_back();
        sparse_.erase(entity);
    }

    bool has(u32 entity) const {
        auto it = sparse_.find(entity);
        return it != sparse_.end()
            && it->second < dense_entities_.size()
            && dense_entities_[it->second] == entity;
    }

    T& get(u32 entity) {
        NEXUS_ASSERT(has(entity), "SparseSet::get - entity not found");
        return dense_components_[sparse_.at(entity)];
    }

    const T& get(u32 entity) const {
        NEXUS_ASSERT(has(entity), "SparseSet::get - entity not found");
        return dense_components_[sparse_.at(entity)];
    }

    std::size_t size() const { return dense_entities_.size(); }

    const std::vector<u32>& entities() const { return dense_entities_; }
    std::vector<T>&         components()       { return dense_components_; }
    const std::vector<T>&   components() const { return dense_components_; }

private:
    // Keyed by entity handle. Using a hash map (rather than a flat vector indexed
    // by the handle) keeps memory proportional to the number of live entities;
    // a flat vector grew to the maximum handle value, which balloons once
    // generational handle reuse pushes handle values into the millions.
    std::unordered_map<u32, u32> sparse_;
    std::vector<u32>             dense_entities_;
    std::vector<T>               dense_components_;
};

// ---------------------------------------------------------------------------
// RingBuffer<T> - fixed-capacity circular buffer
// ---------------------------------------------------------------------------
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity)
        : buffer_(capacity == 0 ? 1 : capacity), capacity_(capacity == 0 ? 1 : capacity) {
        // A zero capacity would make every push/pop compute `% 0` (UB) and index
        // an empty buffer; clamp to at least one slot.
        NEXUS_ASSERT(capacity > 0, "RingBuffer capacity must be non-zero");
    }

    void push(T value) {
        buffer_[tail_] = std::move(value);
        if (full_) {
            head_ = (head_ + 1) % capacity_;
        }
        tail_ = (tail_ + 1) % capacity_;
        full_ = (tail_ == head_);
    }

    std::optional<T> pop() {
        if (empty()) return std::nullopt;
        T val = std::move(buffer_[head_]);
        head_ = (head_ + 1) % capacity_;
        full_ = false;
        return val;
    }

    const T& front() const {
        NEXUS_ASSERT(!empty(), "RingBuffer::front - buffer is empty");
        return buffer_[head_];
    }

    const T& back() const {
        NEXUS_ASSERT(!empty(), "RingBuffer::back - buffer is empty");
        return buffer_[(tail_ + capacity_ - 1) % capacity_];
    }

    std::size_t size() const {
        if (full_) return capacity_;
        if (tail_ >= head_) return tail_ - head_;
        return capacity_ - head_ + tail_;
    }

    bool empty() const { return !full_ && (head_ == tail_); }
    bool full()  const { return full_; }

private:
    std::vector<T> buffer_;
    std::size_t    capacity_ = 0;
    std::size_t    head_     = 0;
    std::size_t    tail_     = 0;
    bool           full_     = false;
};

} // namespace nexus
