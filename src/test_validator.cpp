#include <cstdio>
#include <string>
#include <string_view>

int main(int argc, char* argv[]) {
    std::string in;
    char buf[4096];
    while (size_t n = std::fread(buf, 1, sizeof(buf), stdin)) {
        in.append(buf, n);
    }

    // Optional CLI mode flag for negative/custom validator tests (e.g., "fail", "zero", "false")
    if (argc > 1) {
        std::string_view mode(argv[1]);
        if (mode == "fail") {
            return 2;
        }
        if (mode == "zero") {
            std::fwrite("0\n", 1, 2, stdout);
            return 0;
        }
        if (mode == "false") {
            std::fwrite("false\n", 1, 6, stdout);
            return 0;
        }
        if (mode == "true") {
            std::fwrite("true\n", 1, 5, stdout);
            return 0;
        }
    }

    // Trim leading whitespace
    size_t pos = 0;
    while (pos < in.size() && (in[pos] == ' ' || in[pos] == '\t' || in[pos] == '\r' || in[pos] == '\n')) {
        ++pos;
    }

    if (pos < in.size()) {
        std::string_view trimmed(in.data() + pos, in.size() - pos);
        if (trimmed.rfind("VALID", 0) == 0 || trimmed.front() == '[' || trimmed.front() == '<') {
            std::fwrite("true\n", 1, 5, stdout);
            return 0;
        }
    }

    std::fwrite("false\n", 1, 6, stdout);
    return 0;
}