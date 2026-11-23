/**
 * @file templates.hpp
 * @brief Compile-time assembled composite and format template strings from @ref LITS.
 */

#pragma once

#include "hydration/literals.hpp"

#include <string_view>

/**
 * @defgroup templates Templates Section
 * @brief Zero-overhead, compile-time assembled message and format templates.
 * @{
 */

namespace templates_detail {

struct Span {
    size_t offset;
    size_t length;
};

struct TemplateArena {
    static constexpr size_t kBufCap  = 384;
    static constexpr size_t kSpanCap = 11;

    char buf[kBufCap];
    Span spans[kSpanCap];

    constexpr TemplateArena() noexcept : buf{}, spans{} {
        size_t pos = 0;

        auto push_sv = [&](std::string_view sv) {
            for (size_t i = 0; i < sv.size() && pos < kBufCap; ++i) {
                buf[pos++] = sv[i];
            }
        };
        auto push_span = [&](size_t idx) {
            if (idx >= kSpanCap) return;
            const Span s = spans[idx];
            if (s.offset > kBufCap || s.length > kBufCap - s.offset) return;
            for (size_t i = 0; i < s.length && pos < kBufCap; ++i) {
                buf[pos++] = buf[s.offset + i];
            }
        };
        auto begin_entry = [&](size_t idx) {
            if (idx < kSpanCap) {
                spans[idx].offset = (pos <= kBufCap) ? pos : kBufCap;
            }
        };
        auto end_entry = [&](size_t idx) {
            if (idx >= kSpanCap) return;
            const size_t clamped_pos = (pos <= kBufCap) ? pos : kBufCap;
            const size_t start = spans[idx].offset;
            spans[idx].length = (clamped_pos >= start) ? (clamped_pos - start) : 0;
            if (pos < kBufCap) {
                buf[pos++] = '\0';
            }
        };

        // 0: "] "
        begin_entry(0);
        push_sv(literals_char_sv_at(1));
        push_sv(literals_char_sv_at(3));
        end_entry(0);

        // 1: "[ERROR] "
        begin_entry(1);
        push_sv(literals_char_sv_at(0));
        push_sv(literals_sv_at(13));
        push_span(0);
        end_entry(1);

        // 2: "[HEAL_REQUIRED] %s\n"
        begin_entry(2);
        push_sv(literals_char_sv_at(0));
        push_sv(literals_sv_at(14));
        push_span(0);
        push_sv(literals_sv_at(15));
        push_sv(literals_char_sv_at(5));
        end_entry(2);

        // 3: "[ERROR] Cache '%s' is missing or invalid JSON and no external parent supplied seed data; the file was not created or overwritten.\n"
        begin_entry(3);
        push_span(1);
        push_sv(literals_sv_at(16));
        push_sv(literals_char_sv_at(5));
        end_entry(3);

        // 4: "[\n"
        begin_entry(4);
        push_sv(literals_char_sv_at(0));
        push_sv(literals_char_sv_at(5));
        end_entry(4);

        // 5: "  "
        begin_entry(5);
        push_sv(literals_char_sv_at(3));
        push_sv(literals_char_sv_at(3));
        end_entry(5);

        // 6: "\\u%04x"
        begin_entry(6);
        push_sv(literals_char_sv_at(11));
        push_sv(literals_char_sv_at(20));
        push_sv(literals_sv_at(8));
        end_entry(6);

        // 7: "]\n"
        begin_entry(7);
        push_sv(literals_char_sv_at(1));
        push_sv(literals_char_sv_at(5));
        end_entry(7);

        // 8: "HEAL_NEEDED\n"
        begin_entry(8);
        push_sv(literals_sv_at(9));
        push_sv(literals_char_sv_at(5));
        end_entry(8);

        // 9: Usage string
        begin_entry(9);
        push_sv(literals_sv_at(10));
        push_sv(literals_char_sv_at(3));
        push_sv(literals_sv_at(11));
        push_sv(literals_char_sv_at(3));
        push_sv(literals_char_sv_at(0)); push_sv(literals_sv_at(0)); push_span(0);
        push_sv(literals_char_sv_at(0)); push_sv(literals_sv_at(1)); push_span(0);
        push_sv(literals_char_sv_at(0)); push_sv(literals_sv_at(2)); push_span(0);
        push_sv(literals_char_sv_at(0)); push_sv(literals_sv_at(3)); push_span(0);
        push_sv(literals_sv_at(28));
        push_sv(literals_char_sv_at(0)); push_sv(literals_sv_at(4));
        push_sv(literals_char_sv_at(0)); push_sv(literals_sv_at(12));
        push_sv(literals_char_sv_at(1)); push_sv(literals_char_sv_at(1));
        push_sv(literals_char_sv_at(5));
        end_entry(9);

        // 10: "hydration 1.0.0\n"
        begin_entry(10);
        push_sv(literals_sv_at(36));
        push_sv(literals_sv_at(35));
        push_sv(literals_char_sv_at(5));
        end_entry(10);
    }
};

constexpr TemplateArena ARENA{};

} // namespace templates_detail

/**
 * @brief Retrieves a null-terminated @c std::string_view template in O(1) with zero runtime overhead.
 */
constexpr std::string_view templates_at(size_t idx) noexcept {
    const auto s = templates_detail::ARENA.spans[idx];
    return std::string_view(templates_detail::ARENA.buf + s.offset, s.length);
}

/**
 * @brief Retrieves a null-terminated C-string pointer for a template in O(1).
 */
constexpr const char* templates_cstr_at(size_t idx) noexcept {
    return templates_detail::ARENA.buf + templates_detail::ARENA.spans[idx].offset;
}

/** @} */ // end of templates