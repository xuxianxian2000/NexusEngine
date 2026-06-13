#include "nexus/net/serialization.h"
#include <algorithm>
#include <cmath>

namespace nexus::net {

// ── BitWriter ───────────────────────────────────────────────────────────────

BitWriter::BitWriter(u32 initial_capacity_bytes) {
    buffer_.reserve(initial_capacity_bytes);
}

void BitWriter::ensure_capacity(u32 bits) {
    u32 needed_bytes = (bits_written_ + bits + 7) / 8;
    if (needed_bytes > buffer_.size()) {
        buffer_.resize(needed_bytes, 0);
    }
}

void BitWriter::write_bits(u32 value, u32 bit_count) {
    if (bit_count == 0 || bit_count > 32) return;

    ensure_capacity(bit_count);

    for (u32 i = 0; i < bit_count; i++) {
        u32 byte_index = bits_written_ / 8;
        u32 bit_index = bits_written_ % 8;

        if (byte_index >= buffer_.size()) {
            buffer_.push_back(0);
        }

        if (value & (1u << i)) {
            buffer_[byte_index] |= static_cast<u8>(1u << bit_index);
        }

        bits_written_++;
    }
}

void BitWriter::write_bool(bool value) {
    write_bits(value ? 1u : 0u, 1);
}

void BitWriter::write_u8(u8 value) {
    write_bits(value, 8);
}

void BitWriter::write_u16(u16 value) {
    write_bits(value, 16);
}

void BitWriter::write_u32(u32 value) {
    write_bits(value & 0xFFFF, 16);
    write_bits(value >> 16, 16);
}

void BitWriter::write_u64(u64 value) {
    write_u32(static_cast<u32>(value & 0xFFFFFFFF));
    write_u32(static_cast<u32>(value >> 32));
}

void BitWriter::write_i32(i32 value) {
    u32 uval;
    std::memcpy(&uval, &value, sizeof(u32));
    write_u32(uval);
}

void BitWriter::write_f32(f32 value) {
    u32 uval;
    std::memcpy(&uval, &value, sizeof(u32));
    write_u32(uval);
}

void BitWriter::write_string(const std::string& value) {
    u32 len = static_cast<u32>(value.size());
    write_u32(len);
    for (char c : value) {
        write_u8(static_cast<u8>(c));
    }
}

void BitWriter::write_bytes(const u8* data, u32 size) {
    write_u32(size);
    for (u32 i = 0; i < size; i++) {
        write_u8(data[i]);
    }
}

void BitWriter::write_ranged(i32 value, i32 min_val, i32 max_val) {
    if (min_val >= max_val) {
        return; // No bits needed
    }
    i32 clamped = std::clamp(value, min_val, max_val);
    u32 range = static_cast<u32>(max_val - min_val);
    u32 bits = bits_required(range + 1);
    write_bits(static_cast<u32>(clamped - min_val), bits);
}

void BitWriter::flush() {
    // Pad to byte boundary
    u32 remainder = bits_written_ % 8;
    if (remainder > 0) {
        bits_written_ += (8 - remainder);
    }
    buffer_.resize((bits_written_ + 7) / 8, 0);
}

void BitWriter::reset() {
    buffer_.clear();
    bits_written_ = 0;
}

// ── BitReader ───────────────────────────────────────────────────────────────

BitReader::BitReader(const u8* data, u32 size_bytes)
    : data_(data), total_bits_(size_bytes * 8) {}

BitReader::BitReader(const std::vector<u8>& data)
    : data_(data.data()), total_bits_(static_cast<u32>(data.size()) * 8) {}

u32 BitReader::read_bits(u32 bit_count) {
    if (bit_count == 0 || bit_count > 32) {
        error_ = true;
        return 0;
    }

    if (bits_read_ + bit_count > total_bits_) {
        error_ = true;
        return 0;
    }

    u32 value = 0;
    for (u32 i = 0; i < bit_count; i++) {
        u32 byte_index = bits_read_ / 8;
        u32 bit_index = bits_read_ % 8;

        if (data_[byte_index] & (1u << bit_index)) {
            value |= (1u << i);
        }

        bits_read_++;
    }

    return value;
}

bool BitReader::read_bool() {
    return read_bits(1) != 0;
}

u8 BitReader::read_u8() {
    return static_cast<u8>(read_bits(8));
}

u16 BitReader::read_u16() {
    return static_cast<u16>(read_bits(16));
}

u32 BitReader::read_u32() {
    u32 low = read_bits(16);
    u32 high = read_bits(16);
    return low | (high << 16);
}

u64 BitReader::read_u64() {
    u64 low = read_u32();
    u64 high = read_u32();
    return low | (high << 32);
}

i32 BitReader::read_i32() {
    u32 uval = read_u32();
    i32 ival;
    std::memcpy(&ival, &uval, sizeof(i32));
    return ival;
}

f32 BitReader::read_f32() {
    u32 uval = read_u32();
    f32 fval;
    std::memcpy(&fval, &uval, sizeof(f32));
    return fval;
}

std::string BitReader::read_string() {
    u32 len = read_u32();
    if (error_) return "";

    std::string result;
    // Only reserve what the buffer could actually contain, so a forged length
    // can't trigger a huge allocation (the per-byte loop still guards reads).
    result.reserve(std::min(len, remaining_bits() / 8));
    for (u32 i = 0; i < len; i++) {
        result.push_back(static_cast<char>(read_u8()));
        if (error_) return "";
    }
    return result;
}

std::vector<u8> BitReader::read_bytes(u32 size) {
    std::vector<u8> result;
    result.reserve(std::min(size, remaining_bits() / 8));
    for (u32 i = 0; i < size; i++) {
        result.push_back(read_u8());
        if (error_) return {};
    }
    return result;
}

i32 BitReader::read_ranged(i32 min_val, i32 max_val) {
    if (min_val >= max_val) return min_val;
    u32 range = static_cast<u32>(max_val - min_val);
    u32 bits = bits_required(range + 1);
    u32 raw = read_bits(bits);
    return min_val + static_cast<i32>(raw);
}

// ── DeltaCompressor ─────────────────────────────────────────────────────────

std::vector<u8> DeltaCompressor::compress(const std::vector<u8>& baseline,
                                            const std::vector<u8>& current) {
    // Delta format: [u32 baseline_size] [u32 current_size] [bitmask...] [changed_bytes...]
    // Bitmask: 1 bit per byte, indicating if the byte changed
    // Changed bytes follow in order

    u32 max_size = static_cast<u32>(std::max(baseline.size(), current.size()));

    BitWriter writer;
    writer.write_u32(static_cast<u32>(baseline.size()));
    writer.write_u32(static_cast<u32>(current.size()));
    writer.write_u32(static_cast<u32>(max_size));

    // Write bitmask
    std::vector<u8> changed_bytes;
    for (u32 i = 0; i < max_size; i++) {
        u8 base_byte = (i < baseline.size()) ? baseline[i] : 0;
        u8 curr_byte = (i < current.size()) ? current[i] : 0;
        bool changed = (base_byte != curr_byte);
        writer.write_bool(changed);
        if (changed) {
            changed_bytes.push_back(curr_byte);
        }
    }

    // Write changed bytes
    for (u8 b : changed_bytes) {
        writer.write_u8(b);
    }

    writer.flush();
    return writer.data();
}

std::vector<u8> DeltaCompressor::decompress(const std::vector<u8>& baseline,
                                              const std::vector<u8>& delta) {
    BitReader reader(delta);

    u32 base_size = reader.read_u32();
    u32 curr_size = reader.read_u32();
    u32 max_size = reader.read_u32();
    (void)base_size; // we use the actual baseline

    if (reader.has_error()) return {};

    // Read bitmask
    std::vector<bool> changed_mask;
    changed_mask.reserve(max_size);
    for (u32 i = 0; i < max_size; i++) {
        changed_mask.push_back(reader.read_bool());
    }

    // Reconstruct
    std::vector<u8> result(curr_size, 0);
    for (u32 i = 0; i < max_size; i++) {
        if (changed_mask[i]) {
            u8 val = reader.read_u8();
            if (i < curr_size) result[i] = val;
        } else {
            u8 base_byte = (i < baseline.size()) ? baseline[i] : 0;
            if (i < curr_size) result[i] = base_byte;
        }
    }

    return result;
}

f32 DeltaCompressor::ratio(const std::vector<u8>& original,
                            const std::vector<u8>& compressed) {
    if (original.empty()) return 0.0f;
    return static_cast<f32>(compressed.size()) / static_cast<f32>(original.size());
}

} // namespace nexus::net
