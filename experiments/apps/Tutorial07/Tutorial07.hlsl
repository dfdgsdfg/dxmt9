float4x4 g_mWorldViewProjection;
float4 g_TintA;
float4 g_TintB;
float g_fTime;

sampler2D g_TextureSampler : register(s0);

struct VS_INPUT
{
    float4 Position : POSITION0;
};

struct VS_OUTPUT
{
    float4 Position : POSITION0;
    float4 Color : COLOR0;
    float2 TexCoord : TEXCOORD0;
};

VS_OUTPUT Tutorial07VS(VS_INPUT input)
{
    VS_OUTPUT output;
    float2 uv = input.Position.xy * float2(0.5f, -0.5f) + 0.5f;
    float band = frac(g_fTime * 0.045f + uv.y * 0.55f - uv.x * 0.20f);
    float lift = saturate(1.0f - dot(uv - 0.5f.xx, uv - 0.5f.xx) * 1.85f);

    output.Position = input.Position;
    output.TexCoord = uv + float2((band - 0.5f) * 0.08f, lift * 0.03f);
    output.Color = g_TintA * (1.0f - band) + g_TintB * band;
    output.Color.rgb = saturate(output.Color.rgb + lift * 0.22f);
    output.Color.a = 1.0f;
    return output;
}

float4 Tutorial07PS(VS_OUTPUT input) : COLOR0
{
    float4 texel = tex2D(g_TextureSampler, input.TexCoord * 1.2f);
    float2 grid = abs(frac(input.TexCoord * 12.0f) - 0.5f.xx);
    float lines = saturate(1.0f - min(grid.x, grid.y) * 16.0f);
    float pulse = frac(g_fTime * 0.10f + input.TexCoord.x * 0.40f + input.TexCoord.y * 0.25f);
    float3 ramp = g_TintA.rgb * (1.0f - pulse) + g_TintB.rgb * pulse;
    float3 lit = texel.rgb * 0.50f + input.Color.rgb * 0.35f + ramp * 0.40f;
    lit = lit + lines * float3(0.20f, 0.16f, 0.08f);
    return float4(saturate(lit), 1.0f);
}
