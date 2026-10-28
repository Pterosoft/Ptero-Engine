cbuffer MotionVectorConstants : register(b0)
{
    float4x4 gCurrentMVP;
    float4x4 gPreviousMVP;
};

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD;
    float4 Color    : COLOR;
};

struct VSOutput
{
    float4 Position        : SV_Position;
    float4 CurrentClipPos  : TEXCOORD0;
    float4 PreviousClipPos : TEXCOORD1;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.CurrentClipPos = mul(float4(input.Position, 1.0f), gCurrentMVP);
    output.PreviousClipPos = mul(float4(input.Position, 1.0f), gPreviousMVP);
    output.Position = output.CurrentClipPos;
    return output;
}

float2 ClipToUv(float4 clipPos)
{
    const float invW = rcp(max(abs(clipPos.w), 1e-6f));
    float2 uv = clipPos.xy * invW;
    uv.y = -uv.y;
    return uv * 0.5f + 0.5f;
}

float4 PSMain(VSOutput input) : SV_Target
{
    const float2 currentUv = ClipToUv(input.CurrentClipPos);
    const float2 previousUv = ClipToUv(input.PreviousClipPos);
    const float2 motion = currentUv - previousUv;
    return float4(motion, 0.0f, 0.0f);
}