#include "ProcessPipe.h"

#include <chrono>
#include <thread>

#if defined(_WIN32)
namespace
{
std::wstring toWide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// Quotes one argument for the Windows command line.
std::wstring quoteArg(const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    for (wchar_t c : arg)
    {
        if (c == L'"')
            out += L"\\\"";
        else
            out += c;
    }
    out += L"\"";
    return out;
}
} // namespace

ProcessPipe::~ProcessPipe()
{
    kill();
}

bool ProcessPipe::start(const std::string& exe, const std::vector<std::string>& args)
{
    kill();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    HANDLE childIn = nullptr, childOutWrite = nullptr;
    HANDLE parentIn = nullptr, parentOut = nullptr;
    if (!CreatePipe(&childIn, &parentIn, &sa, 0) || !CreatePipe(&parentOut, &childOutWrite, &sa, 0))
        return false;
    // Our ends must not be inherited by the child.
    SetHandleInformation(parentIn, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentOut, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = childIn;
    si.hStdOutput = childOutWrite;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    std::wstring cmd = quoteArg(toWide(exe));
    for (const auto& a : args)
        cmd += L" " + quoteArg(toWide(a));

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);

    CloseHandle(childIn);
    CloseHandle(childOutWrite);
    if (!ok)
    {
        CloseHandle(parentIn);
        CloseHandle(parentOut);
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    toChild_ = parentIn;
    fromChild_ = parentOut;
    running_ = true;
    return true;
}

bool ProcessPipe::writeAll(const std::uint8_t* data, std::size_t size)
{
    if (!running_)
        return false;
    std::size_t done = 0;
    while (done < size)
    {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(size - done > 65536 ? 65536 : size - done);
        if (!WriteFile(toChild_, data + done, chunk, &written, nullptr) || written == 0)
            return false;
        done += written;
    }
    return true;
}

long ProcessPipe::readWithTimeout(std::uint8_t* buffer, std::size_t size, int timeoutMs)
{
    if (!running_)
        return -1;
    // Windows pipes have no poll, so check for data and sleep briefly.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(fromChild_, nullptr, 0, nullptr, &available, nullptr))
            return -1; // broken pipe: the child has gone
        if (available > 0)
        {
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>(size < available ? size : available);
            if (!ReadFile(fromChild_, buffer, want, &got, nullptr) || got == 0)
                return -1;
            return static_cast<long>(got);
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void ProcessPipe::killChildForTesting()
{
    if (!running_)
        return;
    TerminateProcess(process_, 1);
    WaitForSingleObject(process_, 5000);
}

void ProcessPipe::kill()
{
    if (!running_)
        return;
    if (toChild_ != nullptr)
        CloseHandle(toChild_);
    if (fromChild_ != nullptr)
        CloseHandle(fromChild_);
    if (process_ != nullptr)
    {
        TerminateProcess(process_, 1);
        WaitForSingleObject(process_, 5000);
        CloseHandle(process_);
    }
    toChild_ = fromChild_ = process_ = nullptr;
    running_ = false;
}

#else // POSIX

#include <csignal>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

ProcessPipe::~ProcessPipe()
{
    kill();
}

bool ProcessPipe::start(const std::string& exe, const std::vector<std::string>& args)
{
    kill();

    // Build argv before fork, so the child only makes async-signal-safe calls.
    std::vector<std::string> argStore;
    argStore.push_back(exe);
    argStore.insert(argStore.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& s : argStore)
        argv.push_back(&s[0]);
    argv.push_back(nullptr);

    int in[2] = {-1, -1};
    int out[2] = {-1, -1};
    if (pipe(in) != 0)
        return false;
    if (pipe(out) != 0)
    {
        close(in[0]);
        close(in[1]);
        return false;
    }

    // A write to a child that has died must return an error, not kill us.
    std::signal(SIGPIPE, SIG_IGN);

    const pid_t pid = fork();
    if (pid < 0)
    {
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        return false;
    }
    if (pid == 0)
    {
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        execv(exe.c_str(), argv.data());
        _exit(127);
    }

    close(in[0]);
    close(out[1]);
    toChild_ = in[1];
    fromChild_ = out[0];
    pid_ = pid;
    running_ = true;
    return true;
}

bool ProcessPipe::writeAll(const std::uint8_t* data, std::size_t size)
{
    if (!running_)
        return false;
    std::size_t done = 0;
    while (done < size)
    {
        const ssize_t n = write(toChild_, data + done, size - done);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

long ProcessPipe::readWithTimeout(std::uint8_t* buffer, std::size_t size, int timeoutMs)
{
    if (!running_)
        return -1;
    pollfd pfd{};
    pfd.fd = fromChild_;
    pfd.events = POLLIN;
    int r = 0;
    do
    {
        r = poll(&pfd, 1, timeoutMs);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        return -1;
    if (r == 0)
        return 0;

    ssize_t n = 0;
    do
    {
        n = read(fromChild_, buffer, size);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return -1;
    if (n == 0)
        return -1; // end of file: the child closed its output
    return static_cast<long>(n);
}

void ProcessPipe::killChildForTesting()
{
    if (!running_)
        return;
    ::kill(pid_, SIGKILL);
    int status = 0;
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR)
    {
    }
    // Reaped, but the pipe ends stay open. The next read sees end of file.
}

void ProcessPipe::kill()
{
    if (!running_)
        return;
    close(toChild_);
    close(fromChild_);
    ::kill(pid_, SIGKILL);
    int status = 0;
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR)
    {
    }
    toChild_ = fromChild_ = -1;
    pid_ = -1;
    running_ = false;
}

#endif
