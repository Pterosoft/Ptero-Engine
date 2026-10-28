#pragma once

#include <string>
#include <vector>

struct RendererTimingEntry
{
    std::string Category;
    std::string Name;
    float CpuMilliseconds = 0.0f;
    // GPU execution time between the pass's timestamps, from a frame a few
    // frames old (timestamps are read back without stalling). Negative when
    // no GPU timing is available.
    float GpuMilliseconds = -1.0f;
};

// Per-pass CPU cost of recording one frame's command list.
//
// This measures how long the CPU spends *building* the frame, not how long the
// GPU spends executing it. That distinction is the whole point: when the frame
// time is high while GPU utilisation is low, the cost is here, and this is what
// localises it to a pass. Timers are non-overlapping top-level scopes, so the
// entries sum to roughly TotalCpuMilliseconds and each one's share is
// meaningful on its own.
struct RendererTimingSnapshot
{
    float TotalCpuMilliseconds = 0.0f;
    // GPU time from the start to the end of the scene renderer's work, which
    // includes anything recorded outside a timed pass. 0 when unavailable.
    float TotalGpuMilliseconds = 0.0f;
    std::vector<RendererTimingEntry> Entries;
};
