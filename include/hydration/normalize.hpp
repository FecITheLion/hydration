/**
 * @file normalize.hpp
 * @brief UTF-8 validation, ASCII/Unicode case-folding, whitespace trimming, and zero-width codepoint stripping
 *        with SWAR 8-byte pure-ASCII fast paths.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

/**
 * @defgroup normalize Normalize Section
 * @brief Strict UTF-8 validation, trimming, case folding, and invisible character removal.
 * @{
 */

namespace normalize_detail {

inline bool isAsciiWs(unsigned char c) noexcept {
    return c == 0x20u || c == 0x09u || c == 0x0Au || c == 0x0Du;
}

inline bool isZeroWidthCodepoint(uint32_t cp) noexcept {
    return cp == 0x200Bu || // ZERO WIDTH SPACE
           cp == 0x200Cu || // ZERO WIDTH NON-JOINER
           cp == 0x200Du || // ZERO WIDTH JOINER
           cp == 0x2060u || // WORD JOINER
           cp == 0xFEFFu;   // ZERO WIDTH NO-BREAK SPACE / BOM
}

inline bool isUnicodeSpace(uint32_t cp) noexcept {
    return cp == 0x0020u || cp == 0x0009u || cp == 0x000Au || cp == 0x000Du ||
           cp == 0x000Bu || cp == 0x000Cu || cp == 0x00A0u || cp == 0x1680u ||
           (cp >= 0x2000u && cp <= 0x200Au) ||
           cp == 0x2028u || cp == 0x2029u || cp == 0x202Fu ||
           cp == 0x205Fu || cp == 0x3000u;
}

/**
 * @brief Folds common Latin/Greek/Cyrillic uppercase codepoints to lowercase.
 */
inline uint32_t foldCodepoint(uint32_t cp) noexcept {
    if (cp >= 0x41u && cp <= 0x5Au) {
        return cp + 0x20u; // ASCII 'A'-'Z'
    }
    if ((cp >= 0xC0u && cp <= 0xD6u) || (cp >= 0xD8u && cp <= 0xDEu)) {
        return cp + 0x20u; // Latin-1 Supplement uppercase
    }
    if (cp >= 0x0100u && cp <= 0x017Eu && (cp & 1u) == 0u) {
        if (cp != 0x0130u) { // Skip Turkish dotted I
            return cp + 1u;
        }
    }
    if (cp >= 0x0391u && cp <= 0x03ABu && cp != 0x03A2u) {
        return cp + 0x20u; // Greek uppercase
    }
    if (cp >= 0x0410u && cp <= 0x042Fu) {
        return cp + 0x20u; // Cyrillic uppercase
    }
    if (cp >= 0x0400u && cp <= 0x040Fu) {
        return cp + 0x50u; // Cyrillic extensions
    }
    return cp;
}

inline void appendCodepointUtf8(std::string& out, uint32_t cp) {
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
 * @brief SWAR 8-byte check for any non-ASCII byte (>= 0x80).
 */
inline bool hasNonAscii8(uint64_t v) noexcept {
    return (v & 0x8080808080808080ULL) != 0ULL;
}

/**
 * @brief SWAR 8-byte ASCII lowercase folder (assumes all 8 bytes < 0x80).
 */
inline uint64_t foldAscii8(uint64_t v) noexcept {
    // Byte c in ['A'(0x41) .. 'Z'(0x5A)] iff (c + 0x3F) >= 0x80 and (c + 0x25) < 0x80
    const uint64_t ge_A = v + 0x3F3F3F3F3F3F3F3FULL;
    const uint64_t gt_Z = v + 0x2525252525252525ULL;
    const uint64_t upper_mask = ((ge_A & ~gt_Z) & 0x8080808080808080ULL) >> 2u; // 0x20 on uppercase bytes
    return v | upper_mask;
}

inline bool isPureAscii(const unsigned char* p, size_t len) noexcept {
    const unsigned char* end = p + len;
    while (end - p >= 8) {
        uint64_t chunk;
        std::memcpy(&chunk, p, 8u);
        if (hasNonAscii8(chunk)) {
            return false;
        }
        p += 8;
    }
    while (p < end) {
        if (*p++ >= 0x80u) {
            return false;
        }
    }
    return true;
}

} // namespace normalize_detail

/**
 * @brief Validates UTF-8, trims leading/trailing whitespace, strips zero-width codepoints,
 *        folds case, and appends the normalized bytes directly to @p out.
 *
 * Uses a SWAR 8-byte pure-ASCII fast path whenever no bytes >= 0x80 are present.
 *
 * @return @c true on valid UTF-8, @c false if malformed UTF-8 sequences are encountered.
 */
inline bool normalize_utf8Append(std::string_view src, std::string& out) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(src.data());
    const unsigned char* end = p + src.size();

    // Fast ASCII leading/trailing trim
    while (p < end && normalize_detail::isAsciiWs(*p)) {
        ++p;
    }
    while (end > p && normalize_detail::isAsciiWs(*(end - 1))) {
        --end;
    }

    const size_t trimmed_len = static_cast<size_t>(end - p);
    if (trimmed_len == 0u) {
        return true;
    }

    // SWAR Pure-ASCII Fast Path: fold 8 bytes per cycle with zero codepoint decoding
    if (normalize_detail::isPureAscii(p, trimmed_len)) {
        const size_t start_off = out.size();
        out.resize(start_off + trimmed_len);
        char* dst = out.data() + start_off;

        while (end - p >= 8) {
            uint64_t chunk;
            std::memcpy(&chunk, p, 8u);
            chunk = normalize_detail::foldAscii8(chunk);
            std::memcpy(dst, &chunk, 8u);
            p += 8;
            dst += 8;
        }
        while (p < end) {
            unsigned char c = *p++;
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<unsigned char>(c + 0x20u);
            }
            *dst++ = static_cast<char>(c);
        }
        return true;
    }

    // Full UTF-8 decode + zero-width strip + Unicode whitespace trim + case fold
    const size_t initial_out_size = out.size();
    bool seen_non_ws = false;
    size_t last_non_ws_end = initial_out_size;

    while (p < end) {
        uint32_t cp = 0;
        const unsigned char b0 = *p++;

        if (b0 < 0x80u) {
            cp = b0;
        } else if ((b0 & 0xE0u) == 0xC0u) {
            if (p >= end) {
                out.resize(initial_out_size);
                return false;
            }
            const unsigned char b1 = *p++;
            if ((b1 & 0xC0u) != 0x80u) {
                out.resize(initial_out_size);
                return false;
            }
            cp = ((static_cast<uint32_t>(b0 & 0x1Fu)) << 6u) |
                 static_cast<uint32_t>(b1 & 0x3Fu);
            if (cp < 0x80u) {
                out.resize(initial_out_size);
                return false; // Overlong encoding
            }
        } else if ((b0 & 0xF0u) == 0xE0u) {
            if (end - p < 2) {
                out.resize(initial_out_size);
                return false;
            }
            const unsigned char b1 = *p++;
            const unsigned char b2 = *p++;
            if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) {
                out.resize(initial_out_size);
                return false;
            }
            cp = ((static_cast<uint32_t>(b0 & 0x0Fu)) << 12u) |
                 ((static_cast<uint32_t>(b1 & 0x3Fu)) << 6u) |
                 static_cast<uint32_t>(b2 & 0x3Fu);
            if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) {
                out.resize(initial_out_size);
                return false; // Overlong or UTF-16 surrogate
            }
        } else if ((b0 & 0xF8u) == 0xF0u) {
            if (end - p < 3) {
                out.resize(initial_out_size);
                return false;
            }
            const unsigned char b1 = *p++;
            const unsigned char b2 = *p++;
            const unsigned char b3 = *p++;
            if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u || (b3 & 0xC0u) != 0x80u) {
                out.resize(initial_out_size);
                return false;
            }
            cp = ((static_cast<uint32_t>(b0 & 0x07u)) << 18u) |
                 ((static_cast<uint32_t>(b1 & 0x3Fu)) << 12u) |
                 ((static_cast<uint32_t>(b2 & 0x3Fu)) << 6u) |
                 static_cast<uint32_t>(b3 & 0x3Fu);
            if (cp < 0x10000u || cp > 0x10FFFFu) {
                out.resize(initial_out_size);
                return false; // Overlong or out of Unicode range
            }
        } else {
            out.resize(initial_out_size);
            return false;
        }

        if (normalize_detail::isZeroWidthCodepoint(cp)) {
            continue;
        }

        if (normalize_detail::isUnicodeSpace(cp)) {
            if (!seen_non_ws) {
                continue; // Trim leading Unicode whitespace
            }
            out.push_back(' '); // Normalize interior Unicode whitespace to ASCII space
        } else {
            seen_non_ws = true;
            const uint32_t folded = normalize_detail::foldCodepoint(cp);
            normalize_detail::appendCodepointUtf8(out, folded);
            last_non_ws_end = out.size();
        }
    }

    out.resize(last_non_ws_end);
    return true;
}

/**
 * @brief Byte-span overload of @ref normalize_utf8Append for read-only byte buffers.
 */
inline bool normalize_utf8Append(std::span<const std::byte> src, std::string& out) {
    return normalize_utf8Append(
        std::string_view(reinterpret_cast<const char*>(src.data()), src.size()), out);
}

/** @} */ // end of normalize