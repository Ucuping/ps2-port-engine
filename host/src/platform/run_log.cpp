/*
 * The run log (platform.h): a run's stdout and stderr copied into a file as
 * they are written, wherever the streams went before - a terminal, a pipe, a
 * file. A player's window gives no terminal to read, so the file is the one
 * account of why a run ended. Windows' ports write their log in the
 * executable's own start-up instead (main.cpp, a run without arguments).
 */

#include "platform.h"

#if !defined(_WIN32)

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <thread>
#include <unistd.h>

namespace ps2host::platform {

namespace {

int g_file = -1;                    // the log
int g_saved[2] = {-1, -1};          // stdout's and stderr's descriptors before
std::thread g_copiers[2];

// One stream: what comes through the pipe, to where the stream went before
// and to the log, until the pipe's writing end is closed.
void Copy(int from, int to) {
    char buffer[16384];
    for (;;) {
        const ssize_t n = ::read(from, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (ssize_t done = 0; to >= 0 && done < n;) {
            const ssize_t w = ::write(to, buffer + done, std::size_t(n - done));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) to = -1;                // gone (a closed pipe): the log goes on alone
            else done += w;
        }
        for (ssize_t done = 0; done < n;) {
            const ssize_t w = ::write(g_file, buffer + done, std::size_t(n - done));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) break;
            done += w;
        }
    }
    ::close(from);
}

}  // namespace

int StartRunLog(const std::string& path) {
    if (g_file >= 0) return 1;
    const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (file < 0) return -1;
    // Another run of the same port holds it: that run's log is left alone.
    if (::flock(file, LOCK_EX | LOCK_NB) != 0) {
        ::close(file);
        return 0;
    }
    if (::ftruncate(file, 0) != 0) {
        ::close(file);
        return -1;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    for (int stream = 0; stream < 2; ++stream) {
        int ends[2];
        if (::pipe(ends) != 0) {
            for (int done = 0; done < stream; ++done) {         // the streams already moved, back
                ::dup2(g_saved[done], done + 1);
                g_copiers[done].join();
            }
            ::close(file);
            return -1;
        }
        ::fcntl(ends[0], F_SETFD, FD_CLOEXEC);
        g_saved[stream] = ::dup(stream + 1);
        ::fcntl(g_saved[stream], F_SETFD, FD_CLOEXEC);
        ::dup2(ends[1], stream + 1);
        ::close(ends[1]);
        g_file = file;
        g_copiers[stream] = std::thread(Copy, ends[0], g_saved[stream]);
    }
    // A pipe would have stdout buffered whole: written as it comes, so that a
    // run ended by force loses nothing it printed.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    SetCrashReportFile(file, g_saved[1]);
    return 1;
}

void FinishRunLog() {
    if (g_file < 0) return;
    std::fflush(stdout);
    std::fflush(stderr);
    // The pipes' writing ends closed (the streams back where they were): the
    // copiers read to the end and stop.
    for (int stream = 0; stream < 2; ++stream) ::dup2(g_saved[stream], stream + 1);
    for (int stream = 0; stream < 2; ++stream)
        if (g_copiers[stream].joinable()) g_copiers[stream].join();
    SetCrashReportFile(-1, STDERR_FILENO);
    ::close(g_file);
    g_file = -1;
}

}  // namespace ps2host::platform

#endif
