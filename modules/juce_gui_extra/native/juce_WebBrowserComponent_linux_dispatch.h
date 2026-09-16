#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace juce::LinuxWebViewHelpers
{
// One producer (pipe reader), one consumer (message thread). Never drop a
// command silently: exceeding either limit must fail the owning connection.
template <typename Command>
class DispatchQueue
{
public:
    enum class Push { rejected, queued, schedule };
    struct Entry { Command command; size_t bytes; double received; };
    struct Stats { size_t accepted = 0, delivered = 0, peakCount = 0, peakBytes = 0;
                   double totalWaitMs = 0, maxWaitMs = 0; };

    DispatchQueue (size_t countLimitIn, size_t byteLimitIn)
        : countLimit (countLimitIn), byteLimit (byteLimitIn) {}

    Push push (Command command, size_t bytes, double now)
    {
        const std::lock_guard<std::mutex> guard (mutex);
        if (closed || entries.size() >= countLimit || bytes > byteLimit - queuedBytes)
            return Push::rejected;
        entries.push_back ({ std::move (command), bytes, now });
        queuedBytes += bytes;
        ++stats.accepted;
        stats.peakCount = std::max (stats.peakCount, entries.size());
        stats.peakBytes = std::max (stats.peakBytes, queuedBytes);
        return std::exchange (scheduled, true) ? Push::queued : Push::schedule;
    }

    std::optional<Command> pop (double now)
    {
        const std::lock_guard<std::mutex> guard (mutex);
        if (entries.empty())
        {
            scheduled = false;
            return {};
        }
        auto entry = std::move (entries.front());
        entries.pop_front();
        queuedBytes -= entry.bytes;
        ++stats.delivered;
        const auto wait = std::max (0.0, now - entry.received);
        stats.totalWaitMs += wait;
        stats.maxWaitMs = std::max (stats.maxWaitMs, wait);
        return std::move (entry.command);
    }

    void cancel()
    {
        const std::lock_guard<std::mutex> guard (mutex);
        closed = true;
        entries.clear();
        queuedBytes = 0;
    }

    Stats getStats() const
    {
        const std::lock_guard<std::mutex> guard (mutex);
        return stats;
    }

private:
    mutable std::mutex mutex;
    std::deque<Entry> entries;
    const size_t countLimit, byteLimit;
    size_t queuedBytes = 0;
    bool scheduled = false, closed = false;
    Stats stats;
};
}
