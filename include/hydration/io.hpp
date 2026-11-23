/**
 * @file io.hpp
 * @brief Native Win32 / POSIX file, pipe, child-process validator, and supervised
 *        concurrent-pipe worker I/O primitives.
 */

#pragma once

#include "hydration/literals.hpp"
#include "hydration/templates.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
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
  #include <fcntl.h>
  #include <io.h>
#else
  #include <cerrno>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

/**
 * @defgroup io I/O Section
 * @brief Low-syscall file read/write, stream ingestion, external validator runner,
 *        and managed child pipe supervisor.
 * @{
 */

inline void io_initStdStreams() noexcept {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
}

#ifdef _WIN32
namespace io_win32_detail {

/**
 * @brief Zero-allocation stack UTF-8 / CP_ACP -> UTF-16 converter for paths < 512 wchar_t,
 *        with automatic heap fallback for extended paths.
 */
struct WidePathBuf {
    wchar_t stack_buf[512];
    std::wstring heap_buf;
    const wchar_t* ptr = nullptr;

    bool assign(std::string_view text) noexcept {
        if (text.empty()) {
            return false;
        }
        const int src_len = static_cast<int>(text.size());
        UINT cp = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;

        int needed = ::MultiByteToWideChar(cp, flags, text.data(), src_len, nullptr, 0);
        if (needed <= 0) {
            cp = CP_ACP;
            flags = 0;
            needed = ::MultiByteToWideChar(cp, flags, text.data(), src_len, nullptr, 0);
            if (needed <= 0) {
                return false;
            }
        }

        if (needed < 512) {
            const int written = ::MultiByteToWideChar(cp, flags, text.data(), src_len, stack_buf, 512);
            if (written <= 0) {
                return false;
            }
            stack_buf[written] = L'\0';
            ptr = stack_buf;
            return true;
        }

        heap_buf.resize(static_cast<size_t>(needed));
        const int written = ::MultiByteToWideChar(cp, flags, text.data(), src_len, heap_buf.data(), needed);
        if (written <= 0) {
            heap_buf.clear();
            return false;
        }
        ptr = heap_buf.c_str();
        return true;
    }
};

inline void ensureParentDirectoriesW(const wchar_t* full_path) noexcept {
    if (!full_path) {
        return;
    }
    std::wstring dir(full_path);
    const size_t last_sep = dir.find_last_of(L"\\/");
    if (last_sep == std::wstring::npos || last_sep == 0u) {
        return;
    }
    dir.resize(last_sep);

    size_t pos = 0;
    if (dir.size() >= 2u && dir[1] == L':') {
        pos = 3u;
    }
    for (; pos <= dir.size(); ++pos) {
        if (pos == dir.size() || dir[pos] == L'\\' || dir[pos] == L'/') {
            if (pos > 0u) {
                const wchar_t saved = dir[pos];
                dir[pos] = L'\0';
                ::CreateDirectoryW(dir.c_str(), nullptr);
                dir[pos] = saved;
            }
        }
    }
}

inline bool writeAllHandleW(HANDLE hFile, std::string_view data) noexcept {
    const char* src = data.data();
    size_t remaining = data.size();

    while (remaining > 0u) {
        const DWORD to_write = remaining > 0x7FFFFFFFu
            ? 0x7FFFFFFFu
            : static_cast<DWORD>(remaining);
        DWORD bytes_written = 0;
        if (!::WriteFile(hFile, src, to_write, &bytes_written, nullptr) ||
            bytes_written != to_write) {
            return false;
        }
        src += bytes_written;
        remaining -= static_cast<size_t>(bytes_written);
    }
    return true;
}

} // namespace io_win32_detail
#else
namespace io_posix_detail {

inline void ensureParentDirectoriesPosix(std::string_view full_path) {
    const size_t last_sep = full_path.find_last_of('/');
    if (last_sep == std::string_view::npos || last_sep == 0u) {
        return;
    }
    std::string dir(full_path.substr(0, last_sep));
    for (size_t pos = 1u; pos <= dir.size(); ++pos) {
        if (pos == dir.size() || dir[pos] == '/') {
            const char saved = dir[pos];
            dir[pos] = '\0';
            ::mkdir(dir.c_str(), 0755);
            dir[pos] = saved;
        }
    }
}

inline bool writeAllFd(int fd, std::string_view data) noexcept {
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
}

} // namespace io_posix_detail
#endif

/**
 * @brief Reads an entire file into @p out using 3 native syscalls (Open, Read, Close)
 *        with @c FILE_FLAG_SEQUENTIAL_SCAN and zero intermediate buffering.
 */
inline bool io_readFile(std::string_view path, std::string& out, size_t max_bytes) {
    out.clear();
#ifdef _WIN32
    io_win32_detail::WidePathBuf wpath;
    if (!wpath.assign(path)) {
        return false;
    }

    HANDLE hFile = ::CreateFileW(
        wpath.ptr,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER file_size{};
    if (!::GetFileSizeEx(hFile, &file_size) || file_size.QuadPart < 0) {
        ::CloseHandle(hFile);
        return false;
    }

    const uint64_t u64_size = static_cast<uint64_t>(file_size.QuadPart);
    if (u64_size > max_bytes) {
        ::CloseHandle(hFile);
        return false;
    }

    const size_t total_bytes = static_cast<size_t>(u64_size);
    if (total_bytes == 0u) {
        ::CloseHandle(hFile);
        return true;
    }

    out.resize(total_bytes);
    char* dst = out.data();
    size_t remaining = total_bytes;

    while (remaining > 0u) {
        const DWORD to_read = remaining > 0x7FFFFFFFu
            ? 0x7FFFFFFFu
            : static_cast<DWORD>(remaining);
        DWORD bytes_read = 0;
        if (!::ReadFile(hFile, dst, to_read, &bytes_read, nullptr)) {
            ::CloseHandle(hFile);
            out.clear();
            return false;
        }
        if (bytes_read == 0u) {
            break;
        }
        dst += bytes_read;
        remaining -= static_cast<size_t>(bytes_read);
    }

    ::CloseHandle(hFile);
    if (remaining != 0u) {
        out.resize(total_bytes - remaining);
    }
    return true;
#else
    std::string path_str(path);
    const int fd = ::open(path_str.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size < 0) {
        ::close(fd);
        return false;
    }

    const uint64_t u64_size = static_cast<uint64_t>(st.st_size);
    if (u64_size > max_bytes) {
        ::close(fd);
        return false;
    }

    const size_t total_bytes = static_cast<size_t>(u64_size);
    if (total_bytes == 0u) {
        ::close(fd);
        return true;
    }

    out.resize(total_bytes);
    char* dst = out.data();
    size_t remaining = total_bytes;

    while (remaining > 0u) {
        const ssize_t n = ::read(fd, dst, remaining);
        if (n < 0) {
            ::close(fd);
            out.clear();
            return false;
        }
        if (n == 0) {
            break;
        }
        dst += static_cast<size_t>(n);
        remaining -= static_cast<size_t>(n);
    }

    ::close(fd);
    if (remaining != 0u) {
        out.resize(total_bytes - remaining);
    }
    return true;
#endif
}

/**
 * @brief Writes @p data to @p path: uses direct @c CREATE_NEW for single-cluster (<= 4 KB)
 *        cold cache misses, and atomic PID/TID-tagged temp file + @c MoveFileExW for larger
 *        or existing files so concurrent readers never observe partial writes.
 */
inline bool io_writeFile(std::string_view path, std::string_view data) {
#ifdef _WIN32
    io_win32_detail::WidePathBuf wtarget;
    if (!wtarget.assign(path)) {
        return false;
    }

    // Fast path on cold cache miss for single-cluster payloads (<= 4 KB):
    // larger payloads use atomic .tmp + MoveFileExW so concurrent readers never see partial writes.
    if (data.size() <= 4096u && ::GetFileAttributesW(wtarget.ptr) == INVALID_FILE_ATTRIBUTES) {
        HANDLE hDirect = ::CreateFileW(
            wtarget.ptr,
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (hDirect != INVALID_HANDLE_VALUE) {
            const bool ok = io_win32_detail::writeAllHandleW(hDirect, data);
            ::CloseHandle(hDirect);
            if (ok) {
                return true;
            }
            ::DeleteFileW(wtarget.ptr);
            return false;
        }

        if (::GetLastError() == ERROR_PATH_NOT_FOUND) {
            io_win32_detail::ensureParentDirectoriesW(wtarget.ptr);
            hDirect = ::CreateFileW(
                wtarget.ptr,
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_DELETE,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (hDirect != INVALID_HANDLE_VALUE) {
                const bool ok = io_win32_detail::writeAllHandleW(hDirect, data);
                ::CloseHandle(hDirect);
                if (ok) {
                    return true;
                }
                ::DeleteFileW(wtarget.ptr);
                return false;
            }
        }
    }

    // Atomic path: PID + TID temp file + MoveFileExW
    std::string tmp_utf8;
    tmp_utf8.reserve(path.size() + 32u);
    tmp_utf8.append(path);
    tmp_utf8.append(".tmp.");
    tmp_utf8.append(std::to_string(::GetCurrentProcessId()));
    tmp_utf8.push_back('.');
    tmp_utf8.append(std::to_string(::GetCurrentThreadId()));

    io_win32_detail::WidePathBuf wtmp;
    if (!wtmp.assign(tmp_utf8)) {
        return false;
    }

    HANDLE hFile = ::CreateFileW(
        wtmp.ptr,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE) {
        io_win32_detail::ensureParentDirectoriesW(wtarget.ptr);
        hFile = ::CreateFileW(
            wtmp.ptr,
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            return false;
        }
    }

    const bool write_ok = io_win32_detail::writeAllHandleW(hFile, data);
    ::CloseHandle(hFile);

    if (!write_ok) {
        ::DeleteFileW(wtmp.ptr);
        return false;
    }

    for (int attempt = 0; attempt < 32; ++attempt) {
        if (::MoveFileExW(wtmp.ptr, wtarget.ptr, MOVEFILE_REPLACE_EXISTING)) {
            return true;
        }
        ::Sleep(1);
    }

    ::DeleteFileW(wtmp.ptr);
    return false;
#else
    std::string target_str(path);

    if (data.size() <= 4096u && ::access(target_str.c_str(), F_OK) != 0) {
        int direct_fd = ::open(target_str.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (direct_fd < 0 && errno == ENOENT) {
            io_posix_detail::ensureParentDirectoriesPosix(path);
            direct_fd = ::open(target_str.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        }
        if (direct_fd >= 0) {
            const bool ok = io_posix_detail::writeAllFd(direct_fd, data);
            ::close(direct_fd);
            if (ok) {
                return true;
            }
            ::unlink(target_str.c_str());
            return false;
        }
    }

    std::string tmp_str = target_str + ".tmp." + std::to_string(::getpid());

    int fd = ::open(tmp_str.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        io_posix_detail::ensureParentDirectoriesPosix(path);
        fd = ::open(tmp_str.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            return false;
        }
    }

    const bool write_ok = io_posix_detail::writeAllFd(fd, data);
    ::close(fd);

    if (!write_ok) {
        ::unlink(tmp_str.c_str());
        return false;
    }
    if (::rename(tmp_str.c_str(), target_str.c_str()) != 0) {
        ::unlink(tmp_str.c_str());
        return false;
    }
    return true;
#endif
}

/**
 * @brief Drains a binary stream (e.g., @c stdin pipe) into @p out using a 256 KB aligned stack
 *        buffer, 512 KB initial pipe reserve, and geometric capacity growth up to @p max_bytes.
 */
inline bool io_readStream(FILE* stream, std::string& out, size_t max_bytes) {
    out.clear();
    if (!stream) {
        return false;
    }

#ifdef _WIN32
    if (stream == stdin) {
        HANDLE hIn = ::GetStdHandle(STD_INPUT_HANDLE);
        if (hIn != nullptr && hIn != INVALID_HANDLE_VALUE) {
            const DWORD ftype = ::GetFileType(hIn);
            if (ftype == FILE_TYPE_DISK) {
                LARGE_INTEGER sz{};
                if (::GetFileSizeEx(hIn, &sz) && sz.QuadPart >= 0) {
                    const uint64_t u64_sz = static_cast<uint64_t>(sz.QuadPart);
                    if (u64_sz > max_bytes) {
                        return false;
                    }
                    out.reserve(static_cast<size_t>(u64_sz));
                }
            } else if (ftype == FILE_TYPE_PIPE || ftype == FILE_TYPE_UNKNOWN) {
                const size_t initial_pipe_reserve = (max_bytes < 524288u) ? max_bytes : 524288u;
                out.reserve(initial_pipe_reserve);
            }

            alignas(64) char buf[262144]; // 256 KB stack read buffer
            for (;;) {
                DWORD bytes_read = 0;
                if (!::ReadFile(hIn, buf, sizeof(buf), &bytes_read, nullptr)) {
                    const DWORD err = ::GetLastError();
                    if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) {
                        break;
                    }
                    out.clear();
                    return false;
                }
                if (bytes_read == 0u) {
                    break;
                }
                if (out.size() + static_cast<size_t>(bytes_read) > max_bytes) {
                    out.clear();
                    return false;
                }
                out.append(buf, static_cast<size_t>(bytes_read));
            }
            return true;
        }
    }
#endif

    alignas(64) char buf[262144]; // 256 KB stack read buffer
    for (;;) {
        const size_t n = std::fread(buf, 1, sizeof(buf), stream);
        if (n > 0u) {
            if (out.size() + n > max_bytes) {
                out.clear();
                return false;
            }
            out.append(buf, n);
        }
        if (n < sizeof(buf)) {
            if (std::ferror(stream)) {
                out.clear();
                return false;
            }
            break;
        }
    }
    return true;
}

/**
 * @brief Reads a single newline-delimited payload line from @p stream up to @p max_bytes.
 */
inline bool io_readLineFile(FILE* stream, std::string& out, size_t max_bytes) {
    out.clear();
    if (!stream) {
        return false;
    }

    for (;;) {
        const int ch = std::fgetc(stream);
        if (ch == EOF) {
            return !out.empty() && !std::ferror(stream);
        }
        if (ch == '\n') {
            return true;
        }
        if (out.size() >= max_bytes) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<char>(ch));
    }
}

namespace io_validator_detail {

inline bool isAcceptableValidatorStdout(std::string_view stdout_data) noexcept {
    while (!stdout_data.empty() &&
           (stdout_data.front() == ' ' || stdout_data.front() == '\t' ||
            stdout_data.front() == '\r' || stdout_data.front() == '\n')) {
        stdout_data.remove_prefix(1u);
    }
    while (!stdout_data.empty() &&
           (stdout_data.back() == ' ' || stdout_data.back() == '\t' ||
            stdout_data.back() == '\r' || stdout_data.back() == '\n')) {
        stdout_data.remove_suffix(1u);
    }

    if (stdout_data.empty()) {
        return true;
    }

    const size_t last_nl = stdout_data.find_last_of("\r\n");
    std::string_view last_line = (last_nl == std::string_view::npos)
        ? stdout_data
        : stdout_data.substr(last_nl + 1u);

    while (!last_line.empty() && (last_line.front() == ' ' || last_line.front() == '\t')) {
        last_line.remove_prefix(1u);
    }

    if (last_line == "0" || last_line == "false" || last_line == "False" || last_line == "FALSE") {
        return false;
    }
    return true;
}

} // namespace io_validator_detail

/**
 * @brief Executes an external validator command, piping @p payload to its stdin,
 *        and returns @c true iff the child exits with status 0 and does not emit '0' or 'false'.
 */
inline bool io_runValidator(std::string_view validator_cmd, std::string_view payload) {
    if (validator_cmd.empty()) {
        return false;
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hChildStdinRd = nullptr;
    HANDLE hChildStdinWr = nullptr;
    if (!::CreatePipe(&hChildStdinRd, &hChildStdinWr, &sa, 0)) {
        return false;
    }
    ::SetHandleInformation(hChildStdinWr, HANDLE_FLAG_INHERIT, 0);

    HANDLE hChildStdoutRd = nullptr;
    HANDLE hChildStdoutWr = nullptr;
    if (!::CreatePipe(&hChildStdoutRd, &hChildStdoutWr, &sa, 0)) {
        ::CloseHandle(hChildStdinRd);
        ::CloseHandle(hChildStdinWr);
        return false;
    }
    ::SetHandleInformation(hChildStdoutRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hChildStdinRd;
    si.hStdOutput = hChildStdoutWr;
    si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};

    io_win32_detail::WidePathBuf wcmd;
    if (!wcmd.assign(validator_cmd)) {
        ::CloseHandle(hChildStdinRd);
        ::CloseHandle(hChildStdinWr);
        ::CloseHandle(hChildStdoutRd);
        ::CloseHandle(hChildStdoutWr);
        return false;
    }

    std::wstring cmdline(wcmd.ptr);
    const BOOL created = ::CreateProcessW(
        nullptr,
        cmdline.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi);

    ::CloseHandle(hChildStdinRd);
    ::CloseHandle(hChildStdoutWr);

    if (!created) {
        ::CloseHandle(hChildStdinWr);
        ::CloseHandle(hChildStdoutRd);
        return false;
    }

    const bool write_ok = io_win32_detail::writeAllHandleW(hChildStdinWr, payload);
    ::CloseHandle(hChildStdinWr);

    std::string child_stdout;
    char out_buf[1024];
    for (;;) {
        DWORD nread = 0;
        if (!::ReadFile(hChildStdoutRd, out_buf, sizeof(out_buf), &nread, nullptr) || nread == 0u) {
            break;
        }
        if (child_stdout.size() < 4096u) {
            child_stdout.append(out_buf, static_cast<size_t>(nread));
        }
    }
    ::CloseHandle(hChildStdoutRd);

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    ::GetExitCodeProcess(pi.hProcess, &exit_code);

    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    return write_ok && (exit_code == 0u) &&
           io_validator_detail::isAcceptableValidatorStdout(child_stdout);
#else
    std::string cmd_str(validator_cmd);
    FILE* pipe = ::popen(cmd_str.c_str(), "w");
    if (!pipe) {
        return false;
    }

    bool write_ok = true;
    if (!payload.empty()) {
        const size_t written = std::fwrite(payload.data(), 1, payload.size(), pipe);
        write_ok = (written == payload.size());
    }

    const int status = ::pclose(pipe);
    return write_ok && status != -1 && WIFEXITED(status) && (WEXITSTATUS(status) == 0);
#endif
}

/**
 * @brief Spawns a flagged child worker (@c --child) with 1 MB anonymous pipes and concurrent
 *        reader threads, enforcing all-or-nothing stdout delivery on exit code 0.
 */
inline int io_runManagedChild(
    int argc,
    char* argv[],
    std::string_view stdin_payload,
    size_t max_bytes,
    std::string& stdout_out,
    std::string& stderr_out) {
    stdout_out.clear();
    stderr_out.clear();

#ifdef _WIN32
    wchar_t exe_path[MAX_PATH];
    const DWORD exe_len = ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (exe_len == 0u || exe_len >= MAX_PATH) {
        return -1;
    }

    std::wstring cmdline;
    cmdline.push_back(L'"');
    cmdline.append(exe_path, exe_len);
    cmdline.append(L"\" --child");

    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) {
            continue;
        }
        io_win32_detail::WidePathBuf warg;
        if (!warg.assign(argv[i])) {
            return -1;
        }
        cmdline.push_back(L' ');
        cmdline.push_back(L'"');
        for (const wchar_t* p = warg.ptr; *p != L'\0'; ++p) {
            if (*p == L'"') {
                cmdline.append(L"\\\"");
            } else {
                cmdline.push_back(*p);
            }
        }
        cmdline.push_back(L'"');
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    constexpr DWORD kPipeBufferSize = 1048576u; // 1 MB ring buffer eliminates 4 KB pipe stalls

    HANDLE hInRd = nullptr, hInWr = nullptr;
    HANDLE hOutRd = nullptr, hOutWr = nullptr;
    HANDLE hErrRd = nullptr, hErrWr = nullptr;

    if (!::CreatePipe(&hInRd, &hInWr, &sa, kPipeBufferSize)) {
        return -1;
    }
    ::SetHandleInformation(hInWr, HANDLE_FLAG_INHERIT, 0);

    if (!::CreatePipe(&hOutRd, &hOutWr, &sa, kPipeBufferSize)) {
        ::CloseHandle(hInRd);
        ::CloseHandle(hInWr);
        return -1;
    }
    ::SetHandleInformation(hOutRd, HANDLE_FLAG_INHERIT, 0);

    if (!::CreatePipe(&hErrRd, &hErrWr, &sa, 65536u)) {
        ::CloseHandle(hInRd);
        ::CloseHandle(hInWr);
        ::CloseHandle(hOutRd);
        ::CloseHandle(hOutWr);
        return -1;
    }
    ::SetHandleInformation(hErrRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hInRd;
    si.hStdOutput = hOutWr;
    si.hStdError = hErrWr;

    PROCESS_INFORMATION pi{};
    ::SetEnvironmentVariableW(L"HYD_MANAGED", L"1");

    const BOOL created = ::CreateProcessW(
        exe_path,
        cmdline.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi);

    ::CloseHandle(hInRd);
    ::CloseHandle(hOutWr);
    ::CloseHandle(hErrWr);

    if (!created) {
        ::CloseHandle(hInWr);
        ::CloseHandle(hOutRd);
        ::CloseHandle(hErrRd);
        return -1;
    }

    struct PipeDrainCtx {
        HANDLE hPipe;
        std::string* dst;
        size_t limit;
        bool overflowed;
    };

    auto drainProc = [](LPVOID param) -> DWORD {
        auto* ctx = static_cast<PipeDrainCtx*>(param);
        alignas(64) char buf[262144];
        for (;;) {
            DWORD nread = 0;
            if (!::ReadFile(ctx->hPipe, buf, sizeof(buf), &nread, nullptr) || nread == 0u) {
                break;
            }
            if (ctx->dst->size() + static_cast<size_t>(nread) > ctx->limit) {
                ctx->overflowed = true;
                break;
            }
            ctx->dst->append(buf, static_cast<size_t>(nread));
        }
        return 0;
    };

    const size_t safe_out_limit = (max_bytes > (static_cast<size_t>(-1) - 64u))
        ? static_cast<size_t>(-1)
        : (max_bytes + 64u);

    PipeDrainCtx outCtx{ hOutRd, &stdout_out, safe_out_limit, false };
    PipeDrainCtx errCtx{ hErrRd, &stderr_out, 65536u, false };

    HANDLE hOutThread = ::CreateThread(nullptr, 0, drainProc, &outCtx, 0, nullptr);
    HANDLE hErrThread = ::CreateThread(nullptr, 0, drainProc, &errCtx, 0, nullptr);

    if (!stdin_payload.empty()) {
        (void)io_win32_detail::writeAllHandleW(hInWr, stdin_payload);
    }
    ::CloseHandle(hInWr);

    if (hOutThread) {
        ::WaitForSingleObject(hOutThread, INFINITE);
        ::CloseHandle(hOutThread);
    } else {
        drainProc(&outCtx);
    }

    if (hErrThread) {
        ::WaitForSingleObject(hErrThread, INFINITE);
        ::CloseHandle(hErrThread);
    } else {
        drainProc(&errCtx);
    }

    ::CloseHandle(hOutRd);
    ::CloseHandle(hErrRd);

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    ::GetExitCodeProcess(pi.hProcess, &exit_code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    // Enforce all-or-nothing guarantee: discard partial stdout on non-zero child exit
    if (exit_code != 0u || outCtx.overflowed) {
        stdout_out.clear();
        return 1;
    }
    return 0;
#else
    (void)argc;
    (void)argv;
    (void)stdin_payload;
    (void)max_bytes;
    (void)stdout_out;
    (void)stderr_out;
    return -1;
#endif
}

/** @} */ // end of io