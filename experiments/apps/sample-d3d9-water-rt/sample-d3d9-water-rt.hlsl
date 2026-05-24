float g_WaveScale;
float4 g_WaterTint;

texture g_Texture0;

sampler2D g_Sampler0 = sampler_state {
    Texture = <g_Texture0>;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    MipFilter = LINEAR;
    AddressU = CLAMP;
    AddressV = CLAMP;
};

struct VSIn {
    float4 Position : POSITION0;
    float4 Color : COLOR0;
    float4 TexCoord : TEXCOORD0;
};

struct VSOut {
    float4 Position : POSITION0;
    float4 Color : COLOR0;
    float4 TexCoord : TEXCOORD0;
};

float saturate_ramp(float a, float b, float x) {
    return saturate((x - a) / (b - a));
}

VSOut WaterRTVS(VSIn input) {
    VSOut output;
    output.Position = input.Position;
    output.Color = input.Color;
    output.TexCoord = input.TexCoord;
    return output;
}

float4 ScenePS(VSOut input) : COLOR0 {
    float2 uv = input.TexCoord.xy;
    float4 base = tex2D(g_Sampler0, uv);
    float horizon = saturate_ramp(0.30, 0.90, 1.0 - uv.y);
    base.rgb += float3(0.05, 0.05, 0.06) * horizon;
    base.rgb += (0.03 + uv.x * 0.04) * horizon;
    return base * input.Color;
}

float4 CopyPS(VSOut input) : COLOR0 {
    return tex2D(g_Sampler0, input.TexCoord.xy) * input.Color;
}

float4 WaterPS(VSOut input) : COLOR0 {
    float2 uv = input.TexCoord.xy;
    float2 offset = float2(
        (uv.x - 0.5) * 0.032,
        (0.48 - uv.y) * 0.018) * g_WaveScale;

    float2 sceneUv = input.TexCoord.zw;
    float4 proj = float4(sceneUv, 0.0, 1.0);
    proj.xy += offset * proj.w;
    float4 refracted = tex2Dproj(g_Sampler0, proj);

    float foamMask = saturate_ramp(0.10, 0.38, 1.0 - uv.y);
    foamMask *= saturate_ramp(0.0, 0.15, uv.x) + saturate_ramp(1.0, 0.85, uv.x);
    float fresnel = saturate(0.24 + (1.0 - uv.y) * 0.60);

    float3 color = lerp(refracted.rgb, g_WaterTint.rgb, 0.38);
    color += foamMask * 0.12;
    color += fresnel * 0.06;

    float alpha = saturate(g_WaterTint.a + foamMask * 0.18);
    return float4(color, alpha) * input.Color;
}

float4 WaterPSNoProj(VSOut input) : COLOR0 {
    float2 uv = input.TexCoord.xy;
    float2 offset = float2(
        (uv.x - 0.5) * 0.032,
        (0.48 - uv.y) * 0.018) * g_WaveScale;

    float2 sceneUv = input.TexCoord.zw + offset;
    float4 refracted = tex2D(g_Sampler0, sceneUv);

    float foamMask = saturate_ramp(0.10, 0.38, 1.0 - uv.y);
    foamMask *= saturate_ramp(0.0, 0.15, uv.x) + saturate_ramp(1.0, 0.85, uv.x);
    float fresnel = saturate(0.24 + (1.0 - uv.y) * 0.60);

    float3 color = lerp(refracted.rgb, g_WaterTint.rgb, 0.38);
    color += foamMask * 0.12;
    color += fresnel * 0.06;

    float alpha = saturate(g_WaterTint.a + foamMask * 0.18);
    return float4(color, alpha) * input.Color;
}

float4 WaterPSSolid(VSOut input) : COLOR0 {
    return float4(1.0, 0.15, 0.1, 1.0) * input.Color;
}
