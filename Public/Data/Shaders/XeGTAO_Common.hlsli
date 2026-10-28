// XeGTAO_Common.hlsli
// Shared constant buffer and utilities for all XeGTAO compute passes.

#ifndef XEGTAO_COMMON_HLSLI
#define XEGTAO_COMMON_HLSLI

// Disable min-precision half-float path; use full float for cs_6_0 compatibility.
#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0
// Use 32-bit depth throughout (our depth buffer is R32_FLOAT).
#define XE_GTAO_FP32_DEPTHS 1

#include "XeGTAO.h"

cbuffer GTAOConstantsCB : register(b0)
{
    GTAOConstants g_GTAOConsts;
}

// World-space oct-encoded normal decode (matches GBuffer.hlsl encoding)
float3 DecodeOctNormalWS(float2 oct)
{
    oct = oct * 2.0f - 1.0f;
    float3 n = float3(oct.x, oct.y, 1.0f - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0f)
    {
        float2 wrap = (1.0f - abs(n.yx)) * (float2(n.x >= 0.0f, n.y >= 0.0f) * 2.0f - 1.0f);
        n.xy = wrap;
    }
    return normalize(n);
}

#endif // XEGTAO_COMMON_HLSLI
