/**
 * @file env.hpp
 * @brief Platform environment queries and external-parent process detection.
 */

#pragma once

#include "hydration/cli.hpp"
#include "hydration/literals.hpp"
#include "hydration/templates.hpp"

#include <cstdlib>
#include <cstring>

#if defined(_WIN32) || defined(_WIN64)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #define HYD_WIN 1
#else
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #define HYD_POSIX 1
#endif

/**
 * @defgroup env Environment Section
 * @brief OS environment variable checks and TTY/pipe parent detection.
 * @{
 */

/**
 * @brief Checks whether an environment variable is set to a non-empty value other than @c "0".
 * @param k Null-terminated environment variable name.
 * @return @c true if the variable is active and truthy; @c false otherwise.
 */
inline bool env_isSet(const char* k) {
#if defined(HYD_WIN)
    char b[4];
    return GetEnvironmentVariableA(k, b, sizeof(b)) > 0 && b[0] != literals_c_at(7);
#else
    const char* v = std::getenv(k);
    return v && *v && std::strcmp(v, templates_cstr_at(13)) != 0;
#endif
}

/**
 * @brief Determines whether the process is managed by an external parent caller.
 * @param o Parsed CLI options.
 * @return @c true if running under an external parent; @c false in standalone self-parent mode.
 */
inline bool env_hasExternalParent(const Opts& o) {
    if (o.child || o.no_parent || o.handshake || o.has_ack ||
        env_isSet(literals_s_at(6)) || env_isSet(literals_s_at(7))) {
        return true;
    }
#if defined(HYD_WIN)
    DWORD m = 0;
    HANDLE hi = GetStdHandle(STD_INPUT_HANDLE), ho = GetStdHandle(STD_OUTPUT_HANDLE), he = GetStdHandle(STD_ERROR_HANDLE);
    return !(hi != INVALID_HANDLE_VALUE && GetConsoleMode(hi, &m) &&
             ho != INVALID_HANDLE_VALUE && GetConsoleMode(ho, &m) &&
             he != INVALID_HANDLE_VALUE && GetConsoleMode(he, &m));
#elif defined(HYD_POSIX)
    return isatty(STDIN_FILENO) == 0 || isatty(STDOUT_FILENO) == 0 || isatty(STDERR_FILENO) == 0;
#else
    return false;
#endif
}

/** @} */ // end of env