#pragma once

// MSAA (Multi-Sample Anti-Aliasing) settings for the deferred G-Buffer pass.
// 
// When enabled, the G-Buffer render targets (albedo, normal, material) and depth
// buffer are created with multiple samples per pixel. After the geometry pass,
// the MSAA buffers are resolved to single-sample textures before the deferred
// lighting pass runs.
//
// This provides high-quality geometric edge anti-aliasing at the cost of increased
// memory bandwidth and VRAM usage (2x, 4x, or 8x the base G-Buffer size).

struct MsaaSettings
{
    bool  Enabled = false;
    UINT  SampleCount = 4;  // Valid values: 1 (disabled), 2, 4, 8
    UINT  Quality = 0;      // 0 = default quality level for the sample count

    // Validate and clamp sample count to supported values
    void Validate()
    {
        // Clamp to valid MSAA sample counts
        if (SampleCount < 2)
            SampleCount = 2;
        else if (SampleCount > 8)
            SampleCount = 8;
        else if (SampleCount != 2 && SampleCount != 4 && SampleCount != 8)
            SampleCount = 4; // Default to 4x if invalid value
    }

    // Get memory multiplier for G-Buffer (e.g., 4x MSAA = 4.0f)
    float GetMemoryMultiplier() const
    {
        return Enabled ? static_cast<float>(SampleCount) : 1.0f;
    }

    UINT GetEffectiveSampleCount() const
    {
        return Enabled ? SampleCount : 1;
    }

    UINT GetEffectiveQuality() const
    {
        return Enabled ? Quality : 0;
    }
};

