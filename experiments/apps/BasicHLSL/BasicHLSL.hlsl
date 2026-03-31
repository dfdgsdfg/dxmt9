float4x4 g_mWorld;
float4x4 g_mWorldViewProjection;
float4 g_MaterialAmbientColor;
float4 g_MaterialDiffuseColor;
float4 g_LightDir0;
float4 g_LightDiffuse0;
float4 g_LightAmbient;
float g_fTime;

sampler2D g_TextureSampler : register(s0);

struct VS_INPUT
{
    float4 Position : POSITION0;
};

struct VS_OUTPUT
{
    float4 Position : POSITION0;
    float4 Diffuse : COLOR0;
    float2 TexCoord : TEXCOORD0;
};

VS_OUTPUT BasicHLSLVS(VS_INPUT input)
{
    VS_OUTPUT output;
    float2 uv = input.Position.xy * float2(0.5f, -0.5f) + 0.5f;
    float2 centered = uv - 0.5f.xx;
    float radial = saturate(1.0f - dot(centered, centered) * 1.65f);
    float sweep = frac(g_fTime * 0.08f + uv.x * 0.35f + uv.y * 0.15f);

    output.Position = input.Position;
    output.TexCoord = uv + float2(sweep * 0.05f, 0.0f);
    output.Diffuse = g_MaterialAmbientColor * g_LightAmbient +
                     g_MaterialDiffuseColor * (0.35f + radial * 0.65f);
    output.Diffuse.rgb = saturate(output.Diffuse.rgb + float3(0.18f, 0.10f, 0.04f) * sweep + 0.18f.xxx);
    output.Diffuse.a = 1.0f;
    return output;
}

float4 BasicHLSLPS(VS_OUTPUT input) : COLOR0
{
    float4 texel = tex2D(g_TextureSampler, input.TexCoord);
    float2 grid = abs(frac(input.TexCoord * 10.0f) - 0.5f.xx);
    float wire = saturate(1.0f - min(grid.x, grid.y) * 14.0f);
    float3 gradient = float3(input.TexCoord.x, input.TexCoord.y, 1.0f - input.TexCoord.x * 0.7f);
    float3 lit = texel.rgb * 0.55f + gradient * 0.75f + input.Diffuse.rgb * 0.35f;
    lit = lerp(lit, lit + float3(0.18f, 0.15f, 0.10f), wire * 0.4f);
    return float4(saturate(lit), 1.0f);
}
