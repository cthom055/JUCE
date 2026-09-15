/* Linux WebView pipe/process helpers. Kept independent of JUCE for fault tests. */
#pragma once

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <pthread.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace juce::LinuxWebViewHelpers
{
// A plugin must not change the host's process-wide SIGPIPE disposition. Preserve
// both its calling thread's mask and any SIGPIPE that was already pending.
inline bool writeAll (int fd, const void* data, size_t size)
{
    sigset_t pipeSignal, previousMask, pending;
    sigemptyset (&pipeSignal);
    sigaddset (&pipeSignal, SIGPIPE);
    const auto maskError = pthread_sigmask (SIG_BLOCK, &pipeSignal, &previousMask);

    if (maskError != 0)
    {
        errno = maskError;
        return false;
    }

    if (sigpending (&pending) != 0)
    {
        const auto error = errno;
        pthread_sigmask (SIG_SETMASK, &previousMask, nullptr);
        errno = error;
        return false;
    }
    const auto alreadyPending = sigismember (&pending, SIGPIPE) == 1;
    const auto* bytes = static_cast<const char*> (data);
    size_t written = 0;
    int error = 0;

    while (written < size)
    {
        const auto result = write (fd, bytes + written, size - written);

        if (result > 0)
        {
            written += static_cast<size_t> (result);
            continue;
        }

        if (result < 0 && errno == EINTR)
            continue;

        error = result < 0 ? errno : EIO;
        break;
    }

    if (error == EPIPE && ! alreadyPending)
    {
        const timespec noWait {};
        while (sigtimedwait (&pipeSignal, nullptr, &noWait) < 0 && errno == EINTR) {}
    }

    pthread_sigmask (SIG_SETMASK, &previousMask, nullptr);

    if (error != 0)
        errno = error;

    return written == size;
}

inline void closeDescriptor (int& fd)
{
    if (fd >= 0)
    {
        // On Linux close() has released the descriptor even when interrupted.
        close (fd);
        fd = -1;
    }
}

inline bool childHasExited (pid_t child)
{
    int status = 0;
    pid_t result;

    do { result = waitpid (child, &status, WNOHANG); }
    while (result < 0 && errno == EINTR);

    return result == child || (result < 0 && errno == ECHILD);
}

inline bool waitForChild (pid_t child, int milliseconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (milliseconds);

    do
    {
        if (childHasExited (child))
            return true;

        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    while (std::chrono::steady_clock::now() < deadline);

    return childHasExited (child);
}

// Never signal a child after waitpid has reaped it (including a signalled exit).
inline bool terminateChild (pid_t child)
{
    if (child <= 0 || waitForChild (child, 150))
        return true;

    kill (child, SIGTERM);

    if (waitForChild (child, 150))
        return true;

    kill (child, SIGKILL);
    return waitForChild (child, 150);
}

template <typename Object>
class PendingRequests
{
public:
    using RefFunction = void (*) (Object*);
    using Request = std::unique_ptr<Object, RefFunction>;

    PendingRequests (RefFunction retainIn, RefFunction releaseIn)
        : retain (retainIn), release (releaseIn) {}

    int64_t insert (Object* request)
    {
        while (requests.count (nextId) != 0)
            advanceId();

        const auto id = nextId;
        advanceId();
        retain (request);
        requests.emplace (id, Request { request, release });
        return id;
    }

    Request remove (int64_t id)
    {
        const auto it = requests.find (id);
        if (it == requests.end())
            return Request { nullptr, release };

        auto request = std::move (it->second);
        requests.erase (it);
        return request;
    }

    void clear() { requests.clear(); }

private:
    void advanceId()
    {
        nextId = nextId == std::numeric_limits<int64_t>::max() ? 0 : nextId + 1;
    }

    RefFunction retain, release;
    std::map<int64_t, Request> requests;
    int64_t nextId = 0;
};
}
