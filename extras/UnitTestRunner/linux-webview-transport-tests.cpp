#include "../../modules/juce_gui_extra/native/juce_WebBrowserComponent_linux_helpers.h"

#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace juce::LinuxWebViewHelpers;

static volatile sig_atomic_t signalCount = 0;
static void countSignal (int) { signalCount = signalCount + 1; }

static void testBrokenPipe()
{
    int descriptors[2];
    assert (pipe (descriptors) == 0);
    closeDescriptor (descriptors[0]);
    const char byte = 'x';
    assert (! writeAll (descriptors[1], &byte, 1));
    assert (errno == EPIPE);

    struct sigaction action {}, previous {};
    action.sa_handler = countSignal;
    sigemptyset (&action.sa_mask);
    assert (sigaction (SIGPIPE, &action, &previous) == 0);
    assert (! writeAll (descriptors[1], &byte, 1));
    assert (errno == EPIPE && signalCount == 0);
    struct sigaction afterwards {};
    assert (sigaction (SIGPIPE, nullptr, &afterwards) == 0);
    assert (afterwards.sa_handler == countSignal);
    assert (sigaction (SIGPIPE, &previous, nullptr) == 0);

    sigset_t blocked, oldMask, pending;
    sigemptyset (&blocked);
    sigaddset (&blocked, SIGPIPE);
    assert (pthread_sigmask (SIG_BLOCK, &blocked, &oldMask) == 0);
    assert (raise (SIGPIPE) == 0);
    assert (! writeAll (descriptors[1], &byte, 1));
    assert (errno == EPIPE);
    assert (sigpending (&pending) == 0 && sigismember (&pending, SIGPIPE) == 1);
    const timespec noWait {};
    assert (sigtimedwait (&blocked, nullptr, &noWait) == SIGPIPE);
    assert (pthread_sigmask (SIG_SETMASK, &oldMask, nullptr) == 0);
    closeDescriptor (descriptors[1]);
    assert (descriptors[0] == -1 && descriptors[1] == -1);
}

static void testPartialWrites()
{
    int descriptors[2];
    assert (pipe (descriptors) == 0);
    std::vector<char> expected (2 * 1024 * 1024, 'a'), actual (expected.size());
    struct sigaction action {}, previous {};
    action.sa_handler = countSignal;
    sigemptyset (&action.sa_mask);
    assert (sigaction (SIGUSR1, &action, &previous) == 0);
    const auto writer = pthread_self();
    std::thread reader ([&]
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
        pthread_kill (writer, SIGUSR1); // Interrupt a blocked write without SA_RESTART.
        size_t received = 0;
        while (received < actual.size())
        {
            const auto size = read (descriptors[0], actual.data() + received, actual.size() - received);
            assert (size > 0);
            received += static_cast<size_t> (size);
        }
    });
    assert (writeAll (descriptors[1], expected.data(), expected.size()));
    reader.join();
    assert (actual == expected);
    assert (sigaction (SIGUSR1, &previous, nullptr) == 0);
    closeDescriptor (descriptors[0]);
    closeDescriptor (descriptors[1]);

    assert (pipe (descriptors) == 0);
    std::thread disappearingReader ([&]
    {
        std::array<char, 4096> bytes {};
        assert (read (descriptors[0], bytes.data(), bytes.size()) > 0);
        close (descriptors[0]);
    });
    assert (! writeAll (descriptors[1], expected.data(), expected.size()));
    assert (errno == EPIPE);
    disappearingReader.join();
    descriptors[0] = -1;
    closeDescriptor (descriptors[1]);
}

static void testChildTermination (bool ignoreTerm)
{
    int ready[2];
    assert (pipe (ready) == 0);
    const auto child = fork();
    assert (child >= 0);
    if (child == 0)
    {
        close (ready[0]);
        signal (SIGTERM, ignoreTerm ? SIG_IGN : SIG_DFL);
        const char value = 1;
        writeAll (ready[1], &value, 1);
        close (ready[1]);
        for (;;) pause();
    }
    closeDescriptor (ready[1]);
    char value;
    assert (read (ready[0], &value, 1) == 1);
    closeDescriptor (ready[0]);
    const auto start = std::chrono::steady_clock::now();
    assert (terminateChild (child));
    assert (std::chrono::steady_clock::now() - start < std::chrono::seconds (2));
    assert (terminateChild (child)); // ECHILD must never initiate another signal loop.
}

static void testRequestOwnership()
{
    struct Resource { int references = 1; } first, second;
    const auto retain = +[] (Resource* value) { ++value->references; };
    const auto release = +[] (Resource* value) { --value->references; };
    PendingRequests<Resource> requests { retain, release };
    const auto firstId = requests.insert (&first);
    const auto secondId = requests.insert (&second);
    release (&first);
    release (&second);
    assert (first.references == 1 && second.references == 1);
    {
        auto response = requests.remove (firstId);
        assert (response.get() == &first && first.references == 1);
        assert (requests.remove (firstId) == nullptr);
        assert (requests.remove (999) == nullptr);
    }
    assert (first.references == 0);
    requests.clear(); // Teardown happens before unloading GObject symbols.
    assert (second.references == 0);
    assert (requests.remove (secondId) == nullptr);
}

int main()
{
    signal (SIGPIPE, SIG_DFL);
    testBrokenPipe();
    testPartialWrites();
    testRequestOwnership();
    testChildTermination (false);
    testChildTermination (true);
    const auto normal = fork();
    assert (normal >= 0);
    if (normal == 0) _exit (0);
    assert (terminateChild (normal));
    std::cout << "Linux WebView: SIGPIPE isolation, interrupted/partial writes, request ownership, descriptor closure, and bounded child shutdown passed\n";
}
