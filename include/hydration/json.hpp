/**
 * @file json.hpp
 * @brief Single-pass strict JSON string-array parser and serializer with SWAR scanning,
 *        zero-copy read-only byte span borrowing, empty-entry stripping, and 4-byte-slot deduplication.
 */

#pragma once

#include "hydration/literals.hpp"
#include "hydration/normalize.hpp"
#include "hydration/templates.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * @defgroup json JSON Section
 * @brief Strict JSON array parser, zero-copy borrowed/arena storage, and compact/pretty serializers.
 * @{
 */

/**
 * @struct StringSlice
 * @brief 8-byte offset/length descriptor pointing either into @ref NormalizedArena::source_bytes (borrowed)
 *        or @ref NormalizedArena::data (mutated/normalized).
 */
struct StringSlice {
    static constexpr uint32_t kBorrowedBit = 0x80000000u;
    static constexpr uint32_t kOffsetMask  = 0x7FFFFFFFu;

    uint32_t tagged_offset;
    uint32_t len;

    static constexpr StringSlice makeBorrowed(uint32_t src_offset, uint32_t length) noexcept {
        return StringSlice{kBorrowedBit | (src_offset & kOffsetMask), length};
    }

    static constexpr StringSlice makeOwned(uint32_t arena_offset, uint32_t length) noexcept {
        return StringSlice{arena_offset & kOffsetMask, length};
    }

    constexpr bool isBorrowed() const noexcept {
        return (tagged_offset & kBorrowedBit) != 0u;
    }

    constexpr uint32_t offset() const noexcept {
        return tagged_offset & kOffsetMask;
    }
};

/**
 * @struct NormalizedArena
 * @brief Hybrid zero-copy read-only byte-span + contiguous mutation arena for deduplicated strings.
 */
struct NormalizedArena {
    std::span<const std::byte> source_bytes{}; /**< Read-only byte span over raw input buffer. */
    std::string data;                          /**< Contiguous storage only for mutated/normalized strings. */
    std::vector<StringSlice> slices;           /**< Ordered slice descriptors (borrowed or owned). */
    size_t total_output_bytes = 0;             /**< Exact or tight lower-bound byte count for serialization. */

    void clear() noexcept {
        source_bytes = {};
        data.clear();
        slices.clear();
        total_output_bytes = 0;
    }

    void reserveSlices(size_t item_cap) {
        slices.reserve(item_cap);
    }

    size_t size() const noexcept {
        return slices.size();
    }

    bool empty() const noexcept {
        return slices.empty();
    }

    std::span<const std::byte> byteSpanAt(size_t idx) const noexcept {
        const StringSlice s = slices[idx];
        const std::byte* base = s.isBorrowed()
            ? source_bytes.data()
            : reinterpret_cast<const std::byte*>(data.data());
        return std::span<const std::byte>(base + s.offset(), s.len);
    }

    std::string_view viewAt(size_t idx) const noexcept {
        const StringSlice s = slices[idx];
        const char* base = s.isBorrowed()
            ? reinterpret_cast<const char*>(source_bytes.data())
            : data.data();
        return std::string_view(base + s.offset(), s.len);
    }
};

namespace json_detail {

/**
 * @class FlatArenaSet
 * @brief Open-addressed linear-probing hash set storing 4-byte slice indices.
 */
class FlatArenaSet {
public:
    static constexpr uint32_t kEmpty = 0xFFFFFFFFu;

    explicit FlatArenaSet(const NormalizedArena& arena) noexcept : arena_(&arena) {}

    void reserve(size_t expected_items) {
        size_t target = 16u;
        const size_t min_cap = (expected_items * 10u) / 7u + 4u;
        while (target < min_cap) {
            target <<= 1u;
        }
        if (target > indices_.size()) {
            rehash(target);
        }
    }

    bool insertUniqueSlice(uint32_t candidate_idx) {
        if ((count_ + 1u) * 10u >= indices_.size() * 7u) {
            rehash(indices_.empty() ? 16u : (indices_.size() << 1u));
        }

        const std::span<const std::byte> key = arena_->byteSpanAt(candidate_idx);
        const uint64_t h = fnv1a64(key);
        const size_t mask = indices_.size() - 1u;
        size_t slot = static_cast<size_t>(h) & mask;

        for (;;) {
            const uint32_t existing = indices_[slot];
            if (existing == kEmpty) {
                indices_[slot] = candidate_idx;
                ++count_;
                return true;
            }
            if (arena_->slices[existing].len == key.size()) {
                const std::span<const std::byte> ex_span = arena_->byteSpanAt(existing);
                if (ex_span.size() == 0 ||
                    std::memcmp(ex_span.data(), key.data(), key.size()) == 0) {
                    return false;
                }
            }
            slot = (slot + 1u) & mask;
        }
    }

private:
    static uint64_t fnv1a64(std::span<const std::byte> s) noexcept {
        uint64_t h = 14695981039346656037ULL;
        const std::byte* p = s.data();
        size_t n = s.size();
        while (n >= 8u) {
            uint64_t chunk;
            std::memcpy(&chunk, p, 8u);
            h ^= chunk;
            h *= 1099511628211ULL;
            p += 8u;
            n -= 8u;
        }
        while (n > 0u) {
            h ^= static_cast<uint8_t>(*p++);
            h *= 1099511628211ULL;
            --n;
        }
        // 64-bit avalanche finalizer: propagates upper-byte entropy into low mask bits
        h ^= h >> 33u;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33u;
        return h;
    }

    void rehash(size_t new_cap) {
        std::vector<uint32_t> new_indices(new_cap, kEmpty);
        const size_t mask = new_cap - 1u;

        for (uint32_t idx : indices_) {
            if (idx == kEmpty) {
                continue;
            }
            const std::span<const std::byte> key = arena_->byteSpanAt(idx);
            size_t slot = static_cast<size_t>(fnv1a64(key)) & mask;
            while (new_indices[slot] != kEmpty) {
                slot = (slot + 1u) & mask;
            }
            new_indices[slot] = idx;
        }
        indices_.swap(new_indices);
    }

    const NormalizedArena* arena_ = nullptr;
    std::vector<uint32_t> indices_;
    size_t count_ = 0;
};

inline bool isJsonWs(char c) noexcept {
    return c == literals_c_at(3) || c == literals_c_at(4) ||
           c == literals_c_at(5) || c == literals_c_at(6);
}

inline int hexDigitVal(char c) noexcept {
    if (c >= literals_c_at(7) && c <= literals_c_at(8)) return c - literals_c_at(7);
    if (c >= literals_c_at(9) && c <= 'f') return 10 + (c - literals_c_at(9));
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

inline bool parseHex4(const char* p, uint32_t& out) noexcept {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        const int d = hexDigitVal(p[i]);
        if (d < 0) return false;
        v = (v << 4u) | static_cast<uint32_t>(d);
    }
    out = v;
    return true;
}

inline void appendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((cp >> 6u) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((cp >> 12u) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | ((cp >> 18u) & 0x07u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

/**
 * @brief SWAR 8-byte check for quotes ('"'), backslashes ('\\'), or control bytes (< 0x20).
 */
inline bool hasSpecialByte8(uint64_t v) noexcept {
    constexpr uint64_t kOnes   = 0x0101010101010101ULL;
    constexpr uint64_t kHigh   = 0x8080808080808080ULL;
    constexpr uint64_t kQuotes = 0x2222222222222222ULL; // '"'
    constexpr uint64_t kSlash  = 0x5C5C5C5C5C5C5C5CULL; // '\\'

    const uint64_t ctrl_mask  = (v - (kOnes * 0x20u)) & ~v & kHigh;
    const uint64_t xq = v ^ kQuotes;
    const uint64_t quote_mask = (xq - kOnes) & ~xq & kHigh;
    const uint64_t xs = v ^ kSlash;
    const uint64_t slash_mask = (xs - kOnes) & ~xs & kHigh;

    return (ctrl_mask | quote_mask | slash_mask) != 0ULL;
}

/**
 * @brief SWAR 8-byte check for non-ASCII (>= 0x80) or uppercase ASCII ('A'..'Z').
 */
inline bool hasUpperOrNonAscii8(uint64_t v) noexcept {
    constexpr uint64_t kHigh = 0x8080808080808080ULL;
    if ((v & kHigh) != 0ULL) {
        return true;
    }
    const uint64_t ge_A = v + 0x3F3F3F3F3F3F3F3FULL;
    const uint64_t gt_Z = v + 0x2525252525252525ULL;
    return ((ge_A & ~gt_Z) & kHigh) != 0ULL;
}

/**
 * @brief Returns true if @p data[0..len) is non-empty and already canonical normalized UTF-8
 *        (no leading/trailing ws, no uppercase ASCII, no non-ASCII bytes), enabling zero-copy borrowing.
 */
inline bool isCanonicalAsciiSpan(const char* data, size_t len) noexcept {
    if (len == 0u) {
        return false; // Route empty strings through the empty-entry stripper
    }
    if (static_cast<unsigned char>(data[0]) <= 0x20u ||
        static_cast<unsigned char>(data[len - 1u]) <= 0x20u) {
        return false;
    }

    const char* p = data;
    const char* end = data + len;
    while (end - p >= 8) {
        uint64_t chunk;
        std::memcpy(&chunk, p, 8u);
        if (hasUpperOrNonAscii8(chunk)) {
            return false;
        }
        p += 8;
    }
    while (p < end) {
        const unsigned char c = static_cast<unsigned char>(*p++);
        if (c >= 0x80u || (c >= 'A' && c <= 'Z')) {
            return false;
        }
    }
    return true;
}

inline void writeJsonEscapedOwned(std::string_view s, std::string& out) {
    out.push_back(literals_c_at(10)); // '"'
    const char* p = s.data();
    const char* end = p + s.size();

    while (p < end) {
        const char* run = p;
        while (end - p >= 8) {
            uint64_t chunk;
            std::memcpy(&chunk, p, 8);
            if (hasSpecialByte8(chunk)) {
                break;
            }
            p += 8;
        }
        while (p < end) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c < 0x20u || c == static_cast<unsigned char>(literals_c_at(10)) ||
                c == static_cast<unsigned char>(literals_c_at(11))) {
                break;
            }
            ++p;
        }
        if (p > run) {
            out.append(run, static_cast<size_t>(p - run));
        }
        if (p >= end) {
            break;
        }

        const unsigned char c = static_cast<unsigned char>(*p++);
        switch (c) {
            case '"':
                out.push_back(literals_c_at(11));
                out.push_back(literals_c_at(10));
                break;
            case '\\':
                out.push_back(literals_c_at(11));
                out.push_back(literals_c_at(11));
                break;
            case '\b':
                out.push_back(literals_c_at(11));
                out.push_back(literals_c_at(13));
                break;
            case '\f':
                out.push_back(literals_c_at(11));
                out.push_back(literals_c_at(15));
                break;
            case '\n':
                out.push_back(literals_c_at(11));
                out.push_back(literals_c_at(17));
                break;
            case '\r':
                out.push_back(literals_c_at(11));
                out.push_back(literals_c_at(18));
                break;
            case '\t':
                out.push_back(literals_c_at(11));
                out.push_back(literals_c_at(19));
                break;
            default: {
                char hex_buf[8];
                const int n = std::snprintf(hex_buf, sizeof(hex_buf), templates_cstr_at(6), static_cast<unsigned int>(c));
                if (n > 0) {
                    out.append(hex_buf, static_cast<size_t>(n));
                }
                break;
            }
        }
    }
    out.push_back(literals_c_at(10)); // '"'
}

inline void writeSliceSerialized(const NormalizedArena& arena, size_t idx, std::string& out) {
    const StringSlice s = arena.slices[idx];
    if (s.isBorrowed()) {
        out.push_back(literals_c_at(10)); // '"'
        if (s.len > 0u) {
            const char* ptr = reinterpret_cast<const char*>(arena.source_bytes.data()) + s.offset();
            out.append(ptr, s.len);
        }
        out.push_back(literals_c_at(10)); // '"'
    } else {
        writeJsonEscapedOwned(std::string_view(arena.data.data() + s.offset(), s.len), out);
    }
}

} // namespace json_detail

/**
 * @class StrArrayJson
 * @brief Strict RFC 8259 JSON array-of-strings parser and zero-copy byte-span serializer.
 */
class StrArrayJson {
public:
    explicit StrArrayJson(std::string_view raw) noexcept : raw_(raw) {}

    bool json_parseNormalized(NormalizedArena& arena) const {
        arena.clear();
        const char* base = raw_.data();
        const char* p = base;
        const char* end = base + raw_.size();

        while (p < end && json_detail::isJsonWs(*p)) ++p;
        if (p >= end || *p != literals_c_at(0)) { // '['
            return false;
        }
        ++p;

        while (p < end && json_detail::isJsonWs(*p)) ++p;
        if (p < end && *p == literals_c_at(1)) { // ']'
            ++p;
            while (p < end && json_detail::isJsonWs(*p)) ++p;
            return p == end;
        }

        arena.source_bytes = std::as_bytes(std::span<const char>(base, raw_.size()));

        const size_t est_items = raw_.size() / 16u + 8u;
        arena.reserveSlices(est_items);

        json_detail::FlatArenaSet seen(arena);
        seen.reserve(est_items);

        std::string scratch;
        scratch.reserve(256u);

        for (;;) {
            while (p < end && json_detail::isJsonWs(*p)) ++p;
            if (p >= end || *p != literals_c_at(10)) { // '"'
                arena.clear();
                return false;
            }
            ++p; // consume opening '"'

            bool has_escapes = false;
            const char* raw_str_start = p;

            // SWAR 8-byte scan for closing quote without escapes
            while (end - p >= 8) {
                uint64_t chunk;
                std::memcpy(&chunk, p, 8);
                if (json_detail::hasSpecialByte8(chunk)) {
                    break;
                }
                p += 8;
            }
            while (p < end) {
                const unsigned char c = static_cast<unsigned char>(*p);
                if (c == static_cast<unsigned char>(literals_c_at(10))) { // '"'
                    break;
                }
                if (c == static_cast<unsigned char>(literals_c_at(11)) || c < 0x20u) { // '\\' or ctrl
                    has_escapes = true;
                    break;
                }
                ++p;
            }

            if (!has_escapes) {
                if (p >= end || *p != literals_c_at(10)) {
                    arena.clear();
                    return false;
                }
                const size_t raw_len = static_cast<size_t>(p - raw_str_start);
                const size_t src_off = static_cast<size_t>(raw_str_start - base);
                ++p; // consume closing '"'

                if (src_off <= StringSlice::kOffsetMask &&
                    raw_len <= 0xFFFFFFFFu &&
                    json_detail::isCanonicalAsciiSpan(raw_str_start, raw_len)) {
                    // Zero-copy borrowed byte-span fast path (0 bytes copied to arena.data)
                    const uint32_t slice_idx = static_cast<uint32_t>(arena.slices.size());
                    arena.slices.push_back(StringSlice::makeBorrowed(
                        static_cast<uint32_t>(src_off),
                        static_cast<uint32_t>(raw_len)));

                    if (seen.insertUniqueSlice(slice_idx)) {
                        arena.total_output_bytes += raw_len + 3u;
                    } else {
                        arena.slices.pop_back();
                    }
                } else {
                    // Mutated/Unicode string: materialize normalized bytes into arena.data
                    if (arena.data.capacity() == 0u) {
                        arena.data.reserve(raw_.size() / 2u + 64u);
                    }
                    const size_t slice_start = arena.data.size();
                    const std::string_view raw_item(raw_str_start, raw_len);
                    if (!normalize_utf8Append(raw_item, arena.data)) {
                        arena.clear();
                        return false;
                    }
                    const size_t norm_len = arena.data.size() - slice_start;
                    if (norm_len > 0u) {
                        const uint32_t slice_idx = static_cast<uint32_t>(arena.slices.size());
                        arena.slices.push_back(StringSlice::makeOwned(
                            static_cast<uint32_t>(slice_start),
                            static_cast<uint32_t>(norm_len)));

                        if (seen.insertUniqueSlice(slice_idx)) {
                            arena.total_output_bytes += norm_len + 3u;
                        } else {
                            arena.slices.pop_back();
                            arena.data.resize(slice_start);
                        }
                    }
                }
            } else {
                // Escaped string: unescape into scratch buffer, then normalize into arena.data
                if (arena.data.capacity() == 0u) {
                    arena.data.reserve(raw_.size() / 2u + 64u);
                }
                const size_t slice_start = arena.data.size();
                scratch.assign(raw_str_start, static_cast<size_t>(p - raw_str_start));
                bool closed = false;

                while (p < end) {
                    const unsigned char c = static_cast<unsigned char>(*p++);
                    if (c == static_cast<unsigned char>(literals_c_at(10))) { // '"'
                        closed = true;
                        break;
                    }
                    if (c < 0x20u) {
                        arena.clear();
                        return false;
                    }
                    if (c != static_cast<unsigned char>(literals_c_at(11))) { // not '\\'
                        scratch.push_back(static_cast<char>(c));
                        continue;
                    }

                    if (p >= end) {
                        arena.clear();
                        return false;
                    }
                    const char esc = *p++;
                    switch (esc) {
                        case '"':  scratch.push_back(literals_c_at(10)); break;
                        case '\\': scratch.push_back(literals_c_at(11)); break;
                        case '/':  scratch.push_back(literals_c_at(12)); break;
                        case 'b':  scratch.push_back(literals_c_at(14)); break;
                        case 'f':  scratch.push_back(literals_c_at(16)); break;
                        case 'n':  scratch.push_back(literals_c_at(5));  break;
                        case 'r':  scratch.push_back(literals_c_at(6));  break;
                        case 't':  scratch.push_back(literals_c_at(4));  break;
                        case 'u': {
                            if (end - p < 4) {
                                arena.clear();
                                return false;
                            }
                            uint32_t u1 = 0;
                            if (!json_detail::parseHex4(p, u1)) {
                                arena.clear();
                                return false;
                            }
                            p += 4;

                            if (u1 >= 0xD800u && u1 <= 0xDBFFu) {
                                if (end - p < 6 || p[0] != literals_c_at(11) || p[1] != literals_c_at(20)) {
                                    arena.clear();
                                    return false;
                                }
                                uint32_t u2 = 0;
                                if (!json_detail::parseHex4(p + 2, u2) || u2 < 0xDC00u || u2 > 0xDFFFu) {
                                    arena.clear();
                                    return false;
                                }
                                p += 6;
                                const uint32_t cp = 0x10000u + (((u1 - 0xD800u) << 10u) | (u2 - 0xDC00u));
                                json_detail::appendUtf8(scratch, cp);
                            } else if (u1 >= 0xDC00u && u1 <= 0xDFFFu) {
                                arena.clear();
                                return false;
                            } else {
                                json_detail::appendUtf8(scratch, u1);
                            }
                            break;
                        }
                        default:
                            arena.clear();
                            return false;
                    }
                }

                if (!closed || !normalize_utf8Append(scratch, arena.data)) {
                    arena.clear();
                    return false;
                }

                const size_t norm_len = arena.data.size() - slice_start;
                if (norm_len > 0u) {
                    const uint32_t slice_idx = static_cast<uint32_t>(arena.slices.size());
                    arena.slices.push_back(StringSlice::makeOwned(
                        static_cast<uint32_t>(slice_start),
                        static_cast<uint32_t>(norm_len)));

                    if (seen.insertUniqueSlice(slice_idx)) {
                        arena.total_output_bytes += norm_len + 3u;
                    } else {
                        arena.slices.pop_back();
                        arena.data.resize(slice_start);
                    }
                }
            }

            while (p < end && json_detail::isJsonWs(*p)) ++p;
            if (p >= end) {
                arena.clear();
                return false;
            }
            if (*p == literals_c_at(2)) { // ','
                ++p;
                continue;
            }
            if (*p == literals_c_at(1)) { // ']'
                ++p;
                break;
            }
            arena.clear();
            return false;
        }

        while (p < end && json_detail::isJsonWs(*p)) ++p;
        if (p != end) {
            arena.clear();
            return false;
        }
        return true;
    }

    static void toCompactJson(const NormalizedArena& arena, std::string& out) {
        out.clear();
        out.reserve(arena.total_output_bytes + 4u);
        out.push_back(literals_c_at(0)); // '['
        for (size_t i = 0; i < arena.size(); ++i) {
            if (i > 0) {
                out.push_back(literals_c_at(2)); // ','
            }
            json_detail::writeSliceSerialized(arena, i, out);
        }
        out.push_back(literals_c_at(1)); // ']'
    }

    static void toPrettyJson(const NormalizedArena& arena, std::string& out) {
        out.clear();
        if (arena.empty()) {
            out.push_back(literals_c_at(0)); // '['
            out.push_back(literals_c_at(1)); // ']'
            out.push_back(literals_c_at(5)); // '\n'
            return;
        }
        out.reserve(arena.total_output_bytes + arena.size() * 3u + 8u);
        const std::string_view open_arr  = templates_at(4); // "[\n"
        const std::string_view indent    = templates_at(5); // "  "
        const std::string_view close_arr = templates_at(7); // "]\n"

        out.append(open_arr.data(), open_arr.size());
        for (size_t i = 0; i < arena.size(); ++i) {
            out.append(indent.data(), indent.size());
            json_detail::writeSliceSerialized(arena, i, out);
            if (i + 1 < arena.size()) {
                out.push_back(literals_c_at(2)); // ','
            }
            out.push_back(literals_c_at(5)); // '\n'
        }
        out.append(close_arr.data(), close_arr.size());
    }

private:
    std::string_view raw_;
};

/** @} */ // end of json