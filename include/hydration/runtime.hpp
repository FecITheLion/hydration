/**
 * @file runtime.hpp
 * @brief Execution runtime for cache lookup, probabilistic O(1) structural boundary sniffing,
 *        zero-copy span JSON ingestion, custom validator execution, and native Win32/POSIX output emission.
 */

#pragma once

#include "hydration/cli.hpp"
#include "hydration/io.hpp"
#include "hydration/json.hpp"
#include "hydration/literals.hpp"
#include "hydration/normalize.hpp"
#include "hydration/templates.hpp"

#include <cstdint>
#include <cstdio>
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
#else
  #include <cerrno>
  #include <unistd.h>
#endif

namespace runtime_detail {

enum class PayloadKind : uint8_t {
    EmptyOrInvalid = 0,
    Json           = 1,
    Html           = 2,
    PlainString    = 3
};

struct SniffScores {
    int16_t json   = 0; // Scaled x100 (25 = 0.25)
    int16_t html   = 0; // Scaled x100 (25 = 0.25)
    int16_t string = 0; // Scaled x100 (50 = 0.50)
    PayloadKind primary = PayloadKind::EmptyOrInvalid;
    bool try_secondary  = false;
    bool is_numeric_candidate = false;
};

/**
 * @brief Compile-time 128-bit ASCII character set mask for O(1) branchless membership tests.
 */
struct AsciiSet128 {
    uint64_t lo = 0;
    uint64_t hi = 0;

    constexpr void add(char ch) noexcept {
        const uint8_t u = static_cast<uint8_t>(ch);
        if (u < 64u) {
            lo |= (1ULL << u);
        } else if (u < 128u) {
            hi |= (1ULL << (u - 64u));
        }
    }

    constexpr void addRange(char first, char last) noexcept {
        for (uint8_t c = static_cast<uint8_t>(first); c <= static_cast<uint8_t>(last); ++c) {
            add(static_cast<char>(c));
        }
    }

    constexpr bool contains(unsigned char u) const noexcept {
        if (u < 64u) {
            return ((lo >> u) & 1ULL) != 0ULL;
        }
        if (u < 128u) {
            return ((hi >> (u - 64u)) & 1ULL) != 0ULL;
        }
        return false;
    }
};

constexpr AsciiSet128 makeJsonFirstSet() noexcept {
    AsciiSet128 s{};
    s.add('[');
    s.add('{');
    s.add('"');
    s.add('t');
    s.add('T');
    s.add('f');
    s.add('F');
    s.add('n');
    s.add('N');
    return s;
}

constexpr AsciiSet128 makeJsonLastSet() noexcept {
    AsciiSet128 s{};
    s.add(']');
    s.add('}');
    s.add('"');
    s.add('e');
    s.add('E');
    s.add('l');
    s.add('L');
    s.addRange('0', '9');
    return s;
}

inline constexpr AsciiSet128 kJsonFirstChars = makeJsonFirstSet();
inline constexpr AsciiSet128 kJsonLastChars  = makeJsonLastSet();

/**
 * @brief Writes a raw byte slice directly to @c STD_OUTPUT_HANDLE or @c STD_ERROR_HANDLE
 *        via native Win32 @c WriteFile (or POSIX @c write), bypassing UCRT FILE locking & buffering.
 */
inline bool writeRawStream(bool to_stderr, std::string_view data) noexcept {
    if (data.empty()) {
        return true;
    }
#ifdef _WIN32
    const HANDLE hOut = ::GetStdHandle(to_stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (hOut == nullptr || hOut == INVALID_HANDLE_VALUE) {
        return false;
    }

    const char* src = data.data();
    size_t remaining = data.size();

    while (remaining > 0u) {
        const DWORD to_write = remaining > 0x7FFFFFFFu
            ? 0x7FFFFFFFu
            : static_cast<DWORD>(remaining);
        DWORD written = 0;
        if (!::WriteFile(hOut, src, to_write, &written, nullptr) || written != to_write) {
            return false;
        }
        src += written;
        remaining -= static_cast<size_t>(written);
    }
    return true;
#else
    const int fd = to_stderr ? STDERR_FILENO : STDOUT_FILENO;
    const char* src = data.data();
    size_t remaining = data.size();

    while (remaining > 0u) {
        const ssize_t n = ::write(fd, src, remaining);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        src += static_cast<size_t>(n);
        remaining -= static_cast<size_t>(n);
    }
    return true;
#endif
}

inline bool emitToStdout(std::string_view out) noexcept {
    if (out.empty()) {
        return true;
    }
    if (!writeRawStream(false, out)) {
        return false;
    }
    const std::string_view nl = literals_char_sv_at(5); // "\n"
    if (out.back() != nl[0]) {
        return writeRawStream(false, nl);
    }
    return true;
}

/**
 * @brief Single-syscall overload for mutable @c std::string buffers: appends '\n' in-place
 *        before calling @c WriteFile so pipe consumers receive payload + newline in 1 syscall.
 */
inline bool emitOwnedStdout(std::string& out) {
    if (out.empty()) {
        return true;
    }
    const char nl = literals_c_at(5); // '\n'
    if (out.back() != nl) {
        out.push_back(nl);
    }
    return writeRawStream(false, out);
}

inline std::string_view normalizeInputView(std::string_view src) noexcept {
    if (src.size() >= 3 &&
        static_cast<unsigned char>(src[0]) == 0xEF &&
        static_cast<unsigned char>(src[1]) == 0xBB &&
        static_cast<unsigned char>(src[2]) == 0xBF) {
        src.remove_prefix(3);
    }

    while (!src.empty() &&
           (src.front() == literals_c_at(3) || src.front() == literals_c_at(4) ||
            src.front() == literals_c_at(6) || src.front() == literals_c_at(5))) {
        src.remove_prefix(1);
    }

    while (!src.empty() &&
           (src.back() == literals_c_at(3) || src.back() == literals_c_at(4) ||
            src.back() == literals_c_at(6) || src.back() == literals_c_at(5))) {
        src.remove_suffix(1);
    }

    return src;
}

inline bool isValidJsonNumber(std::string_view s) noexcept {
    if (s.empty()) {
        return false;
    }
    size_t i = 0;
    if (s[i] == '-') {
        ++i;
        if (i >= s.size()) {
            return false;
        }
    }

    if (s[i] == '0') {
        ++i;
        if (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            return false;
        }
    } else if (s[i] >= '1' && s[i] <= '9') {
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            ++i;
        }
    } else {
        return false;
    }

    if (i < s.size() && s[i] == '.') {
        ++i;
        if (i >= s.size() || s[i] < '0' || s[i] > '9') {
            return false;
        }
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            ++i;
        }
    }

    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            ++i;
        }
        if (i >= s.size() || s[i] < '0' || s[i] > '9') {
            return false;
        }
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            ++i;
        }
    }

    return i == s.size();
}

inline bool isLikelyValidHtml(std::string_view s) noexcept {
    if (s.size() < 3u || s.front() != '<' || s.back() != '>') {
        return false;
    }
    const unsigned char c1 = static_cast<unsigned char>(s[1]);
    return (c1 >= 'a' && c1 <= 'z') ||
           (c1 >= 'A' && c1 <= 'Z') ||
           c1 == '!' || c1 == '?' || c1 == '/';
}

/**
 * @brief Probabilistic O(1) payload sniffer using 128-bit ASCII bitmasks and integer likelihood weights.
 */
inline SniffScores sniffPayloadProbabilistic(std::string_view raw, std::string_view& clean_out) noexcept {
    SniffScores res{};
    clean_out = normalizeInputView(raw);

    if (clean_out.empty()) {
        return res;
    }

    res.json   = 25;
    res.html   = 25;
    res.string = 50;

    const unsigned char first = static_cast<unsigned char>(clean_out.front());
    const unsigned char last  = static_cast<unsigned char>(clean_out.back());

    const bool first_is_num = (first >= '0' && first <= '9') || first == '-';
    if (first_is_num) {
        res.html -= 5;
        res.json += 5;
        res.is_numeric_candidate = true;
    } else if (first == '<') {
        res.json -= 5;
        res.html += 5;
    } else if (kJsonFirstChars.contains(first)) {
        res.json += 5;
        res.html -= 5;
    } else {
        res.json -= 5;
        res.html -= 5;
    }

    if (res.html >= res.json && last == '>') {
        res.json -= 5;
        res.html += 5;
    } else if (kJsonLastChars.contains(last)) {
        res.json += 5;
        res.html -= 5;
    } else {
        res.json -= 5;
        res.html -= 5;
    }

    if (res.json > 25 || res.html > 25) {
        if (res.json >= res.html) {
            res.primary = PayloadKind::Json;
            res.try_secondary = (res.html > 25);
        } else {
            res.primary = PayloadKind::Html;
            res.try_secondary = (res.json > 25);
        }
    } else {
        res.primary = PayloadKind::PlainString;
    }

    return res;
}

inline bool parseStrictJsonArrayNormalized(std::string_view raw, NormalizedArena& arena) {
    arena.clear();
    std::string_view clean;
    const SniffScores scores = sniffPayloadProbabilistic(raw, clean);
    if (scores.primary != PayloadKind::Json) {
        return false;
    }
    if (clean.front() != literals_c_at(0) || clean.back() != literals_c_at(1)) {
        return false;
    }
    return StrArrayJson(clean).json_parseNormalized(arena);
}

inline void fail(const Opts& o, const char* reason) noexcept {
    const std::string_view prefix = templates_at(1); // "[ERROR] "
    writeRawStream(true, prefix);
    const std::string_view msg = reason ? std::string_view(reason) : literals_sv_at(19);
    writeRawStream(true, msg);
    if (!o.cache.empty()) {
        const std::string_view mid = literals_sv_at(20);    // "; file '"
        const std::string_view suffix = literals_sv_at(21); // "' was not created or overwritten."
        writeRawStream(true, mid);
        writeRawStream(true, o.cache);
        writeRawStream(true, suffix);
    }
    writeRawStream(true, literals_char_sv_at(5)); // "\n"
}

inline void serializeArena(const Opts& o, const NormalizedArena& arena, std::string& out) {
    if (o.pretty) {
        StrArrayJson::toPrettyJson(arena, out);
    } else {
        StrArrayJson::toCompactJson(arena, out);
    }
}

inline bool tryEmitCached(const Opts& o) {
    if (o.cache.empty()) {
        return false;
    }

    std::string cached_raw;
    if (!io_readFile(o.cache, cached_raw, o.max_bytes) || cached_raw.empty()) {
        return false;
    }

    if (o.has_validator) {
        if (!io_runValidator(o.validator, cached_raw)) {
            return false;
        }
        return emitOwnedStdout(cached_raw);
    }

    std::string_view clean_cached;
    const SniffScores scores = sniffPayloadProbabilistic(cached_raw, clean_cached);
    if (scores.primary != PayloadKind::Json ||
        clean_cached.front() != literals_c_at(0) ||
        clean_cached.back() != literals_c_at(1)) {
        return false;
    }

    NormalizedArena arena;
    if (!StrArrayJson(clean_cached).json_parseNormalized(arena)) {
        return false;
    }

    std::string out;
    serializeArena(o, arena, out);

    arena = NormalizedArena();
    cached_raw = std::string();

    return emitOwnedStdout(out);
}

inline int writeAndEmitArena(const Opts& o, NormalizedArena& arena, std::string* owned_seed = nullptr) {
    std::string compact;
    StrArrayJson::toCompactJson(arena, compact);

    if (o.pretty) {
        std::string pretty;
        StrArrayJson::toPrettyJson(arena, pretty);

        arena = NormalizedArena();
        if (owned_seed) {
            *owned_seed = std::string();
        }

        if (!o.cache.empty()) {
            if (!io_writeFile(o.cache, compact)) {
                fail(o, literals_s_at(22)); // "Failed writing cache file"
                return 1;
            }
        }
        compact = std::string();

        if (!emitOwnedStdout(pretty)) {
            fail(o, literals_s_at(23)); // "Failed writing output to stdout"
            return 1;
        }
        return 0;
    }

    arena = NormalizedArena();
    if (owned_seed) {
        *owned_seed = std::string();
    }

    if (!o.cache.empty()) {
        if (!io_writeFile(o.cache, compact)) {
            fail(o, literals_s_at(22)); // "Failed writing cache file"
            return 1;
        }
    }

    if (!emitOwnedStdout(compact)) {
        fail(o, literals_s_at(23)); // "Failed writing output to stdout"
        return 1;
    }
    return 0;
}

inline int parseAndPersistSeed(const Opts& o, std::string_view raw_seed, std::string* owned_seed = nullptr) {
    if (o.has_validator) {
        if (!io_runValidator(o.validator, raw_seed)) {
            fail(o, literals_s_at(25)); // "Invalid seed JSON array"
            return 1;
        }
        if (!o.cache.empty()) {
            if (!io_writeFile(o.cache, raw_seed)) {
                fail(o, literals_s_at(22)); // "Failed writing cache file"
                return 1;
            }
        }
        if (!emitToStdout(raw_seed)) {
            fail(o, literals_s_at(23)); // "Failed writing output to stdout"
            return 1;
        }
        if (owned_seed) {
            *owned_seed = std::string();
        }
        return 0;
    }

    std::string_view clean_seed;
    const SniffScores scores = sniffPayloadProbabilistic(raw_seed, clean_seed);

    if (scores.primary == PayloadKind::EmptyOrInvalid) {
        fail(o, literals_s_at(24)); // "No seed JSON received"
        return 1;
    }

    if (scores.primary != PayloadKind::Json ||
        clean_seed.front() != literals_c_at(0) ||
        clean_seed.back() != literals_c_at(1)) {
        fail(o, literals_s_at(25)); // "Invalid seed JSON array"
        return 1;
    }

    NormalizedArena arena;
    if (!StrArrayJson(clean_seed).json_parseNormalized(arena)) {
        fail(o, literals_s_at(25)); // "Invalid seed JSON array"
        return 1;
    }

    return writeAndEmitArena(o, arena, owned_seed);
}

inline int resolveAndConsumeSeed(const Opts& o) {
    if (o.has_inline) {
        return parseAndPersistSeed(o, o.inline_input);
    }

    std::string payload;

    if (!o.ack.empty()) {
        const std::string_view trimmed_ack = normalizeInputView(o.ack);
        if (trimmed_ack.empty()) {
            fail(o, literals_s_at(24)); // "No seed JSON received"
            return 1;
        }

        if (trimmed_ack.front() == literals_c_at(21)) { // '@'
            const std::string_view ack_path = trimmed_ack.substr(1);
            if (ack_path.empty() || !io_readFile(ack_path, payload, o.max_bytes)) {
                fail(o, literals_s_at(26)); // "Failed reading seed file"
                return 1;
            }
            return parseAndPersistSeed(o, payload, &payload);
        }

        if (o.ack_is_file ||
            (!o.has_validator && trimmed_ack.front() != literals_c_at(0))) {
            if (!io_readFile(trimmed_ack, payload, o.max_bytes)) {
                fail(o, literals_s_at(26)); // "Failed reading seed file"
                return 1;
            }
            return parseAndPersistSeed(o, payload, &payload);
        }

        return parseAndPersistSeed(o, trimmed_ack);
    }

    if (o.handshake) {
        writeRawStream(true, templates_at(8)); // "HEAL_NEEDED\n"
        if (!io_readLineFile(stdin, payload, o.max_bytes)) {
            fail(o, literals_s_at(24)); // "No seed JSON received"
            return 1;
        }
        return parseAndPersistSeed(o, payload, &payload);
    }

    const std::string_view hr = templates_at(2);    // "[HEAL_REQUIRED] %s\n"
    const size_t ph = hr.find(literals_sv_at(15));  // "%s"
    if (ph != std::string_view::npos) {
        writeRawStream(true, hr.substr(0, ph));
        writeRawStream(true, o.cache);
        writeRawStream(true, hr.substr(ph + 2u));
    } else {
        writeRawStream(true, hr);
    }

    if (!io_readStream(stdin, payload, o.max_bytes)) {
        fail(o, literals_s_at(27)); // "Failed reading stdin"
        return 1;
    }

    return parseAndPersistSeed(o, payload, &payload);
}

} // namespace runtime_detail

inline int runtime_runWorker(const Opts& o) {
    io_initStdStreams();

    if (runtime_detail::tryEmitCached(o)) {
        return 0;
    }

    return runtime_detail::resolveAndConsumeSeed(o);
}

inline int runtime_runSelfParent(int argc, char* argv[], const Opts& o) {
    io_initStdStreams();

    // Fast path: warm cache hit never spawns a child
    if (runtime_detail::tryEmitCached(o)) {
        return 0;
    }

    // Direct inline or --acknowledge seed runs in-process
    if (o.has_inline || o.has_ack) {
        return runtime_detail::resolveAndConsumeSeed(o);
    }

#ifdef _WIN32
    // If stdin is a pipe or redirected file, coordinate a --child worker with 1 MB pipes
    // and concurrent readers so large streams never stall and errors roll back cleanly.
    HANDLE hIn = ::GetStdHandle(STD_INPUT_HANDLE);
    const DWORD ftype = (hIn != nullptr && hIn != INVALID_HANDLE_VALUE)
        ? ::GetFileType(hIn)
        : FILE_TYPE_UNKNOWN;

    if (ftype == FILE_TYPE_PIPE || ftype == FILE_TYPE_DISK) {
        std::string piped_stdin;
        if (!io_readStream(stdin, piped_stdin, o.max_bytes)) {
            runtime_detail::fail(o, literals_s_at(27)); // "Failed reading stdin"
            return 1;
        }
        if (piped_stdin.empty()) {
            runtime_detail::fail(o, literals_s_at(24)); // "No seed JSON received"
            return 1;
        }

        std::string child_out;
        std::string child_err;
        const int rc = io_runManagedChild(
            argc, argv, piped_stdin, o.max_bytes, child_out, child_err);
        if (!child_err.empty()) {
            runtime_detail::writeRawStream(true, child_err);
        }
        if (rc == 0) {
            runtime_detail::emitOwnedStdout(child_out);
            return 0;
        }
        return 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    // Standalone cold cache with no explicit seed or piped data -> fail immediately without hanging
    const std::string_view msg = templates_at(3);
    const size_t ph = msg.find(literals_sv_at(15)); // "%s"
    if (ph != std::string_view::npos) {
        runtime_detail::writeRawStream(true, msg.substr(0, ph));
        runtime_detail::writeRawStream(true, o.cache);
        runtime_detail::writeRawStream(true, msg.substr(ph + 2u));
    } else {
        runtime_detail::writeRawStream(true, msg);
    }
    return 1;
}