texture g_GrassTexture;
texture g_RockTexture;
texture g_BlendTexture;

sampler2D g_GrassSampler = sampler_state {
    Texture = <g_GrassTexture>;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    MipFilter = LINEAR;
    AddressU = WRAP;
    AddressV = WRAP;
};

sampler2D g_RockSampler = sampler_state {
    Texture = <g_RockTexture>;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    MipFilter = LINEAR;
    AddressU = WRAP;
    AddressV = WRAP;
};

sampler2D g_BlendSampler = sampler_state {
    Texture = <g_BlendTexture>;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    MipFilter = LINEAR;
    AddressU = CLAMP;
    AddressV = CLAMP;
};

struct VSIn {
    float4 Position : POSITION0;
    float4 Color : COLOR0;
    float2 Tex0 : TEXCOORD0;
};

struct VSOut {
    float4 Position : POSITION0;
    float4 Color : COLOR0;
    float2 Tex0 : TEXCOORD0;
};

VSOut MultiTextureTerrainVS(VSIn input) {
    VSOut output;
    output.Position = input.Position;
    output.Color = input.Color;
    output.Tex0 = input.Tex0;
    return output;
}

float4 MultiTextureTerrainPS(VSOut input) : COLOR0 {
    float2 grassUv = input.Tex0;
    float2 rockUv = input.Tex0 * float2(1.75, 1.35) + float2(0.12, 0.05);
    float2 blendUv = input.Tex0 * float2(0.25, 0.55);
    float4 grass = tex2D(g_GrassSampler, grassUv);
    float4 rock = tex2D(g_RockSampler, rockUv);
    float blend = tex2D(g_BlendSampler, blendUv).r;

    float4 terrain = lerp(grass, rock, blend);
    float fog = saturate((blendUv.y - 0.15) / 0.85);
    float3 fogColor = float3(0.72, 0.80, 0.88);
    terrain.rgb = lerp(terrain.rgb, fogColor, fog * 0.35);
    return terrain * input.Color;
}
