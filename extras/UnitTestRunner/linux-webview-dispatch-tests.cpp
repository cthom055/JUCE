#include "../../modules/juce_gui_extra/native/juce_WebBrowserComponent_linux_dispatch.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>

using Queue = juce::LinuxWebViewHelpers::DispatchQueue<int>;
int main()
{
    Queue queue (3, 10);
    assert (queue.push (1, 4, 0) == Queue::Push::schedule);
    assert (queue.push (2, 4, 0) == Queue::Push::queued);
    assert (queue.push (99, 3, 0) == Queue::Push::rejected);
    assert (queue.pop (1) == 1);
    assert (queue.push (3, 2, 1) == Queue::Push::queued);
    assert (queue.push (4, 2, 1) == Queue::Push::queued);
    assert (queue.push (99, 0, 1) == Queue::Push::rejected);
    assert (queue.pop (2) == 2);
    assert (queue.pop (2) == 3);
    assert (queue.pop (2) == 4);
    assert (! queue.pop (2));
    assert (queue.push (5, 10, 3) == Queue::Push::schedule);
    queue.cancel();
    assert (! queue.pop (3));
    assert (queue.push (6, 1, 3) == Queue::Push::rejected);
    assert (queue.getStats().delivered == 4);
    assert (queue.getStats().peakBytes == 10);

    // Exercise the empty-queue/rearm race and FIFO ordering with real producer
    // and consumer threads. The consumer only wakes when push requests it.
    Queue concurrent (128, 512);
    std::atomic<bool> wake { false }, finished { false };
    std::thread producer ([&]
    {
        for (int i = 0; i < 100000;)
        {
            const auto result = concurrent.push (i, 4, 0);
            if (result == Queue::Push::rejected)
                std::this_thread::yield();
            else
            {
                ++i;
                if (result == Queue::Push::schedule)
                    wake.store (true);
            }
        }
        finished.store (true);
    });
    int expected = 0;
    while (expected != 100000)
    {
        if (! wake.exchange (false))
        {
            std::this_thread::yield();
            continue;
        }
        while (const auto value = concurrent.pop (1))
            assert (*value == expected++);
    }
    producer.join();
    assert (finished.load());
    assert (concurrent.getStats().peakCount <= 128);
    assert (concurrent.getStats().peakBytes <= 512);

    // Closing one instance cannot cancel the surviving instance's work.
    Queue survivor (2, 8);
    survivor.push (42, 4, 0);
    concurrent.cancel();
    assert (survivor.pop (1) == 42);
    std::cout << "Dispatch queue: FIFO, byte/count pressure, rearm race, cancellation, instance isolation passed\n";
}
