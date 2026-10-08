// A child process with its stdin and stdout connected to us (M4).
//
// JUCE's ChildProcess reads the child's output but cannot write to its input,
// so this class does both, with plain system calls. POSIX and Windows versions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
  #define NOMINMAX
  #include <windows.h>
#endif

class ProcessPipe
{
public:
    ProcessPipe() = default;
    ~ProcessPipe();

    ProcessPipe(const ProcessPipe&) = delete;
    ProcessPipe& operator=(const ProcessPipe&) = delete;

    // Starts `exe` with `args`. Any earlier child is stopped first.
    bool start(const std::string& exe, const std::vector<std::string>& args);

    // Writes every byte or returns false (the child has gone).
    bool writeAll(const std::uint8_t* data, std::size_t size);

    // Reads up to `size` bytes, waiting at most `timeoutMs`.
    // Returns the byte count (above 0), 0 on timeout, or -1 if the child has closed its output.
    long readWithTimeout(std::uint8_t* buffer, std::size_t size, int timeoutMs);

    // Stops the child at once (SIGKILL / TerminateProcess) and reaps it.
    void kill();

    // Kills the child but keeps our pipe ends open, as a real crash looks to us:
    // the next read or write fails. Used by the kill test.
    void killChildForTesting();

    bool running() const noexcept { return running_; }

private:
    bool running_ = false;

#if defined(_WIN32)
    HANDLE toChild_ = nullptr;
    HANDLE fromChild_ = nullptr;
    HANDLE process_ = nullptr;
#else
    int toChild_ = -1;
    int fromChild_ = -1;
    int pid_ = -1;
#endif
};
