/**
 * @file cli.hpp
 * @brief Command-line argument parser and configuration options struct (@ref Opts)
 *        for the hydration utility.
 */

#pragma once

#include "hydration/literals.hpp"
#include "hydration/templates.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

/**
 * @defgroup cli CLI Section
 * @brief Flag parsing, size limit parsing, and execution options container.
 * @{
 */

/**
 * @struct Opts
 * @brief Parsed command-line options controlling cache paths, seed ingestion modes,
 *        external validation, output formatting, and worker/parent behavior.
 */
struct Opts {
    std::string cache;
    std::string inline_input;
    std::string ack;
    std::string validator;

    size_t max_bytes = 64u * 1024u * 1024u; // 64 MiB default cap

    bool has_inline    = false;
    bool has_ack       = false;
    bool ack_is_file   = false;
    bool has_validator = false;
    bool pretty        = false;
    bool child         = false;
    bool no_parent     = false;
    bool handshake     = false;
    bool show_help     = false;
    bool show_version  = false;
};

namespace cli_detail {

inline bool equalsFlag(const char* arg, std::string_view flag) noexcept {
    if (!arg) {
        return false;
    }
    return std::string_view(arg) == flag;
}

inline bool startsWithEq(const char* arg, std::string_view prefix_with_eq, std::string_view& val_out) noexcept {
    if (!arg) {
        return false;
    }
    const std::string_view sv(arg);
    if (sv.size() < prefix_with_eq.size()) {
        return false;
    }
    if (sv.substr(0, prefix_with_eq.size()) == prefix_with_eq) {
        val_out = sv.substr(prefix_with_eq.size());
        return true;
    }
    return false;
}

inline void assignValidator(Opts& opts, std::string_view raw_val) {
    if (!raw_val.empty() && raw_val.front() == '@') {
        raw_val.remove_prefix(1u);
    }
    opts.validator.assign(raw_val.data(), raw_val.size());
    opts.has_validator = !opts.validator.empty();
}

inline bool parseSizeBytes(std::string_view s, size_t& out) noexcept {
    if (s.empty()) {
        return false;
    }

    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    if (i >= s.size()) {
        return false;
    }

    uint64_t val = 0;
    bool any_digit = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        any_digit = true;
        const uint64_t d = static_cast<uint64_t>(s[i] - '0');
        if (val > (UINT64_MAX - d) / 10ULL) {
            return false;
        }
        val = val * 10ULL + d;
        ++i;
    }

    if (!any_digit) {
        return false;
    }

    uint64_t mult = 1ULL;
    if (i < s.size()) {
        const char u = s[i];
        if (u == 'k' || u == 'K') {
            mult = 1024ULL;
            ++i;
            if (i < s.size() && (s[i] == 'b' || s[i] == 'B')) {
                ++i;
            }
        } else if (u == 'm' || u == 'M') {
            mult = 1024ULL * 1024ULL;
            ++i;
            if (i < s.size() && (s[i] == 'b' || s[i] == 'B')) {
                ++i;
            }
        } else if (u == 'g' || u == 'G') {
            mult = 1024ULL * 1024ULL * 1024ULL;
            ++i;
            if (i < s.size() && (s[i] == 'b' || s[i] == 'B')) {
                ++i;
            }
        } else if (u == 'b' || u == 'B') {
            mult = 1ULL;
            ++i;
        }
    }

    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    if (i != s.size()) {
        return false;
    }

    if (mult > 1ULL && val > (UINT64_MAX / mult)) {
        return false;
    }

    const uint64_t total = val * mult;
    if (total > static_cast<uint64_t>(SIZE_MAX)) {
        return false;
    }

    out = static_cast<size_t>(total);
    return true;
}

inline void applyDefaultMaxBytes(Opts& o) noexcept {
#ifdef _WIN32
    char buf[64];
    const DWORD n = ::GetEnvironmentVariableA(literals_s_at(28), buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        size_t parsed = 0;
        if (parseSizeBytes(std::string_view(buf, n), parsed) && parsed > 0u) {
            o.max_bytes = parsed;
        }
    }
#else
    const char* env_max = std::getenv(literals_s_at(28));
    if (env_max != nullptr && env_max[0] != '\0') {
        size_t parsed = 0;
        if (parseSizeBytes(std::string_view(env_max), parsed) && parsed > 0u) {
            o.max_bytes = parsed;
        }
    }
#endif
}

} // namespace cli_detail

/**
 * @brief Parses command-line arguments into @p opts.
 * @return @c true if arguments are valid, @c false on missing target, unknown flags, or missing values.
 */
inline bool cli_parseArgs(int argc, char* argv[], Opts& opts) {
    opts = Opts{};
    cli_detail::applyDefaultMaxBytes(opts);

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!arg || arg[0] == '\0') {
            continue;
        }

        std::string_view val;

        if (cli_detail::equalsFlag(arg, "-h") || cli_detail::equalsFlag(arg, "--help")) {
            opts.show_help = true;
            return true;
        }
        if (cli_detail::equalsFlag(arg, "-v") || cli_detail::equalsFlag(arg, "-V") ||
            cli_detail::equalsFlag(arg, "--version")) {
            opts.show_version = true;
            return true;
        }
        if (cli_detail::equalsFlag(arg, "--pretty")) {
            opts.pretty = true;
            continue;
        }
        if (cli_detail::equalsFlag(arg, "--child")) {
            opts.child = true;
            continue;
        }
        if (cli_detail::equalsFlag(arg, "--no-parent")) {
            opts.no_parent = true;
            continue;
        }
        if (cli_detail::equalsFlag(arg, "--handshake")) {
            opts.handshake = true;
            continue;
        }
        if (cli_detail::equalsFlag(arg, "--ack-file")) {
            opts.ack_is_file = true;
            continue;
        }

        // --cache <path> or --cache=<path>
        if (cli_detail::equalsFlag(arg, "--cache") || cli_detail::equalsFlag(arg, "-c")) {
            if (i + 1 >= argc || !argv[i + 1]) {
                return false;
            }
            std::string_view cpath(argv[++i]);
            if (!cpath.empty() && cpath.front() == '@') {
                cpath.remove_prefix(1u);
            }
            opts.cache.assign(cpath.data(), cpath.size());
            continue;
        }
        if (cli_detail::startsWithEq(arg, "--cache=", val)) {
            if (!val.empty() && val.front() == '@') {
                val.remove_prefix(1u);
            }
            opts.cache.assign(val.data(), val.size());
            continue;
        }

        // --inline <json> or --inline=<json>
        if (cli_detail::equalsFlag(arg, "--inline")) {
            if (i + 1 >= argc || !argv[i + 1]) {
                return false;
            }
            opts.inline_input = argv[++i];
            opts.has_inline = true;
            continue;
        }
        if (cli_detail::startsWithEq(arg, "--inline=", val)) {
            opts.inline_input.assign(val.data(), val.size());
            opts.has_inline = true;
            continue;
        }

        // --acknowledge / --ack <json|@file|file>
        if (cli_detail::equalsFlag(arg, "--acknowledge") || cli_detail::equalsFlag(arg, "--ack")) {
            if (i + 1 >= argc || !argv[i + 1]) {
                return false;
            }
            opts.ack = argv[++i];
            opts.has_ack = true;
            continue;
        }
        if (cli_detail::startsWithEq(arg, "--acknowledge=", val) ||
            cli_detail::startsWithEq(arg, "--ack=", val)) {
            opts.ack.assign(val.data(), val.size());
            opts.has_ack = true;
            continue;
        }

        // --validator <cmd|@exe> or --validator=<cmd|@exe>
        if (cli_detail::equalsFlag(arg, "--validator")) {
            if (i + 1 >= argc || !argv[i + 1]) {
                return false;
            }
            cli_detail::assignValidator(opts, argv[++i]);
            continue;
        }
        if (cli_detail::startsWithEq(arg, "--validator=", val)) {
            cli_detail::assignValidator(opts, val);
            continue;
        }

        // --max-bytes <N> or --max-bytes=<N>
        if (cli_detail::equalsFlag(arg, "--max-bytes")) {
            if (i + 1 >= argc || !argv[i + 1]) {
                return false;
            }
            if (!cli_detail::parseSizeBytes(argv[++i], opts.max_bytes)) {
                return false;
            }
            continue;
        }
        if (cli_detail::startsWithEq(arg, "--max-bytes=", val)) {
            if (!cli_detail::parseSizeBytes(val, opts.max_bytes)) {
                return false;
            }
            continue;
        }

        // Positional @<cache_file> (e.g. @test_cache.json)
        if (arg[0] == '@' && opts.cache.empty()) {
            opts.cache = arg + 1;
            if (opts.cache.empty()) {
                return false;
            }
            continue;
        }

        // Positional inline payload without '@' (e.g. '["a","b"]')
        if (arg[0] != '-' && !opts.has_inline) {
            opts.inline_input = arg;
            opts.has_inline = true;
            continue;
        }

        return false;
    }

    return !opts.cache.empty() || opts.has_inline || opts.has_ack;
}

/** @} */ // end of cli