float4x4 g_mWorldViewProjection;
float4 g_TintA;
float4 g_TintB;
float4 g_OverlayColor;
float g_fTime;
float g_PassMix;
float g_OverlayAlpha;

sampler2D g_TextureSampler : register(s0);

struct VS_INPUT
{
    float4 Position : POSITION0;
    float2 TexCoord : TEXCOORD0;
};

struct VS_OUTPUT
{
    float4 Position : POSITION0;
    float4 Color : COLOR0;
    float2 TexCoord : TEXCOORD0;
};

VS_OUTPUT SimpleSampleVS(VS_INPUT input)
{
    VS_OUTPUT output;
    float2 uv = input.TexCoord;
    float swing = frac(g_fTime * 0.05f + uv.x * 0.30f - uv.y * 0.18f);
    float focus = saturate(1.0f - dot(uv - 0.5f.xx, uv - 0.5f.xx) * 1.7f);

    output.Position = input.Position;
    output.TexCoord = uv + float2((swing - 0.5f) * 0.07f, focus * 0.02f);
    output.Color = g_TintA * (1.0f - swing) + g_TintB * swing;
    output.Color.rgb = saturate(output.Color.rgb + focus * 0.18f);
    output.Color.a = 1.0f;
    return output;
}

float4 SimpleSamplePS(VS_OUTPUT input) : COLOR0
{
    float4 texel = tex2D(g_TextureSampler, input.TexCoord * 1.15f);
    float2 grid = abs(frac(input.TexCoord * 14.0f) - 0.5f.xx);
    float weave = saturate(1.0f - min(grid.x, grid.y) * 18.0f);
    float2 centered = input.TexCoord - 0.5f.xx;
    float radial = saturate(1.0f - dot(centered, centered) * 4.5f);
    float blend = saturate(g_PassMix);
    float alpha = (1.0f - blend) + blend * ((texel.a / 255.0f) * g_OverlayAlpha * radial);
    float3 lit = texel.rgb * (0.52f - blend * 0.18f) + input.Color.rgb * (0.40f - blend * 0.12f);
    lit += weave * float3(0.16f, 0.10f, 0.06f);
    float3 color = lit + g_OverlayColor.rgb * (blend * (0.28f + radial * 0.32f));
    return float4(saturate(color), saturate(alpha));
}
