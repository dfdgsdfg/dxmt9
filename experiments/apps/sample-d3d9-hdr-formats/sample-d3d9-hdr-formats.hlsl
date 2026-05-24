float4x4 g_mWorldViewProjection;
float4 g_HDRColorA;
float4 g_HDRColorB;
float4 g_ToneBias;
float g_fTime;

sampler2D g_HDRSampler : register(s0);

struct VS_INPUT
{
    float4 Position : POSITION0;
};

struct VS_OUTPUT
{
    float4 Position : POSITION0;
    float2 TexCoord : TEXCOORD0;
};

VS_OUTPUT HDRFormatsVS(VS_INPUT input)
{
    VS_OUTPUT output;
    float2 uv = input.Position.xy * float2(0.5f, -0.5f) + 0.5f;
    output.Position = input.Position;
    output.TexCoord = uv;
    return output;
}

float4 HDRScenePS(VS_OUTPUT input) : COLOR0
{
    float2 centered = input.TexCoord - 0.5f.xx;
    float radial = saturate(1.25f - dot(centered, centered) * 3.1f);
    float sweep = frac(g_fTime * 0.04f + input.TexCoord.x * 0.35f + input.TexCoord.y * 0.22f);
    float3 hdr = g_HDRColorA.rgb * radial + g_HDRColorB.rgb * sweep;
    hdr += float3(0.8f, 0.5f, 0.2f) * (1.0f - radial) * 1.6f;
    return float4(hdr, 1.0f);
}

float4 HDRToneMapPS(VS_OUTPUT input) : COLOR0
{
    float3 hdr = tex2D(g_HDRSampler, input.TexCoord).rgb;
    float3 mapped = hdr / (1.0f.xxx + hdr);
    mapped += g_ToneBias.rgb;
    return float4(saturate(mapped), 1.0f);
}
