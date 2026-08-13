// shaders_stereo3d.h - AFR-only stereo compositor for GBFR
// Both eyes are REAL engine renders held in two slots (t0 = LEFT, t1 = RIGHT).
#pragma once

static const char* kStereoHLSL = R"HLSL(
Texture2D    texL   : register(t0);   // LEFT slot
Texture2D    texR   : register(t1);   // RIGHT slot
SamplerState sPoint : register(s0);   // POINT, MIRROR address (seam mirror)

cbuffer Params : register(b0) {
    float gConv;       // per-eye convergence HIT (uv)
    float gOutW;       // output width
    float gOutH;       // output height
    float gMode;       // output mode 1..6
    float gSwapEyes;   // 1 -> swap L/R
    float gShiftL;     // stale-eye reprojection shift for LEFT
    float gShiftR;     // stale-eye reprojection shift for RIGHT
    float gForceMono;  // 1 -> both eyes read the FRESH slot
    float gFreshLeft;  // 1 -> fresh slot is LEFT (t0), else RIGHT (t1)
    float3 _pad;
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    int mode = (int)(gMode + 0.5);
    float2 uv = i.uv;
    float  px = i.pos.x, py = i.pos.y;

    if (mode == 0) return float4(texL.SampleLevel(sPoint, uv, 0).rgb, 1);

    float  eyeSign;   // -1 left, +1 right
    float2 srcUV;
    if (mode == 1 || mode == 6) {            // SBS / VR 2W x H
        bool left = uv.x < 0.5; eyeSign = left ? -1.0 : 1.0;
        srcUV = float2(frac(uv.x * 2.0), uv.y);
    } else if (mode == 2) {                  // TAB
        bool left = uv.y < 0.5; eyeSign = left ? -1.0 : 1.0;
        srcUV = float2(uv.x, frac(uv.y * 2.0));
    } else if (mode == 3) {                  // Line
        bool left = (fmod(floor(py), 2.0) < 0.5); eyeSign = left ? -1.0 : 1.0; srcUV = uv;
    } else if (mode == 4) {                  // Column
        bool left = (fmod(floor(px), 2.0) < 0.5); eyeSign = left ? -1.0 : 1.0; srcUV = uv;
    } else {                                 // Checker
        bool left = (fmod(floor(px) + floor(py), 2.0) < 0.5); eyeSign = left ? -1.0 : 1.0; srcUV = uv;
    }

    // fresh-in-both fallback: a stale/idle eye can never desync
    if (gForceMono > 0.5) {
        float3 c = (gFreshLeft > 0.5) ? texL.SampleLevel(sPoint, srcUV, 0).rgb
                                      : texR.SampleLevel(sPoint, srcUV, 0).rgb;
        return float4(c, 1);
    }

    if (gSwapEyes > 0.5) eyeSign = -eyeSign;
    float extra = (eyeSign < 0.0) ? gShiftL : gShiftR;         // stale-eye reprojection
    float2 s = float2(srcUV.x - eyeSign * gConv + extra, srcUV.y);
    float3 c = (eyeSign < 0.0) ? texL.SampleLevel(sPoint, s, 0).rgb
                               : texR.SampleLevel(sPoint, s, 0).rgb;
    return float4(c, 1);
}
)HLSL";
