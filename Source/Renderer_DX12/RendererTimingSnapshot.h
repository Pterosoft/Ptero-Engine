#pragma once

#include <string>
#include <vector>

struct RendererTimingEntry
{
    std::string Category;
    std::string Name;
    float CpuMilliseconds = 0.0f;
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
    std::vector<RendererTimingEntry> Entries;
};
