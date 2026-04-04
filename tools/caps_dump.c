#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static void dump_caps(const D3DCAPS9* caps) {
    printf("Caps=0x%08lx\n", caps->Caps);
    printf("Caps2=0x%08lx\n", caps->Caps2);
    printf("Caps3=0x%08lx\n", caps->Caps3);
    printf("PresentationIntervals=0x%08lx\n", caps->PresentationIntervals);
    printf("CursorCaps=0x%08lx\n", caps->CursorCaps);
    printf("DevCaps=0x%08lx\n", caps->DevCaps);
    printf("PrimitiveMiscCaps=0x%08lx\n", caps->PrimitiveMiscCaps);
    printf("RasterCaps=0x%08lx\n", caps->RasterCaps);
    printf("ZCmpCaps=0x%08lx\n", caps->ZCmpCaps);
    printf("SrcBlendCaps=0x%08lx\n", caps->SrcBlendCaps);
    printf("DestBlendCaps=0x%08lx\n", caps->DestBlendCaps);
    printf("AlphaCmpCaps=0x%08lx\n", caps->AlphaCmpCaps);
    printf("ShadeCaps=0x%08lx\n", caps->ShadeCaps);
    printf("TextureCaps=0x%08lx\n", caps->TextureCaps);
    printf("TextureFilterCaps=0x%08lx\n", caps->TextureFilterCaps);
    printf("CubeTextureFilterCaps=0x%08lx\n", caps->CubeTextureFilterCaps);
    printf("VolumeTextureFilterCaps=0x%08lx\n", caps->VolumeTextureFilterCaps);
    printf("TextureAddressCaps=0x%08lx\n", caps->TextureAddressCaps);
    printf("VolumeTextureAddressCaps=0x%08lx\n", caps->VolumeTextureAddressCaps);
    printf("LineCaps=0x%08lx\n", caps->LineCaps);
    printf("MaxTextureWidth=%lu\n", caps->MaxTextureWidth);
    printf("MaxTextureHeight=%lu\n", caps->MaxTextureHeight);
    printf("MaxVolumeExtent=%lu\n", caps->MaxVolumeExtent);
    printf("MaxTextureRepeat=%lu\n", caps->MaxTextureRepeat);
    printf("MaxTextureAspectRatio=%lu\n", caps->MaxTextureAspectRatio);
    printf("MaxAnisotropy=%lu\n", caps->MaxAnisotropy);
    printf("MaxVertexW=%f\n", caps->MaxVertexW);
    printf("GuardBandLeft=%f\n", caps->GuardBandLeft);
    printf("GuardBandTop=%f\n", caps->GuardBandTop);
    printf("GuardBandRight=%f\n", caps->GuardBandRight);
    printf("GuardBandBottom=%f\n", caps->GuardBandBottom);
    printf("ExtentsAdjust=%f\n", caps->ExtentsAdjust);
    printf("StencilCaps=0x%08lx\n", caps->StencilCaps);
    printf("FVFCaps=0x%08lx\n", caps->FVFCaps);
    printf("TextureOpCaps=0x%08lx\n", caps->TextureOpCaps);
    printf("MaxTextureBlendStages=%lu\n", caps->MaxTextureBlendStages);
    printf("MaxSimultaneousTextures=%lu\n", caps->MaxSimultaneousTextures);
    printf("VertexProcessingCaps=0x%08lx\n", caps->VertexProcessingCaps);
    printf("MaxActiveLights=%lu\n", caps->MaxActiveLights);
    printf("MaxUserClipPlanes=%lu\n", caps->MaxUserClipPlanes);
    printf("MaxVertexBlendMatrices=%lu\n", caps->MaxVertexBlendMatrices);
    printf("MaxVertexBlendMatrixIndex=%lu\n", caps->MaxVertexBlendMatrixIndex);
    printf("MaxPointSize=%f\n", caps->MaxPointSize);
    printf("MaxPrimitiveCount=%lu\n", caps->MaxPrimitiveCount);
    printf("MaxVertexIndex=%lu\n", caps->MaxVertexIndex);
    printf("MaxStreams=%lu\n", caps->MaxStreams);
    printf("MaxStreamStride=%lu\n", caps->MaxStreamStride);
    printf("VertexShaderVersion=0x%08lx\n", caps->VertexShaderVersion);
    printf("MaxVertexShaderConst=%lu\n", caps->MaxVertexShaderConst);
    printf("PixelShaderVersion=0x%08lx\n", caps->PixelShaderVersion);
    printf("PixelShader1xMaxValue=%f\n", caps->PixelShader1xMaxValue);
    printf("DevCaps2=0x%08lx\n", caps->DevCaps2);
    printf("MaxNpatchTessellationLevel=%f\n", caps->MaxNpatchTessellationLevel);
    printf("MasterAdapterOrdinal=%u\n", caps->MasterAdapterOrdinal);
    printf("AdapterOrdinalInGroup=%u\n", caps->AdapterOrdinalInGroup);
    printf("NumberOfAdaptersInGroup=%u\n", caps->NumberOfAdaptersInGroup);
    printf("DeclTypes=0x%08lx\n", caps->DeclTypes);
    printf("NumSimultaneousRTs=%lu\n", caps->NumSimultaneousRTs);
    printf("StretchRectFilterCaps=0x%08lx\n", caps->StretchRectFilterCaps);
    printf("VS20Caps.Caps=0x%08lx\n", caps->VS20Caps.Caps);
    printf("VS20Caps.DynamicFlowControlDepth=%d\n", caps->VS20Caps.DynamicFlowControlDepth);
    printf("VS20Caps.NumTemps=%d\n", caps->VS20Caps.NumTemps);
    printf("VS20Caps.StaticFlowControlDepth=%d\n", caps->VS20Caps.StaticFlowControlDepth);
    printf("PS20Caps.Caps=0x%08lx\n", caps->PS20Caps.Caps);
    printf("PS20Caps.DynamicFlowControlDepth=%d\n", caps->PS20Caps.DynamicFlowControlDepth);
    printf("PS20Caps.NumTemps=%d\n", caps->PS20Caps.NumTemps);
    printf("PS20Caps.StaticFlowControlDepth=%d\n", caps->PS20Caps.StaticFlowControlDepth);
    printf("PS20Caps.NumInstructionSlots=%d\n", caps->PS20Caps.NumInstructionSlots);
    printf("VertexTextureFilterCaps=0x%08lx\n", caps->VertexTextureFilterCaps);
    printf("MaxVShaderInstructionsExecuted=%lu\n", caps->MaxVShaderInstructionsExecuted);
    printf("MaxPShaderInstructionsExecuted=%lu\n", caps->MaxPShaderInstructionsExecuted);
    printf("MaxVertexShader30InstructionSlots=%lu\n", caps->MaxVertexShader30InstructionSlots);
    printf("MaxPixelShader30InstructionSlots=%lu\n", caps->MaxPixelShader30InstructionSlots);
}

int main(void) {
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        fprintf(stderr, "Direct3DCreate9 failed\n");
        return 1;
    }

    D3DADAPTER_IDENTIFIER9 ident;
    HRESULT hr = IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &ident);
    if (FAILED(hr)) {
        fprintf(stderr, "GetAdapterIdentifier failed: 0x%08lx\n", hr);
        IDirect3D9_Release(d3d);
        return 2;
    }

    D3DCAPS9 caps;
    hr = IDirect3D9_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    if (FAILED(hr)) {
        fprintf(stderr, "GetDeviceCaps failed: 0x%08lx\n", hr);
        IDirect3D9_Release(d3d);
        return 3;
    }

    printf("Driver=%s\n", ident.Driver);
    printf("Description=%s\n", ident.Description);
    printf("DeviceName=%s\n", ident.DeviceName);
    printf("VendorId=0x%08lx\n", ident.VendorId);
    printf("DeviceId=0x%08lx\n", ident.DeviceId);
    printf("SubSysId=0x%08lx\n", ident.SubSysId);
    printf("Revision=0x%08lx\n", ident.Revision);
    printf("WHQLLevel=%lu\n", ident.WHQLLevel);
    dump_caps(&caps);

    IDirect3D9_Release(d3d);
    return 0;
}
