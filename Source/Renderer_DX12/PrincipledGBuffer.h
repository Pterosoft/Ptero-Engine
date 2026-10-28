#pragma once

#include <d3d12.h>

// The G-Buffer targets that carry the Disney principled BRDF's extra parameters
// (PrincipledBrdf.hlsli): SV_Target4 and SV_Target5, after albedo, normal, material and
// the analytic edges. Every geometry-pass pipeline declares them, whether or not its
// shader has anything to put there - a writer with no principled parameters writes
// zero, which is the model with every extra lobe off.
//
// Plain UNORM rather than one packed integer target, so MSAA can resolve them the way
// it resolves the material target: an integer format cannot be resolved at all, and its
// single-sample copy would hold nothing but the clear value under MSAA.
namespace PrincipledGBuffer
{
    constexpr DXGI_FORMAT kFormat      = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr UINT        kFirstTarget = 4;
    constexpr UINT        kTargetCount = 2;

    // Render targets a geometry-pass pipeline declares in total.
    constexpr UINT kGeometryPassTargetCount = kFirstTarget + kTargetCount;

    inline void DeclareTargets(DXGI_FORMAT* rtvFormats)
    {
        for (UINT i = 0; i < kTargetCount; ++i)
            rtvFormats[kFirstTarget + i] = kFormat;
    }
}
