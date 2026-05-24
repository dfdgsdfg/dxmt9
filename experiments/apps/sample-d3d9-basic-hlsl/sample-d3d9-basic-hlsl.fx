//--------------------------------------------------------------------------------------
// File: sample-d3d9-basic-hlsl.fx
//
// Derived from the Microsoft DirectX SDK BasicHLSL sample effect.
//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License (MIT).
//--------------------------------------------------------------------------------------

float4 g_MaterialAmbientColor;
float4 g_MaterialDiffuseColor;

float3 g_LightDir[3];
float4 g_LightDiffuse[3];
float4 g_LightAmbient;

texture g_MeshTexture;

float g_fTime;
float4x4 g_mWorld;
float4x4 g_mWorldViewProjection;

sampler MeshTextureSampler =
sampler_state
{
    Texture = <g_MeshTexture>;
    MipFilter = LINEAR;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
};

struct VS_OUTPUT
{
    float4 Position  : POSITION;
    float4 Diffuse   : COLOR0;
    float2 TextureUV : TEXCOORD0;
};

VS_OUTPUT RenderSceneVS( float4 vPos : POSITION,
                         float3 vNormal : NORMAL,
                         float2 vTexCoord0 : TEXCOORD0,
                         uniform int nNumLights,
                         uniform bool bTexture,
                         uniform bool bAnimate )
{
    VS_OUTPUT Output;
    float3 vNormalWorldSpace;

    float4 vAnimatedPos = vPos;
    if( bAnimate )
        vAnimatedPos += float4(vNormal, 0) * (sin(g_fTime + 5.5) + 0.5) * 0.2;

    Output.Position = mul(vAnimatedPos, g_mWorldViewProjection);

    vNormalWorldSpace = normalize(mul(vNormal, (float3x3)g_mWorld));

    float3 vTotalLightDiffuse = float3(0, 0, 0);
    for( int i = 0; i < nNumLights; i++ )
        vTotalLightDiffuse += g_LightDiffuse[i] * max(0, dot(vNormalWorldSpace, g_LightDir[i]));

    Output.Diffuse.rgb = g_MaterialDiffuseColor * vTotalLightDiffuse +
                         g_MaterialAmbientColor * g_LightAmbient;
    Output.Diffuse.a = 1.0f;

    if( bTexture )
        Output.TextureUV = vTexCoord0;
    else
        Output.TextureUV = 0;

    return Output;
}

struct PS_OUTPUT
{
    float4 RGBColor : COLOR0;
};

PS_OUTPUT RenderScenePS( VS_OUTPUT In,
                         uniform bool bTexture )
{
    PS_OUTPUT Output;

    if( bTexture )
        Output.RGBColor = tex2D(MeshTextureSampler, In.TextureUV) * In.Diffuse;
    else
        Output.RGBColor = In.Diffuse;

    return Output;
}

technique RenderSceneWithTexture1Light
{
    pass P0
    {
        VertexShader = compile vs_2_0 RenderSceneVS( 1, true, true );
        PixelShader = compile ps_2_0 RenderScenePS( true );
    }
}

technique RenderSceneWithTexture2Light
{
    pass P0
    {
        VertexShader = compile vs_2_0 RenderSceneVS( 2, true, true );
        PixelShader = compile ps_2_0 RenderScenePS( true );
    }
}

technique RenderSceneWithTexture3Light
{
    pass P0
    {
        VertexShader = compile vs_2_0 RenderSceneVS( 3, true, true );
        PixelShader = compile ps_2_0 RenderScenePS( true );
    }
}

technique RenderSceneNoTexture
{
    pass P0
    {
        VertexShader = compile vs_2_0 RenderSceneVS( 1, false, false );
        PixelShader = compile ps_2_0 RenderScenePS( false );
    }
}
