/**
 * @file literals.hpp
 * @brief Immutable storage and O(1) indexed/sliced accessors for character and string literals.
 */

#pragma once

#include <string_view>

/**
 * @defgroup literals Literals Section
 * @brief Centralized, deduplicated compile-time literal tables backed by read-only views.
 * @{
 */

/**
 * @struct LitStore
 * @brief Fixed literal container holding a packed read-only character view and string literal table.
 */
struct LitStore {
    std::string_view chars;      /**< [0]: Packed 23-byte read-only character table (indices 0..22). */
    std::string_view strs[38];   /**< [1]: Unique multi-character string literals with compile-time lengths (indices 0..37). */
};

/**
 * @brief Global compile-time instance of @ref LitStore in .rodata.
 */
constexpr LitStore LITS = {
    // [0] Packed 23-byte read-only character view (indices 0..22):
    //  0:'['  1:']'  2:','  3:' '  4:'\t' 5:'\n' 6:'\r' 7:'0'  8:'9'  9:'a' 10:'"' 11:'\\'
    // 12:'/' 13:'b' 14:'\b' 15:'f' 16:'\f' 17:'n' 18:'r' 19:'t' 20:'u' 21:'@' 22:'-'
    std::string_view("[], \t\n\r09a\"\\/b\bf\fn\rtu@-", 23),

    // [1] std::string_view[] — unique multi-character string literals with compile-time lengths (indices 0..37)
    {
        "--pretty",                                                                                       // 0
        "--child",                                                                                        // 1
        "--handshake",                                                                                    // 2
        "--no-parent",                                                                                    // 3
        "--acknowledge",                                                                                  // 4
        "--acknowledge=",                                                                                 // 5
        "HYD_NO_PARENT",                                                                                  // 6
        "HYD_MANAGED",                                                                                    // 7
        "%04x",                                                                                           // 8
        "HEAL_NEEDED",                                                                                    // 9
        "Usage: hydration",                                                                               // 10
        "(<@cache> | '<json>' | '@<file>')",                                                              // 11
        "='<json>'|@<file>",                                                                              // 12
        "ERROR",                                                                                          // 13
        "HEAL_REQUIRED",                                                                                  // 14
        "%s",                                                                                             // 15
        "Cache '%s' is missing or invalid JSON and no external parent supplied seed data; the file was not created or overwritten.", // 16
        "--validator",                                                                                    // 17
        "--validator=",                                                                                   // 18
        "Operation failed",                                                                               // 19
        "; file '",                                                                                       // 20
        "' was not created or overwritten.",                                                              // 21
        "Failed writing cache file",                                                                      // 22
        "Failed writing output to stdout",                                                                // 23
        "No seed JSON received",                                                                          // 24
        "Invalid seed JSON array",                                                                        // 25
        "Failed reading seed file",                                                                       // 26
        "Failed reading stdin",                                                                           // 27
        "[--validator=@<file>] ",                                                                         // 28
        "--help",                                                                                         // 29
        "-h",                                                                                             // 30
        "--version",                                                                                      // 31
        "-v",                                                                                             // 32
        "--max-bytes",                                                                                    // 33
        "--max-bytes=",                                                                                   // 34
        "1.0.0",                                                                                          // 35
        "hydration ",                                                                                     // 36
        "NtQueryInformationProcess"                                                                       // 37
    }
};

static_assert(LITS.chars.size() == 23);
static_assert(LITS.chars[0]  == '[');
static_assert(LITS.chars[1]  == ']');
static_assert(LITS.chars[2]  == ',');
static_assert(LITS.chars[10] == '"');
static_assert(LITS.chars[11] == '\\');
static_assert(LITS.chars[20] == 'u');
static_assert(LITS.chars[21] == '@');
static_assert(LITS.chars[22] == '-');

/**
 * @brief Retrieves a single-character literal from @ref LITS in O(1) (constant-folds to an immediate byte).
 */
constexpr char literals_c_at(size_t idx) noexcept { return LITS.chars[idx]; }

/**
 * @brief Slices a zero-allocation 1-byte read-only @c std::string_view from @ref LITS.chars in O(1).
 */
constexpr std::string_view literals_char_sv_at(size_t idx) noexcept { return LITS.chars.substr(idx, 1); }

/**
 * @brief Retrieves a null-terminated string literal pointer from @ref LITS in O(1).
 */
constexpr const char* literals_s_at(size_t idx) noexcept { return LITS.strs[idx].data(); }

/**
 * @brief Retrieves a sized @c std::string_view literal from @ref LITS in O(1).
 */
constexpr std::string_view literals_sv_at(size_t idx) noexcept { return LITS.strs[idx]; }

/** @} */ // end of literals