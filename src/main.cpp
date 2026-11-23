/**
 * @file main.cpp
 * @brief Entry point for the @c hydration CLI utility.
 */

#include "hydration/cli.hpp"
#include "hydration/runtime.hpp"
#include "hydration/templates.hpp"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

namespace {

void printUsage(bool to_stderr) noexcept {
    runtime_detail::writeRawStream(to_stderr, templates_at(9));
}

void printVersion() noexcept {
    runtime_detail::writeRawStream(false, templates_at(10)); // "hydration 1.0.0\n"
}

bool envTruthy(const char* name) noexcept {
    if (!name || name[0] == '\0') {
        return false;
    }
#ifdef _WIN32
    char buf[32];
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0u || n >= sizeof(buf)) {
        return false;
    }
    return buf[0] == '1' || buf[0] == 't' || buf[0] == 'T' || buf[0] == 'y' || buf[0] == 'Y';
#else
    const char* v = std::getenv(name);
    if (!v || v[0] == '\0') {
        return false;
    }
    return v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y';
#endif
}

bool isManagedEnv() noexcept {
    return envTruthy("HYD_MANAGED") || envTruthy("HYD_NO_PARENT");
}

} // namespace

int main(int argc, char* argv[]) {
    Opts o{};
    if (!cli_parseArgs(argc, argv, o)) {
        printUsage(true);
        return 1;
    }

    if (o.show_help) {
        printUsage(false);
        return 0;
    }

    if (o.show_version) {
        printVersion();
        return 0;
    }

    if (o.child || o.no_parent || isManagedEnv()) {
        return runtime_runWorker(o);
    }

    return runtime_runSelfParent(argc, argv, o);
}