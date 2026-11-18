#pragma once

#include <atomic>
#include <cstdint>

namespace ck {

/** Live performance numbers, written by the audio thread and read by the UI.
    Load is render time divided by the block's real-time duration: 1.0 means the
    block took as long to render as it takes to play, which is a dropout. */
class RenderStats
{
public:
    struct Snapshot
    {
        float lastLoad = 0.0f;
        float peakLoad = 0.0f;
        uint32_t overloads = 0;  ///< Blocks that took longer to render than to play.
        int activeVoices = 0;
        int peakVoices = 0;
        uint32_t steals = 0;         ///< Notes cut short because every voice was busy.
        uint32_t limitedBlocks = 0;  ///< Blocks where the output limiter had to act.
    };

    Snapshot snapshot() const noexcept
    {
        return { lastLoad.load (std::memory_order_relaxed),
                 peakLoad.load (std::memory_order_relaxed),
                 overloads.load (std::memory_order_relaxed),
                 activeVoices.load (std::memory_order_relaxed),
                 peakVoices.load (std::memory_order_relaxed),
                 steals.load (std::memory_order_relaxed),
                 limitedBlocks.load (std::memory_order_relaxed) };
    }

    /** Called from the UI thread; the audio thread may race it harmlessly. */
    void resetPeaks() noexcept
    {
        peakLoad.store (0.0f, std::memory_order_relaxed);
        peakVoices.store (0, std::memory_order_relaxed);
        overloads.store (0, std::memory_order_relaxed);
        steals.store (0, std::memory_order_relaxed);
        limitedBlocks.store (0, std::memory_order_relaxed);
    }

    void recordSteal() noexcept    { steals.fetch_add (1, std::memory_order_relaxed); }
    void recordLimiting() noexcept { limitedBlocks.fetch_add (1, std::memory_order_relaxed); }

    // Audio thread only.
    void recordBlock (float load, int voices) noexcept
    {
        lastLoad.store (load, std::memory_order_relaxed);
        if (load > peakLoad.load (std::memory_order_relaxed))
            peakLoad.store (load, std::memory_order_relaxed);
        if (load >= 1.0f)
            overloads.fetch_add (1, std::memory_order_relaxed);

        activeVoices.store (voices, std::memory_order_relaxed);
        if (voices > peakVoices.load (std::memory_order_relaxed))
            peakVoices.store (voices, std::memory_order_relaxed);
    }

private:
    std::atomic<float> lastLoad { 0.0f };
    std::atomic<float> peakLoad { 0.0f };
    std::atomic<uint32_t> overloads { 0 };
    std::atomic<int> activeVoices { 0 };
    std::atomic<int> peakVoices { 0 };
    std::atomic<uint32_t> steals { 0 };
    std::atomic<uint32_t> limitedBlocks { 0 };

    static_assert (std::atomic<float>::is_always_lock_free);
    static_assert (std::atomic<int>::is_always_lock_free);
};

} // namespace ck
