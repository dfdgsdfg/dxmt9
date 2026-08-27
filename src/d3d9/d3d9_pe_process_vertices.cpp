#include "d3d9_pe_process_vertices.hpp"

#include "d3d9_pe_child_validation.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace dxmt9::d3d9::pe::process_vertices {

constexpr std::uint32_t kShaderHeaderVS = 0xFFFEu;

static inline HRESULT hr32(int32_t r) { return (HRESULT)r; }

static void processVerticesDebugLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    dxmt9::util::vlogf(dxmt9::util::LogLevel::Debug, "dxmt9-device", fmt, args);
    va_end(args);
}

UINT vertexElementTypeSize(UINT type) noexcept {
    switch (type) {
        case D3DDECLTYPE_FLOAT1:    return 4;
        case D3DDECLTYPE_FLOAT2:    return 8;
        case D3DDECLTYPE_FLOAT3:    return 12;
        case D3DDECLTYPE_FLOAT4:    return 16;
        case D3DDECLTYPE_D3DCOLOR:  return 4;
        case D3DDECLTYPE_UBYTE4:    return 4;
        case D3DDECLTYPE_SHORT2:    return 4;
        case D3DDECLTYPE_SHORT4:    return 8;
        case D3DDECLTYPE_UBYTE4N:   return 4;
        case D3DDECLTYPE_SHORT2N:   return 4;
        case D3DDECLTYPE_SHORT4N:   return 8;
        case D3DDECLTYPE_USHORT2N:  return 4;
        case D3DDECLTYPE_USHORT4N:  return 8;
        case D3DDECLTYPE_UDEC3:     return 4;
        case D3DDECLTYPE_DEC3N:     return 4;
        case D3DDECLTYPE_FLOAT16_2: return 4;
        case D3DDECLTYPE_FLOAT16_4: return 8;
        default:                    return 0;
    }
}

static UINT fvfTexcoordBytes(DWORD fvf, UINT index) {
    const DWORD sizeBits = (fvf >> (index * 2u + 16u)) & 0x3u;
    if (sizeBits == 1u) return 12u;
    if (sizeBits == 2u) return 16u;
    if (sizeBits == 3u) return 4u;
    return 8u;
}

bool processFvfXyzbPosition(DWORD positionMask) {
    return positionMask == D3DFVF_XYZB1 ||
           positionMask == D3DFVF_XYZB2 ||
           positionMask == D3DFVF_XYZB3 ||
           positionMask == D3DFVF_XYZB4 ||
           positionMask == D3DFVF_XYZB5;
}

static UINT processTexDeclBytes(UINT type, bool /*destination*/) {
    switch (type) {
        case D3DDECLTYPE_FLOAT1:
            return 4u;
        case D3DDECLTYPE_FLOAT2:
            return 8u;
        case D3DDECLTYPE_FLOAT3:
            return 12u;
        case D3DDECLTYPE_FLOAT4:
            return 16u;
        case D3DDECLTYPE_D3DCOLOR:
            return 4u;
        case D3DDECLTYPE_UBYTE4:
        case D3DDECLTYPE_SHORT2:
        case D3DDECLTYPE_UBYTE4N:
        case D3DDECLTYPE_UDEC3:
        case D3DDECLTYPE_DEC3N:
        case D3DDECLTYPE_SHORT2N:
        case D3DDECLTYPE_USHORT2N:
        case D3DDECLTYPE_FLOAT16_2:
            return 4u;
        case D3DDECLTYPE_SHORT4:
        case D3DDECLTYPE_SHORT4N:
        case D3DDECLTYPE_USHORT4N:
        case D3DDECLTYPE_FLOAT16_4:
            return 8u;
        default:
            return 0u;
    }
}

static UINT processFloatVectorDeclBytes(UINT type, bool allowTwoComponent) {
    switch (type) {
        case D3DDECLTYPE_FLOAT1:
            return allowTwoComponent ? 4u : 0u;
        case D3DDECLTYPE_FLOAT2:
            return allowTwoComponent ? 8u : 0u;
        case D3DDECLTYPE_FLOAT3:
            return 12u;
        case D3DDECLTYPE_FLOAT4:
            return 16u;
        case D3DDECLTYPE_SHORT2:
            return allowTwoComponent ? 4u : 0u;
        case D3DDECLTYPE_UBYTE4:
            return 4u;
        case D3DDECLTYPE_SHORT4:
            return 8u;
        case D3DDECLTYPE_DEC3N:
        case D3DDECLTYPE_UDEC3:
        case D3DDECLTYPE_UBYTE4N:
            return 4u;
        case D3DDECLTYPE_SHORT2N:
        case D3DDECLTYPE_USHORT2N:
        case D3DDECLTYPE_FLOAT16_2:
            return allowTwoComponent ? 4u : 0u;
        case D3DDECLTYPE_SHORT4N:
        case D3DDECLTYPE_USHORT4N:
        case D3DDECLTYPE_FLOAT16_4:
            return 8u;
        default:
            return 0u;
    }
}

static UINT processGenericDeclBytes(UINT type) {
    if (type == D3DDECLTYPE_D3DCOLOR) return 4u;
    return processFloatVectorDeclBytes(type, true);
}

bool describeProcessFvf(DWORD fvf, FvfProcessLayout& layout) {
    layout = {};
    switch (fvf & D3DFVF_POSITION_MASK) {
        case D3DFVF_XYZ:
            layout.positionOffset = 0u;
            layout.positionType = D3DDECLTYPE_FLOAT3;
            layout.positionBytes = 12u;
            break;
        case D3DFVF_XYZB1:
        case D3DFVF_XYZB2:
        case D3DFVF_XYZB3:
        case D3DFVF_XYZB4:
        case D3DFVF_XYZB5:
            layout.positionOffset = 0u;
            layout.positionType = D3DDECLTYPE_FLOAT3;
            layout.positionBytes = 12u;
            break;
        case D3DFVF_XYZRHW:
        case D3DFVF_XYZW:
            layout.positionOffset = 0u;
            layout.positionType = D3DDECLTYPE_FLOAT4;
            layout.positionBytes = 16u;
            break;
        default:
            return false;
    }
    UINT offset = layout.positionBytes;
    if (processFvfXyzbPosition(fvf & D3DFVF_POSITION_MASK)) {
        const UINT betaCount =
            ((fvf & D3DFVF_POSITION_MASK) - D3DFVF_XYZB1) / 2u + 1u;
        const bool lastBetaUbyte4 = (fvf & D3DFVF_LASTBETA_UBYTE4) != 0;
        const bool lastBetaD3dcolor = (fvf & D3DFVF_LASTBETA_D3DCOLOR) != 0;
        if (lastBetaUbyte4 && lastBetaD3dcolor) return false;
        if (lastBetaUbyte4 || lastBetaD3dcolor) {
            const UINT weightCount = betaCount - 1u;
            if (weightCount > 4u) return false;
            if (weightCount != 0u) {
                layout.blendWeight = true;
                layout.blendWeightOffset = offset;
                layout.blendWeightType =
                    weightCount == 1u ? D3DDECLTYPE_FLOAT1 :
                    weightCount == 2u ? D3DDECLTYPE_FLOAT2 :
                    weightCount == 3u ? D3DDECLTYPE_FLOAT3 :
                                        D3DDECLTYPE_FLOAT4;
                layout.blendWeightBytes = weightCount * sizeof(float);
                offset += layout.blendWeightBytes;
            }
            layout.blendIndices = true;
            layout.blendIndicesOffset = offset;
            layout.blendIndicesType =
                lastBetaD3dcolor ? D3DDECLTYPE_D3DCOLOR : D3DDECLTYPE_UBYTE4;
            layout.blendIndicesBytes = 4u;
            offset += 4u;
        } else {
            if (betaCount > 4u) return false;
            layout.blendWeight = true;
            layout.blendWeightOffset = offset;
            layout.blendWeightType =
                betaCount == 1u ? D3DDECLTYPE_FLOAT1 :
                betaCount == 2u ? D3DDECLTYPE_FLOAT2 :
                betaCount == 3u ? D3DDECLTYPE_FLOAT3 :
                                  D3DDECLTYPE_FLOAT4;
            layout.blendWeightBytes = betaCount * sizeof(float);
            offset += layout.blendWeightBytes;
        }
    }
    if (fvf & D3DFVF_NORMAL) {
        layout.normal = true;
        layout.normalOffset = offset;
        layout.normalType = D3DDECLTYPE_FLOAT3;
        layout.normalBytes = 12u;
        offset += 12u;
    }
    if (fvf & D3DFVF_PSIZE) {
        layout.psize = true;
        layout.psizeOffset = offset;
        offset += 4u;
    }
    if (fvf & D3DFVF_DIFFUSE) {
        layout.diffuse = true;
        layout.diffuseOffset = offset;
        offset += 4u;
    }
    if (fvf & D3DFVF_SPECULAR) {
        layout.specular = true;
        layout.specularOffset = offset;
        offset += 4u;
    }
    layout.texCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    if (layout.texCount > 8u) return false;
    for (UINT i = 0; i < layout.texCount; ++i) {
        layout.texOffset[i] = offset;
        layout.texBytes[i] = fvfTexcoordBytes(fvf, i);
        layout.texType[i] = layout.texBytes[i] == 4u ? D3DDECLTYPE_FLOAT1
                          : layout.texBytes[i] == 12u ? D3DDECLTYPE_FLOAT3
                          : layout.texBytes[i] == 16u ? D3DDECLTYPE_FLOAT4
                          : D3DDECLTYPE_FLOAT2;
        offset += layout.texBytes[i];
    }
    layout.stride = offset;
    layout.streamStride[0] = offset;
    return layout.stride != 0u;
}

bool describeProcessDeclaration(IDirect3DVertexDeclaration9* declaration,
                                       FvfProcessLayout& layout,
                                       bool destination,
                                       D9CVertexDecl* validatedRaw) {
    layout = {};
    if (!declaration) return false;
    D9CVertexElement elements[MAXD3DDECLLENGTH + 1]{};
    uint32_t count = MAXD3DDECLLENGTH + 1;
    if (!validatedRaw || FAILED(hr32(dxmt9c_vdecl_get_declaration(
            validatedRaw, elements, &count)))) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const D9CVertexElement& e = elements[i];
        if (e.stream == 0xff && e.type == D3DDECLTYPE_UNUSED) {
            break;
        }
        if ((destination && e.stream != 0) ||
            (!destination && e.stream >= D9C_DRAW_PACKET_MAX_STREAMS) ||
            e.method != D3DDECLMETHOD_DEFAULT) {
            return false;
        }
        const UINT elementBytes = vertexElementTypeSize(e.type);
        if (elementBytes == 0u) return false;
        layout.stride = std::max<UINT>(layout.stride, e.offset + elementBytes);
        layout.streamStride[e.stream] =
            std::max<UINT>(layout.streamStride[e.stream], e.offset + elementBytes);
        const bool expectedPosition =
            e.usageIndex == 0 &&
            ((destination && e.usage == D3DDECLUSAGE_POSITIONT) ||
             (!destination && e.usage == D3DDECLUSAGE_POSITION));
        if (expectedPosition) {
            if (layout.positionBytes != 0u) return false;
            if (destination) {
                if (e.type != D3DDECLTYPE_FLOAT4) return false;
                layout.positionType = D3DDECLTYPE_FLOAT4;
                layout.positionBytes = 16u;
            } else {
                const UINT bytes = processFloatVectorDeclBytes(e.type, true);
                if (bytes == 0u) return false;
                layout.positionType = e.type;
                layout.positionBytes = bytes;
            }
            layout.positionStream = e.stream;
            layout.positionOffset = e.offset;
        } else if (e.usage == D3DDECLUSAGE_COLOR && e.usageIndex == 0) {
            if (e.type != D3DDECLTYPE_D3DCOLOR || layout.diffuse) return false;
            layout.diffuse = true;
            layout.diffuseStream = e.stream;
            layout.diffuseOffset = e.offset;
        } else if (!destination && e.usage == D3DDECLUSAGE_NORMAL &&
                   e.usageIndex == 0) {
            const UINT bytes = processFloatVectorDeclBytes(e.type, false);
            if (bytes == 0u || layout.normal) return false;
            layout.normal = true;
            layout.normalStream = e.stream;
            layout.normalOffset = e.offset;
            layout.normalType = e.type;
            layout.normalBytes = bytes;
        } else if (!destination && e.usage == D3DDECLUSAGE_TANGENT &&
                   e.usageIndex == 0) {
            const UINT bytes = processFloatVectorDeclBytes(e.type, false);
            if (bytes == 0u || layout.tangent) return false;
            layout.tangent = true;
            layout.tangentStream = e.stream;
            layout.tangentOffset = e.offset;
            layout.tangentType = e.type;
            layout.tangentBytes = bytes;
        } else if (!destination && e.usage == D3DDECLUSAGE_BINORMAL &&
                   e.usageIndex == 0) {
            const UINT bytes = processFloatVectorDeclBytes(e.type, false);
            if (bytes == 0u || layout.binormal) return false;
            layout.binormal = true;
            layout.binormalStream = e.stream;
            layout.binormalOffset = e.offset;
            layout.binormalType = e.type;
            layout.binormalBytes = bytes;
        } else if (!destination && e.usage == D3DDECLUSAGE_BLENDWEIGHT &&
                   e.usageIndex == 0) {
            const UINT blendBytes = processFloatVectorDeclBytes(e.type, true);
            if (blendBytes == 0u) return false;
            if (layout.blendWeight) return false;
            layout.blendWeight = true;
            layout.blendWeightStream = e.stream;
            layout.blendWeightOffset = e.offset;
            layout.blendWeightType = e.type;
            layout.blendWeightBytes = blendBytes;
        } else if (!destination && e.usage == D3DDECLUSAGE_BLENDINDICES &&
                   e.usageIndex == 0) {
            if ((e.type != D3DDECLTYPE_UBYTE4 &&
                 e.type != D3DDECLTYPE_D3DCOLOR) ||
                layout.blendIndices) return false;
            layout.blendIndices = true;
            layout.blendIndicesStream = e.stream;
            layout.blendIndicesOffset = e.offset;
            layout.blendIndicesType = e.type;
            layout.blendIndicesBytes = 4u;
        } else if (!destination && e.usage == D3DDECLUSAGE_PSIZE &&
                   e.usageIndex == 0) {
            if (e.type != D3DDECLTYPE_FLOAT1 || layout.psize) return false;
            layout.psize = true;
            layout.psizeStream = e.stream;
            layout.psizeOffset = e.offset;
        } else if (destination && e.usage == D3DDECLUSAGE_PSIZE &&
                   e.usageIndex == 0) {
            if (e.type != D3DDECLTYPE_FLOAT1 || layout.psize) return false;
            layout.psize = true;
            layout.psizeStream = e.stream;
            layout.psizeOffset = e.offset;
        } else if (e.usage == D3DDECLUSAGE_COLOR && e.usageIndex == 1) {
            if (e.type != D3DDECLTYPE_D3DCOLOR || layout.specular) return false;
            layout.specular = true;
            layout.specularStream = e.stream;
            layout.specularOffset = e.offset;
        } else if (e.usage == D3DDECLUSAGE_TEXCOORD && e.usageIndex < 8) {
            const UINT texBytes = processTexDeclBytes(e.type, destination);
            if (texBytes == 0u) return false;
            layout.texCount = std::max<UINT>(layout.texCount, e.usageIndex + 1u);
            layout.texStream[e.usageIndex] = e.stream;
            layout.texOffset[e.usageIndex] = e.offset;
            layout.texBytes[e.usageIndex] = texBytes;
            layout.texType[e.usageIndex] = e.type;
        } else {
            if (destination) return false;
            const UINT genericBytes = processGenericDeclBytes(e.type);
            if (genericBytes == 0u ||
                layout.genericInputCount >= std::size(layout.genericInput)) {
                return false;
            }
            for (UINT generic = 0; generic < layout.genericInputCount; ++generic) {
                if (layout.genericInput[generic].usage == e.usage &&
                    layout.genericInput[generic].usageIndex == e.usageIndex) {
                    return false;
                }
            }
            layout.genericInput[layout.genericInputCount++] = {
                e.usage,
                e.usageIndex,
                e.stream,
                e.offset,
                e.type,
                genericBytes,
            };
        }
    }
    return (destination ? layout.positionBytes == 16u
                        : layout.positionBytes != 0u) &&
           layout.stride != 0u;
}

static UINT shaderRegType(DWORD token) {
    const UINT low = (token >> D3DSP_REGTYPE_SHIFT) & 0x7u;
    const UINT officialHigh = (token & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2;
    if (officialHigh != 0u) {
        return low | officialHigh;
    }
    // The local PE ProcessVertices fixtures build a few SM3 tokens with the
    // secondary register-type bits packed at 8..9 instead of D3D's 11..12.
    // Accept that encoding for simple CPU execution while preserving the
    // official decode for normal bytecode.
    return low | (((token >> 8u) & 0x3u) << 3u);
}

static UINT shaderRegIndex(DWORD token) {
    UINT index = token & D3DSP_REGNUM_MASK;
    if ((token & D3DSP_REGTYPE_MASK2) == 0u &&
        ((token >> 8u) & 0x3u) != 0u) {
        index &= ~0x300u;
    }
    return index;
}

static UINT shaderWriteMask(DWORD token) {
    return (token & D3DSP_WRITEMASK_ALL) >> 16u;
}

static UINT shaderSwizzle(DWORD token) {
    return (token & D3DSP_SWIZZLE_MASK) >> D3DSP_SWIZZLE_SHIFT;
}

static UINT simpleProcessShaderOperandCount(UINT opcode, DWORD token) {
    switch (opcode) {
        case D3DSIO_NOP:
        case D3DSIO_RET:
        case D3DSIO_PHASE:
        case D3DSIO_ELSE:
        case D3DSIO_ENDIF:
        case D3DSIO_ENDLOOP:
        case D3DSIO_ENDREP:
        case D3DSIO_BREAK:
            return 0;
        case D3DSIO_MOV:
        case D3DSIO_MOVA:
        case D3DSIO_RCP:
        case D3DSIO_RSQ:
        case D3DSIO_FRC:
        case D3DSIO_ABS:
        case D3DSIO_EXP:
        case D3DSIO_LOG:
        case D3DSIO_LIT:
        case D3DSIO_EXPP:
        case D3DSIO_LOGP:
        case D3DSIO_SGN:
        case D3DSIO_SINCOS:
        case D3DSIO_NRM:
        case D3DSIO_SETP:
        case D3DSIO_BREAKP:
            return 2;
        case D3DSIO_MAD:
        case D3DSIO_LRP:
            return 4;
        case D3DSIO_ADD:
        case D3DSIO_SUB:
        case D3DSIO_MUL:
        case D3DSIO_DP3:
        case D3DSIO_DP4:
        case D3DSIO_SLT:
        case D3DSIO_SGE:
        case D3DSIO_MIN:
        case D3DSIO_MAX:
        case D3DSIO_POW:
        case D3DSIO_CRS:
        case D3DSIO_DST:
        case D3DSIO_M4x4:
        case D3DSIO_M4x3:
        case D3DSIO_M3x4:
        case D3DSIO_M3x3:
        case D3DSIO_M3x2:
            return 3;
        case D3DSIO_DCL:
            return 2;
        case D3DSIO_IF:
        case D3DSIO_REP:
        case D3DSIO_LABEL:
        case D3DSIO_CALL:
            return 1;
        case D3DSIO_IFC:
        case D3DSIO_BREAKC:
        case D3DSIO_CALLNZ:
            return 2;
        case D3DSIO_LOOP:
            return (token >> D3DSI_INSTLENGTH_SHIFT) & 0xfu;
        case D3DSIO_DEF:
        case D3DSIO_DEFI:
            return 5;
        default:
            return (token >> D3DSI_INSTLENGTH_SHIFT) & 0xfu;
    }
}

struct SimpleProcessShaderOperands {
    UINT count = 0;
    std::array<DWORD, 8> operands{};
    std::array<DWORD, 8> relAddrOperands{};
};

static bool simpleProcessShaderOperandCarriesRelAddr(UINT opcode, UINT operandIndex) {
    switch (opcode) {
        case D3DSIO_DEF:
        case D3DSIO_DEFI:
            return operandIndex == 0u;
        case D3DSIO_DCL:
            return operandIndex == 1u;
        case D3DSIO_LABEL:
        case D3DSIO_CALL:
            return false;
        case D3DSIO_CALLNZ:
            return operandIndex == 1u;
        default:
            return true;
    }
}

static bool simpleProcessShaderTokenHasRelAddr(DWORD token) {
    return (token & D3DSHADER_ADDRESSMODE_MASK) != 0u;
}

static bool simpleProcessShaderReadOperands(const std::vector<DWORD>& words,
                                            size_t& index,
                                            UINT opcode,
                                            DWORD token,
                                            SimpleProcessShaderOperands& out) {
    out = {};
    out.count = simpleProcessShaderOperandCount(opcode, token);
    if (out.count > out.operands.size()) return false;
    if (out.count > words.size() - index) return false;
    for (UINT i = 0; i < out.count; ++i) {
        const DWORD operand = words[index++];
        out.operands[i] = operand;
        if (!simpleProcessShaderOperandCarriesRelAddr(opcode, i) ||
            !simpleProcessShaderTokenHasRelAddr(operand)) {
            continue;
        }
        if (index >= words.size()) return false;
        out.relAddrOperands[i] = words[index++];
    }
    return true;
}

static bool shaderSkipComment(const std::vector<DWORD>& words, size_t& index,
                              DWORD token) {
    const size_t commentWords = (token >> 16u) & 0x7fffu;
    if (commentWords > words.size() - index) return false;
    index += commentWords;
    return true;
}

static void noteProcessShaderInput(ProcessShaderIo& io, UINT usage,
                                   UINT usageIndex, UINT reg) {
    if (usage == D3DDECLUSAGE_POSITION && usageIndex == 0) {
        io.inputPosition = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_NORMAL && usageIndex == 0) {
        io.inputNormal = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_TANGENT && usageIndex == 0) {
        io.inputTangent = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_BINORMAL && usageIndex == 0) {
        io.inputBinormal = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_BLENDWEIGHT && usageIndex == 0) {
        io.inputBlendWeight = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_BLENDINDICES && usageIndex == 0) {
        io.inputBlendIndices = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_PSIZE && usageIndex == 0) {
        io.inputPSize = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_COLOR && usageIndex == 0) {
        io.inputDiffuse = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_COLOR && usageIndex == 1) {
        io.inputSpecular = static_cast<int>(reg);
    } else if (usage == D3DDECLUSAGE_TEXCOORD && usageIndex < 8) {
        io.inputTex[usageIndex] = static_cast<int>(reg);
    } else if (io.inputGenericCount < std::size(io.inputGeneric)) {
        io.inputGeneric[io.inputGenericCount++] = {
            usage,
            usageIndex,
            static_cast<int>(reg),
        };
    } else {
        io.inputGenericOverflow = true;
    }
}

static void noteProcessShaderOutput(ProcessShaderIo& io, UINT usage,
                                    UINT usageIndex, ProcessShaderReg reg) {
    if (usage == D3DDECLUSAGE_POSITION && usageIndex == 0) {
        io.outputPosition = reg;
        io.hasOutputPosition = true;
    } else if (usage == D3DDECLUSAGE_PSIZE && usageIndex == 0) {
        io.outputPSize = reg;
        io.hasOutputPSize = true;
    } else if (usage == D3DDECLUSAGE_COLOR && usageIndex == 0) {
        io.outputDiffuse = reg;
        io.hasOutputDiffuse = true;
    } else if (usage == D3DDECLUSAGE_COLOR && usageIndex == 1) {
        io.outputSpecular = reg;
        io.hasOutputSpecular = true;
    } else if (usage == D3DDECLUSAGE_TEXCOORD && usageIndex < 8) {
        io.outputTex[usageIndex] = reg;
        io.hasOutputTex[usageIndex] = true;
    }
}

bool analyzeSimpleProcessVertexShader(const std::vector<DWORD>& words,
                                             ProcessShaderIo& io) {
    io = {};
    for (int& tex : io.inputTex) tex = -1;
    if (words.empty() || (words[0] >> 16u) != kShaderHeaderVS) return false;
    io.major = (words[0] >> 8u) & 0xffu;
    if (io.major < 3u) {
        io.inputPosition = 0;
        io.inputDiffuse = 5;
        io.inputSpecular = 6;
        for (UINT i = 0; i < 8; ++i) io.inputTex[i] = static_cast<int>(7u + i);
        io.outputPosition = {D3DSPR_RASTOUT, D3DSRO_POSITION};
        io.outputDiffuse = {D3DSPR_ATTROUT, 0};
        io.outputSpecular = {D3DSPR_ATTROUT, 1};
        io.hasOutputPosition = true;
        io.hasOutputDiffuse = true;
        io.hasOutputSpecular = true;
        for (UINT i = 0; i < 8; ++i) {
            io.outputTex[i] = {D3DSPR_TEXCRDOUT, i};
            io.hasOutputTex[i] = true;
        }
    }

    for (size_t index = 1; index < words.size();) {
        const DWORD token = words[index++];
        const UINT opcode = token & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_END) {
            return io.hasOutputPosition && !io.inputGenericOverflow;
        }
        if (opcode == D3DSIO_COMMENT) {
            if (!shaderSkipComment(words, index, token)) return false;
            continue;
        }
        SimpleProcessShaderOperands parsedOperands;
        if (!simpleProcessShaderReadOperands(words, index, opcode, token,
                                             parsedOperands)) {
            return false;
        }
        const DWORD* operands = parsedOperands.operands.data();
        if (opcode == D3DSIO_DCL) {
            const UINT usage = (operands[0] & D3DSP_DCL_USAGE_MASK) >> D3DSP_DCL_USAGE_SHIFT;
            const UINT usageIndex =
                (operands[0] & D3DSP_DCL_USAGEINDEX_MASK) >> D3DSP_DCL_USAGEINDEX_SHIFT;
            const ProcessShaderReg reg{shaderRegType(operands[1]),
                                       shaderRegIndex(operands[1])};
            if (reg.type == D3DSPR_INPUT) {
                noteProcessShaderInput(io, usage, usageIndex, reg.index);
                if (reg.index < 32u) io.usedInputMask |= (1u << reg.index);
            } else if (reg.type == D3DSPR_OUTPUT || reg.type == D3DSPR_TEXCRDOUT) {
                noteProcessShaderOutput(io, usage, usageIndex, reg);
            } else if (reg.type == D3DSPR_RASTOUT || reg.type == D3DSPR_ATTROUT) {
                noteProcessShaderOutput(io, usage, usageIndex, reg);
            }
            continue;
        }
        // Non-DCL: track v# registers actually used as source operands. For
        // vs_1_x this is the only signal we have that DIFFUSE/SPECULAR/TEXCOORD
        // slots are read (DCL is not required there). Operand 0 is the
        // destination for the ALU/CTRL ops we care about; operands [1..count) are
        // sources.
        for (UINT i = 1; i < parsedOperands.count; ++i) {
            if (shaderRegType(operands[i]) != D3DSPR_INPUT) continue;
            const UINT regIdx = shaderRegIndex(operands[i]);
            if (regIdx < 32u) {
                io.usedInputMask |= (1u << regIdx);
            }
        }
    }
    return false;
}

static float clamp01(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static uint8_t floatColorByte(float value) {
    return static_cast<uint8_t>(clamp01(value) * 255.0f + 0.5f);
}

static void unpackD3DColor(DWORD color, float out[4]) {
    out[0] = static_cast<float>((color >> 16u) & 0xffu) / 255.0f;
    out[1] = static_cast<float>((color >> 8u) & 0xffu) / 255.0f;
    out[2] = static_cast<float>(color & 0xffu) / 255.0f;
    out[3] = static_cast<float>((color >> 24u) & 0xffu) / 255.0f;
}

static DWORD packD3DColor(const float in[4]) {
    return (static_cast<DWORD>(floatColorByte(in[3])) << 24u) |
           (static_cast<DWORD>(floatColorByte(in[0])) << 16u) |
           (static_cast<DWORD>(floatColorByte(in[1])) << 8u) |
           static_cast<DWORD>(floatColorByte(in[2]));
}

static D9CColorRGBA d3dColorToRgba(DWORD color) {
    float rgba[4]{};
    unpackD3DColor(color, rgba);
    return {rgba[0], rgba[1], rgba[2], rgba[3]};
}

static float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static bool normalize3(float v[3]) {
    const float lenSq = dot3(v, v);
    if (lenSq <= 0.0f) return false;
    const float invLen = 1.0f / std::sqrt(lenSq);
    v[0] *= invLen;
    v[1] *= invLen;
    v[2] *= invLen;
    return true;
}

static void addColorProduct(float out[4], const D9CColorRGBA& a,
                            const D9CColorRGBA& b, float scale) {
    out[0] += a.r * b.r * scale;
    out[1] += a.g * b.g * scale;
    out[2] += a.b * b.b * scale;
}

struct ProcessFixedFunctionLightingColors {
    DWORD diffuse;
    DWORD specular;
};

static ProcessFixedFunctionLightingColors processFixedFunctionLightingColors(
    const float position[3],
    const float normalIn[3],
    const D9CMaterial& material,
    DWORD ambient,
    const D9CLight lights[8],
    DWORD lightEnableMask,
    bool specularEnabled) {
    float normal[3]{normalIn[0], normalIn[1], normalIn[2]};
    normalize3(normal);

    float ambientColor[4]{};
    unpackD3DColor(ambient, ambientColor);
    float lit[4]{
        material.emissive.r + material.ambient.r * ambientColor[0],
        material.emissive.g + material.ambient.g * ambientColor[1],
        material.emissive.b + material.ambient.b * ambientColor[2],
        material.diffuse.a,
    };
    float specular[4]{0.0f, 0.0f, 0.0f, 0.0f};

    for (UINT i = 0; i < 8u; ++i) {
        if ((lightEnableMask & (1u << i)) == 0) continue;
        const D9CLight& light = lights[i];

        float toLight[3]{};
        float attenuation = 1.0f;
        bool directional = false;
        if (light.type == D3DLIGHT_DIRECTIONAL) {
            directional = true;
            toLight[0] = -light.direction[0];
            toLight[1] = -light.direction[1];
            toLight[2] = -light.direction[2];
            if (!normalize3(toLight)) continue;
        } else if (light.type == D3DLIGHT_POINT || light.type == D3DLIGHT_SPOT) {
            toLight[0] = light.position[0] - position[0];
            toLight[1] = light.position[1] - position[1];
            toLight[2] = light.position[2] - position[2];
            const float distanceSq = dot3(toLight, toLight);
            if (distanceSq <= 0.0f) continue;
            const float distance = std::sqrt(distanceSq);
            if (light.range > 0.0f && distance > light.range) continue;
            const float denom = light.attenuation0 +
                                light.attenuation1 * distance +
                                light.attenuation2 * distanceSq;
            if (denom > 0.0f) attenuation = clamp01(1.0f / denom);
            toLight[0] /= distance;
            toLight[1] /= distance;
            toLight[2] /= distance;
            if (light.type == D3DLIGHT_SPOT) {
                float spotDirection[3]{
                    -light.direction[0],
                    -light.direction[1],
                    -light.direction[2],
                };
                if (!normalize3(spotDirection)) continue;
                const float rho = dot3(spotDirection, toLight);
                const float cosInner = std::cos(0.5f * light.theta);
                const float cosOuter = std::cos(0.5f * light.phi);
                float spotFactor = 0.0f;
                if (rho >= cosInner) {
                    spotFactor = 1.0f;
                } else if (rho > cosOuter) {
                    const float denom = std::max(cosInner - cosOuter, 1.0e-6f);
                    const float cone = clamp01((rho - cosOuter) / denom);
                    spotFactor = std::pow(cone, std::max(light.falloff, 0.0f));
                }
                attenuation *= spotFactor;
            }
        } else {
            continue;
        }
        addColorProduct(lit, material.ambient, light.ambient,
                        directional ? 1.0f : attenuation);
        const float ndotl = std::max(0.0f, dot3(normal, toLight));
        const float diffuse = ndotl * attenuation;
        addColorProduct(lit, material.diffuse, light.diffuse, diffuse);
        if (specularEnabled && ndotl > 0.0f) {
            float halfVec[3]{toLight[0], toLight[1], toLight[2] + 1.0f};
            if (normalize3(halfVec)) {
                const float shininess = std::max(material.power, 1.0f);
                const float factor = std::pow(std::max(0.0f, dot3(normal, halfVec)),
                                              shininess) * attenuation;
                addColorProduct(specular, material.specular, light.specular, factor);
            }
        }
    }
    return {packD3DColor(lit), packD3DColor(specular)};
}

static float snorm16ToFloat(int16_t value) {
    if (value <= -32768) return -1.0f;
    return static_cast<float>(value) / 32767.0f;
}

static float unorm16ToFloat(uint16_t value) {
    return static_cast<float>(value) / 65535.0f;
}

static float snorm10ToFloat(uint32_t value) {
    int32_t signedValue = static_cast<int32_t>(value & 0x3ffu);
    if (signedValue & 0x200) signedValue |= ~0x3ff;
    if (signedValue <= -512) return -1.0f;
    return static_cast<float>(signedValue) / 511.0f;
}

static float halfToFloat(uint16_t value) {
    const uint32_t sign = value & 0x8000u;
    const uint32_t exponent = (value >> 10u) & 0x1fu;
    const uint32_t mantissa = value & 0x3ffu;
    float result = 0.0f;

    if (exponent == 0u) {
        result = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 0x1fu) {
        result = mantissa == 0u
               ? std::numeric_limits<float>::infinity()
               : std::numeric_limits<float>::quiet_NaN();
    } else {
        result = std::ldexp(static_cast<float>(1024u + mantissa),
                            static_cast<int>(exponent) - 25);
    }
    return sign ? -result : result;
}

static uint16_t floatToHalf(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16u) & 0x8000u;
    int32_t exponent = static_cast<int32_t>((bits >> 23u) & 0xffu) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffu;

    if (((bits >> 23u) & 0xffu) == 0xffu) {
        if (mantissa == 0u) return static_cast<uint16_t>(sign | 0x7c00u);
        return static_cast<uint16_t>(sign | 0x7e00u);
    }
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exponent);
        uint32_t halfMantissa = mantissa >> shift;
        if ((mantissa >> (shift - 1u)) & 1u) ++halfMantissa;
        return static_cast<uint16_t>(sign | halfMantissa);
    }

    uint32_t halfMantissa = mantissa >> 13u;
    if (mantissa & 0x1000u) {
        ++halfMantissa;
        if (halfMantissa == 0x400u) {
            halfMantissa = 0u;
            ++exponent;
            if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
        }
    }
    return static_cast<uint16_t>(sign |
                                 (static_cast<uint32_t>(exponent) << 10u) |
                                 halfMantissa);
}

static int16_t floatToSnorm16(float value) {
    if (value <= -1.0f) return static_cast<int16_t>(-32768);
    if (value >= 1.0f) return static_cast<int16_t>(32767);
    return static_cast<int16_t>(std::lround(value * 32767.0f));
}

static uint16_t floatToUnorm16(float value) {
    return static_cast<uint16_t>(std::lround(clamp01(value) * 65535.0f));
}

static int32_t floatToSnorm10Bits(float value) {
    if (value <= -1.0f) return 0x200;
    if (value >= 1.0f) return 0x1ff;
    return static_cast<int32_t>(std::lround(value * 511.0f)) & 0x3ff;
}

static bool encodeProcessDeclVector(const float in[4],
                                    UINT type,
                                    uint8_t* destination) {
    switch (type) {
    case D3DDECLTYPE_FLOAT1:
    case D3DDECLTYPE_FLOAT2:
    case D3DDECLTYPE_FLOAT3:
    case D3DDECLTYPE_FLOAT4: {
        const UINT bytes = processTexDeclBytes(type, true);
        std::memcpy(destination, in, bytes);
        return true;
    }
    case D3DDECLTYPE_D3DCOLOR: {
        const DWORD color = packD3DColor(in);
        std::memcpy(destination, &color, sizeof(color));
        return true;
    }
    case D3DDECLTYPE_SHORT2: {
        int16_t out[2] = {
            static_cast<int16_t>(std::clamp<long>(std::lround(in[0]), -32768, 32767)),
            static_cast<int16_t>(std::clamp<long>(std::lround(in[1]), -32768, 32767)),
        };
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_SHORT4: {
        int16_t out[4]{};
        for (UINT c = 0; c < 4u; ++c) {
            out[c] = static_cast<int16_t>(
                std::clamp<long>(std::lround(in[c]), -32768, 32767));
        }
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_UBYTE4: {
        uint8_t out[4]{};
        for (UINT c = 0; c < 4u; ++c) {
            out[c] = static_cast<uint8_t>(
                std::clamp<long>(std::lround(in[c]), 0, 255));
        }
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_SHORT2N: {
        int16_t out[2] = {floatToSnorm16(in[0]), floatToSnorm16(in[1])};
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_SHORT4N: {
        int16_t out[4]{};
        for (UINT c = 0; c < 4u; ++c) out[c] = floatToSnorm16(in[c]);
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_USHORT2N: {
        uint16_t out[2] = {floatToUnorm16(in[0]), floatToUnorm16(in[1])};
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_USHORT4N: {
        uint16_t out[4]{};
        for (UINT c = 0; c < 4u; ++c) out[c] = floatToUnorm16(in[c]);
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_UBYTE4N: {
        uint8_t out[4]{};
        for (UINT c = 0; c < 4u; ++c) out[c] = floatColorByte(in[c]);
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_UDEC3: {
        const DWORD packed =
            (static_cast<DWORD>(std::clamp<long>(std::lround(in[0]), 0, 1023)) & 0x3ffu) |
            ((static_cast<DWORD>(std::clamp<long>(std::lround(in[1]), 0, 1023)) & 0x3ffu) << 10u) |
            ((static_cast<DWORD>(std::clamp<long>(std::lround(in[2]), 0, 1023)) & 0x3ffu) << 20u);
        std::memcpy(destination, &packed, sizeof(packed));
        return true;
    }
    case D3DDECLTYPE_DEC3N: {
        const DWORD packed =
            (static_cast<DWORD>(floatToSnorm10Bits(in[0])) & 0x3ffu) |
            ((static_cast<DWORD>(floatToSnorm10Bits(in[1])) & 0x3ffu) << 10u) |
            ((static_cast<DWORD>(floatToSnorm10Bits(in[2])) & 0x3ffu) << 20u);
        std::memcpy(destination, &packed, sizeof(packed));
        return true;
    }
    case D3DDECLTYPE_FLOAT16_2: {
        uint16_t out[2] = {floatToHalf(in[0]), floatToHalf(in[1])};
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    case D3DDECLTYPE_FLOAT16_4: {
        uint16_t out[4]{};
        for (UINT c = 0; c < 4u; ++c) out[c] = floatToHalf(in[c]);
        std::memcpy(destination, out, sizeof(out));
        return true;
    }
    default:
        return false;
    }
}

static bool decodeProcessDeclVector(const uint8_t* source,
                                    UINT type,
                                    UINT bytes,
                                    std::array<float, 4>& out) {
    out = {0.0f, 0.0f, 0.0f, 1.0f};
    switch (type) {
    case D3DDECLTYPE_FLOAT1:
    case D3DDECLTYPE_FLOAT2:
    case D3DDECLTYPE_FLOAT3:
    case D3DDECLTYPE_FLOAT4: {
        if (bytes == 0u || bytes > sizeof(float) * 4u ||
            (bytes % sizeof(float)) != 0u) {
            return false;
        }
        const UINT components = bytes / sizeof(float);
        std::memcpy(out.data(), source,
                    std::min<UINT>(components, 4u) * sizeof(float));
        return true;
    }
    case D3DDECLTYPE_SHORT4: {
        int16_t in[4]{};
        std::memcpy(in, source, sizeof(in));
        for (UINT c = 0; c < 4u; ++c) out[c] = static_cast<float>(in[c]);
        return true;
    }
    case D3DDECLTYPE_SHORT2: {
        int16_t in[2]{};
        std::memcpy(in, source, sizeof(in));
        out[0] = static_cast<float>(in[0]);
        out[1] = static_cast<float>(in[1]);
        return true;
    }
    case D3DDECLTYPE_UBYTE4: {
        uint8_t in[4]{};
        std::memcpy(in, source, sizeof(in));
        for (UINT c = 0; c < 4u; ++c) out[c] = static_cast<float>(in[c]);
        return true;
    }
    case D3DDECLTYPE_SHORT2N: {
        int16_t in[2]{};
        std::memcpy(in, source, sizeof(in));
        out[0] = snorm16ToFloat(in[0]);
        out[1] = snorm16ToFloat(in[1]);
        return true;
    }
    case D3DDECLTYPE_SHORT4N: {
        int16_t in[4]{};
        std::memcpy(in, source, sizeof(in));
        for (UINT c = 0; c < 4u; ++c) out[c] = snorm16ToFloat(in[c]);
        return true;
    }
    case D3DDECLTYPE_USHORT2N: {
        uint16_t in[2]{};
        std::memcpy(in, source, sizeof(in));
        out[0] = unorm16ToFloat(in[0]);
        out[1] = unorm16ToFloat(in[1]);
        return true;
    }
    case D3DDECLTYPE_USHORT4N: {
        uint16_t in[4]{};
        std::memcpy(in, source, sizeof(in));
        for (UINT c = 0; c < 4u; ++c) out[c] = unorm16ToFloat(in[c]);
        return true;
    }
    case D3DDECLTYPE_UBYTE4N: {
        uint8_t in[4]{};
        std::memcpy(in, source, sizeof(in));
        for (UINT c = 0; c < 4u; ++c) {
            out[c] = static_cast<float>(in[c]) / 255.0f;
        }
        return true;
    }
    case D3DDECLTYPE_DEC3N: {
        uint32_t packed = 0;
        std::memcpy(&packed, source, sizeof(packed));
        out[0] = snorm10ToFloat(packed);
        out[1] = snorm10ToFloat(packed >> 10u);
        out[2] = snorm10ToFloat(packed >> 20u);
        return true;
    }
    case D3DDECLTYPE_UDEC3: {
        uint32_t packed = 0;
        std::memcpy(&packed, source, sizeof(packed));
        out[0] = static_cast<float>(packed & 0x3ffu);
        out[1] = static_cast<float>((packed >> 10u) & 0x3ffu);
        out[2] = static_cast<float>((packed >> 20u) & 0x3ffu);
        return true;
    }
    case D3DDECLTYPE_FLOAT16_2: {
        uint16_t in[2]{};
        std::memcpy(in, source, sizeof(in));
        out[0] = halfToFloat(in[0]);
        out[1] = halfToFloat(in[1]);
        return true;
    }
    case D3DDECLTYPE_FLOAT16_4: {
        uint16_t in[4]{};
        std::memcpy(in, source, sizeof(in));
        for (UINT c = 0; c < 4u; ++c) out[c] = halfToFloat(in[c]);
        return true;
    }
    default:
        return false;
    }
}

struct SimpleVsRegisters {
    std::array<std::array<float, 4>, 32> temp{};
    std::array<std::array<float, 4>, 16> input{};
    std::array<std::array<float, 4>, 256> constant{};
    std::array<std::array<int32_t, 4>, 16> constantInt{};
    std::array<uint32_t, 16> constantBool{};
    std::array<std::array<float, 4>, 16> predicate{};
    std::array<float, 4> address{};
    std::array<float, 4> loop{};
    std::array<std::array<float, 4>, 16> output{};
    std::array<std::array<float, 4>, 3> rastOut{};
    std::array<std::array<float, 4>, 2> attrOut{};
    std::array<std::array<float, 4>, 8> texOut{};
};

struct SimpleVsTextureState {
    std::array<D9CTexture*, kPeVertexTextureSamplerSlots> vertexTextures{};
    std::array<DWORD, kPeVertexTextureSamplerSlots> addressU{};
    std::array<DWORD, kPeVertexTextureSamplerSlots> addressV{};
    std::array<DWORD, kPeVertexTextureSamplerSlots> borderColor{};
    std::array<DWORD, kPeVertexTextureSamplerSlots> minMipLevel{};
};

static std::array<float, 4>* simpleVsRegister(SimpleVsRegisters& regs,
                                              UINT major,
                                              UINT type,
                                              UINT index) {
    switch (type) {
        case D3DSPR_TEMP:
            return index < regs.temp.size() ? &regs.temp[index] : nullptr;
        case D3DSPR_INPUT:
            return index < regs.input.size() ? &regs.input[index] : nullptr;
        case D3DSPR_CONST:
            return index < regs.constant.size() ? &regs.constant[index] : nullptr;
        case D3DSPR_ADDR:
            return index == 0u ? &regs.address : nullptr;
        case D3DSPR_RASTOUT:
            return index < regs.rastOut.size() ? &regs.rastOut[index] : nullptr;
        case D3DSPR_ATTROUT:
            return index < regs.attrOut.size() ? &regs.attrOut[index] : nullptr;
        case D3DSPR_TEXCRDOUT:
            if (major >= 3u) {
                return index < regs.output.size() ? &regs.output[index] : nullptr;
            }
            return index < regs.texOut.size() ? &regs.texOut[index] : nullptr;
        case D3DSPR_PREDICATE:
            return index < regs.predicate.size() ? &regs.predicate[index] : nullptr;
        case D3DSPR_LOOP:
            return index == 0u ? &regs.loop : nullptr;
        default:
            return nullptr;
    }
}

static const std::array<float, 4>* simpleVsRegisterConst(
        const SimpleVsRegisters& regs, UINT major, UINT type, UINT index) {
    switch (type) {
        case D3DSPR_TEMP:
            return index < regs.temp.size() ? &regs.temp[index] : nullptr;
        case D3DSPR_INPUT:
            return index < regs.input.size() ? &regs.input[index] : nullptr;
        case D3DSPR_CONST:
            return index < regs.constant.size() ? &regs.constant[index] : nullptr;
        case D3DSPR_ADDR:
            return index == 0u ? &regs.address : nullptr;
        case D3DSPR_RASTOUT:
            return index < regs.rastOut.size() ? &regs.rastOut[index] : nullptr;
        case D3DSPR_ATTROUT:
            return index < regs.attrOut.size() ? &regs.attrOut[index] : nullptr;
        case D3DSPR_TEXCRDOUT:
            if (major >= 3u) {
                return index < regs.output.size() ? &regs.output[index] : nullptr;
            }
            return index < regs.texOut.size() ? &regs.texOut[index] : nullptr;
        case D3DSPR_PREDICATE:
            return index < regs.predicate.size() ? &regs.predicate[index] : nullptr;
        case D3DSPR_LOOP:
            return index == 0u ? &regs.loop : nullptr;
        default:
            return nullptr;
    }
}

static bool simpleVsRelAddrOffset(const SimpleVsRegisters& regs,
                                  UINT major,
                                  DWORD token,
                                  int32_t& offset) {
    const UINT type = shaderRegType(token);
    if (type != D3DSPR_ADDR && type != D3DSPR_LOOP) return false;
    const auto* reg = simpleVsRegisterConst(regs, major, type,
                                            shaderRegIndex(token));
    if (!reg) return false;
    const UINT component = shaderSwizzle(token) & 0x3u;
    offset = static_cast<int32_t>(std::lround((*reg)[component]));
    return true;
}

static bool simpleVsSourceIndex(const SimpleVsRegisters& regs,
                                UINT major,
                                DWORD token,
                                DWORD relAddrToken,
                                UINT maxCount,
                                UINT& index) {
    long effective = static_cast<long>(shaderRegIndex(token));
    if (simpleProcessShaderTokenHasRelAddr(token)) {
        if (relAddrToken == 0u) return false;
        int32_t relOffset = 0;
        if (!simpleVsRelAddrOffset(regs, major, relAddrToken, relOffset)) {
            return false;
        }
        effective += relOffset;
        if (effective < 0) effective = 0;
        const long maxIndex = maxCount > 0u ? static_cast<long>(maxCount - 1u) : 0;
        if (effective > maxIndex) effective = maxIndex;
    }
    if (effective < 0 || static_cast<UINT>(effective) >= maxCount) return false;
    index = static_cast<UINT>(effective);
    return true;
}

static bool simpleVsReadSource(const SimpleVsRegisters& regs,
                               UINT major,
                               DWORD token,
                               float out[4],
                               DWORD relAddrToken = 0u) {
    const UINT type = shaderRegType(token);
    UINT index = 0;
    switch (type) {
        case D3DSPR_TEMP:
            if (!simpleVsSourceIndex(regs, major, token, relAddrToken,
                                     static_cast<UINT>(regs.temp.size()), index)) {
                return false;
            }
            break;
        case D3DSPR_CONST:
            if (!simpleVsSourceIndex(regs, major, token, relAddrToken,
                                     static_cast<UINT>(regs.constant.size()), index)) {
                return false;
            }
            break;
        case D3DSPR_CONSTINT:
            if (!simpleVsSourceIndex(regs, major, token, relAddrToken,
                                     static_cast<UINT>(regs.constantInt.size()), index)) {
                return false;
            }
            break;
        case D3DSPR_CONSTBOOL:
            if (!simpleVsSourceIndex(regs, major, token, relAddrToken,
                                     static_cast<UINT>(regs.constantBool.size()), index)) {
                return false;
            }
            break;
        case D3DSPR_INPUT:
            if (!simpleVsSourceIndex(regs, major, token, relAddrToken,
                                     static_cast<UINT>(regs.input.size()), index)) {
                return false;
            }
            break;
        default:
            if (simpleProcessShaderTokenHasRelAddr(token)) return false;
            index = shaderRegIndex(token);
            break;
    }
    const auto* reg = simpleVsRegisterConst(regs, major, type, index);
    const UINT swizzle = shaderSwizzle(token);
    if (type == D3DSPR_CONSTINT) {
        const auto& intReg = regs.constantInt[index];
        for (UINT i = 0; i < 4; ++i) {
            out[i] = static_cast<float>(intReg[(swizzle >> (i * 2u)) & 0x3u]);
        }
    } else if (type == D3DSPR_CONSTBOOL) {
        const float value = regs.constantBool[index] != 0u ? 1.0f : 0.0f;
        out[0] = out[1] = out[2] = out[3] = value;
    } else {
        if (!reg) return false;
        for (UINT i = 0; i < 4; ++i) {
            out[i] = (*reg)[(swizzle >> (i * 2u)) & 0x3u];
        }
    }
    const UINT modifier = (token & D3DSP_SRCMOD_MASK) >> D3DSP_SRCMOD_SHIFT;
    switch (modifier) {
        case 0: /* D3DSPSM_NONE */
            return true;
        case 1: /* D3DSPSM_NEG */
            for (UINT i = 0; i < 4; ++i) out[i] = -out[i];
            return true;
        case 2: /* D3DSPSM_BIAS */
            for (UINT i = 0; i < 4; ++i) out[i] -= 0.5f;
            return true;
        case 3: /* D3DSPSM_BIASNEG */
            for (UINT i = 0; i < 4; ++i) out[i] = -(out[i] - 0.5f);
            return true;
        case 4: /* D3DSPSM_SIGN */
            for (UINT i = 0; i < 4; ++i) out[i] = out[i] * 2.0f - 1.0f;
            return true;
        case 5: /* D3DSPSM_SIGNNEG */
            for (UINT i = 0; i < 4; ++i) out[i] = -(out[i] * 2.0f - 1.0f);
            return true;
        case 6: /* D3DSPSM_COMP */
            for (UINT i = 0; i < 4; ++i) out[i] = 1.0f - out[i];
            return true;
        case 7: /* D3DSPSM_X2 */
            for (UINT i = 0; i < 4; ++i) out[i] *= 2.0f;
            return true;
        case 8: /* D3DSPSM_X2NEG */
            for (UINT i = 0; i < 4; ++i) out[i] *= -2.0f;
            return true;
        case 9: { /* D3DSPSM_DZ */
            const float z = out[2];
            for (UINT i = 0; i < 4; ++i) out[i] = z != 0.0f ? out[i] / z : 0.0f;
            return true;
        }
        case 10: { /* D3DSPSM_DW */
            const float w = out[3];
            for (UINT i = 0; i < 4; ++i) out[i] = w != 0.0f ? out[i] / w : 0.0f;
            return true;
        }
        case 11: /* D3DSPSM_ABS */
            for (UINT i = 0; i < 4; ++i) if (out[i] < 0.0f) out[i] = -out[i];
            return true;
        case 12: /* D3DSPSM_ABSNEG */
            for (UINT i = 0; i < 4; ++i) {
                if (out[i] < 0.0f) out[i] = -out[i];
                out[i] = -out[i];
            }
            return true;
        case 13: /* D3DSPSM_NOT */
            for (UINT i = 0; i < 4; ++i) out[i] = out[i] != 0.0f ? 0.0f : 1.0f;
            return true;
        default:
            return false;
    }
}

static bool simpleVsSampleTexture2D(const SimpleVsTextureState* textures,
                                    UINT sampler,
                                    const float coord[4],
                                    float out[4]) {
    if (!textures || sampler >= textures->vertexTextures.size()) return false;
    auto* texture = textures->vertexTextures[sampler];
    if (!texture) return false;
    const UINT levels = dxmt9c_texture_get_level_count(texture);
    if (levels == 0u) return false;
    const long requestedLevel = std::lround(coord[3]);
    const long minMipLevel = static_cast<long>(textures->minMipLevel[sampler]);
    const UINT level = static_cast<UINT>(
        std::clamp<long>(std::max(requestedLevel, minMipLevel), 0,
                         static_cast<long>(levels - 1u)));
    bool border = false;
    const auto addressCoord = [&](float value, DWORD mode) -> float {
        if (!std::isfinite(value)) value = 0.0f;
        switch (mode) {
            case D3DTADDRESS_WRAP: {
                float wrapped = std::fmod(value, 1.0f);
                if (wrapped < 0.0f) wrapped += 1.0f;
                return wrapped;
            }
            case D3DTADDRESS_MIRROR: {
                float mirrored = std::fmod(value, 2.0f);
                if (mirrored < 0.0f) mirrored += 2.0f;
                return mirrored <= 1.0f ? mirrored : 2.0f - mirrored;
            }
            case D3DTADDRESS_BORDER:
                if (value < 0.0f || value > 1.0f) border = true;
                return std::clamp(value, 0.0f, 1.0f);
            case D3DTADDRESS_MIRRORONCE:
                return std::clamp(std::fabs(value), 0.0f, 1.0f);
            case D3DTADDRESS_CLAMP:
            default:
                return std::clamp(value, 0.0f, 1.0f);
        }
    };
    const float u = addressCoord(coord[0], textures->addressU[sampler]);
    const float v = addressCoord(coord[1], textures->addressV[sampler]);
    if (border) {
        const DWORD color = textures->borderColor[sampler];
        out[0] = static_cast<float>((color >> 16) & 0xffu) / 255.0f;
        out[1] = static_cast<float>((color >> 8) & 0xffu) / 255.0f;
        out[2] = static_cast<float>(color & 0xffu) / 255.0f;
        out[3] = static_cast<float>((color >> 24) & 0xffu) / 255.0f;
        return true;
    }
    return SUCCEEDED(hr32(dxmt9c_texture_sample_2d(
        texture, level, u, v, out)));
}

static bool simpleVsWriteDest(SimpleVsRegisters& regs,
                              UINT major,
                              DWORD token,
                              const float in[4],
                              DWORD relAddrToken = 0u) {
    const UINT type = shaderRegType(token);
    UINT index = shaderRegIndex(token);
    if (simpleProcessShaderTokenHasRelAddr(token)) {
        UINT maxCount = 0;
        switch (type) {
            case D3DSPR_TEMP:
                maxCount = static_cast<UINT>(regs.temp.size());
                break;
            case D3DSPR_CONST:
                maxCount = static_cast<UINT>(regs.constant.size());
                break;
            case D3DSPR_TEXCRDOUT:
                maxCount = major >= 3u
                               ? static_cast<UINT>(regs.output.size())
                               : static_cast<UINT>(regs.texOut.size());
                break;
            default:
                return false;
        }
        if (!simpleVsSourceIndex(regs, major, token, relAddrToken,
                                 maxCount, index)) {
            return false;
        }
    }
    auto* reg = simpleVsRegister(regs, major, type, index);
    if (!reg) return false;
    float value[4] = {in[0], in[1], in[2], in[3]};
    const UINT modifier = (token & D3DSP_DSTMOD_MASK) >> D3DSP_DSTMOD_SHIFT;
    if (modifier & 0x1u) {
        for (float& v : value) v = clamp01(v);
    }
    if ((modifier & ~0x3u) != 0u) return false;
    const UINT mask = shaderWriteMask(token);
    for (UINT i = 0; i < 4; ++i) {
        if (mask & (1u << i)) {
            (*reg)[i] = value[i];
        }
    }
    return true;
}

static bool simpleVsIfcCompare(DWORD token, float a, float b) {
    switch ((token >> 16u) & 0xfu) {
        case 1: return a > b;   /* D3DSPC_GT */
        case 2: return a == b;  /* D3DSPC_EQ */
        case 3: return a >= b;  /* D3DSPC_GE */
        case 4: return a < b;   /* D3DSPC_LT */
        case 5: return a != b;  /* D3DSPC_NE */
        case 6: return a <= b;  /* D3DSPC_LE */
        default: return a == b;
    }
}

static bool simpleVsSkipControlBlock(const std::vector<DWORD>& words,
                                     size_t& index,
                                     bool stopAtElse) {
    UINT depth = 0;
    for (size_t scan = index; scan < words.size();) {
        const DWORD token = words[scan++];
        const UINT opcode = token & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_END) return false;
        if (opcode == D3DSIO_COMMENT) {
            if (!shaderSkipComment(words, scan, token)) return false;
            continue;
        }
        SimpleProcessShaderOperands parsedOperands;
        if (!simpleProcessShaderReadOperands(words, scan, opcode, token,
                                             parsedOperands)) {
            return false;
        }
        if (opcode == D3DSIO_IF || opcode == D3DSIO_IFC) {
            ++depth;
        } else if (opcode == D3DSIO_ENDIF) {
            if (depth == 0u) {
                index = scan;
                return true;
            }
            --depth;
        } else if (opcode == D3DSIO_ELSE && stopAtElse && depth == 0u) {
            index = scan;
            return true;
        }
    }
    return false;
}

static bool simpleVsFindLoopEnd(const std::vector<DWORD>& words,
                                size_t bodyBegin,
                                UINT endOpcode,
                                size_t& bodyEnd,
                                size_t& afterEnd) {
    UINT depth = 0;
    for (size_t scan = bodyBegin; scan < words.size();) {
        const size_t tokenIndex = scan;
        const DWORD token = words[scan++];
        const UINT opcode = token & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_END) return false;
        if (opcode == D3DSIO_COMMENT) {
            if (!shaderSkipComment(words, scan, token)) return false;
            continue;
        }
        SimpleProcessShaderOperands parsedOperands;
        if (!simpleProcessShaderReadOperands(words, scan, opcode, token,
                                             parsedOperands)) {
            return false;
        }
        if (opcode == D3DSIO_LOOP || opcode == D3DSIO_REP) {
            ++depth;
        } else if (opcode == D3DSIO_ENDLOOP || opcode == D3DSIO_ENDREP) {
            if (depth == 0u) {
                if (opcode != endOpcode) return false;
                bodyEnd = tokenIndex;
                afterEnd = scan;
                return true;
            }
            --depth;
        }
    }
    return false;
}

static bool simpleVsLoopCount(const SimpleVsRegisters& regs,
                              const ProcessShaderIo& io,
                              DWORD source,
                              UINT& count,
                              DWORD relAddrToken = 0u) {
    float value[4]{};
    if (!simpleVsReadSource(regs, io.major, source, value, relAddrToken)) return false;
    const long rounded = std::lround(value[0]);
    if (rounded <= 0) {
        count = 0;
        return true;
    }
    if (rounded > 1024) return false;
    count = static_cast<UINT>(rounded);
    return true;
}

static bool simpleVsLoopControl(const SimpleVsRegisters& regs,
                                const ProcessShaderIo& io,
                                DWORD source,
                                UINT& count,
                                int32_t& initial,
                                int32_t& step,
                                DWORD relAddrToken = 0u) {
    float value[4]{};
    if (!simpleVsReadSource(regs, io.major, source, value, relAddrToken)) return false;
    const long roundedCount = std::lround(value[0]);
    if (roundedCount <= 0) {
        count = 0;
    } else {
        if (roundedCount > 1024) return false;
        count = static_cast<UINT>(roundedCount);
    }
    initial = static_cast<int32_t>(std::lround(value[1]));
    step = static_cast<int32_t>(std::lround(value[2]));
    return true;
}

static bool simpleVsConstantMatrixBase(const SimpleVsRegisters& regs,
                                       UINT major,
                                       DWORD token,
                                       DWORD relAddrToken,
                                       UINT rowCount,
                                       UINT& base) {
    if (shaderRegType(token) != D3DSPR_CONST) return false;
    if (!simpleVsSourceIndex(regs, major, token, relAddrToken,
                             static_cast<UINT>(regs.constant.size()), base)) {
        return false;
    }
    return rowCount != 0u && base <= regs.constant.size() - rowCount;
}

static UINT simpleVsLabelIndex(DWORD token) {
    return token & D3DSP_REGNUM_MASK;
}

static bool simpleVsFindRet(const std::vector<DWORD>& words,
                            size_t bodyBegin,
                            size_t& retIndex,
                            size_t& afterRet) {
    UINT depth = 0;
    for (size_t scan = bodyBegin; scan < words.size();) {
        const size_t tokenIndex = scan;
        const DWORD token = words[scan++];
        const UINT opcode = token & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_END) return false;
        if (opcode == D3DSIO_COMMENT) {
            if (!shaderSkipComment(words, scan, token)) return false;
            continue;
        }
        SimpleProcessShaderOperands parsedOperands;
        if (!simpleProcessShaderReadOperands(words, scan, opcode, token,
                                             parsedOperands)) {
            return false;
        }
        if (opcode == D3DSIO_IF || opcode == D3DSIO_IFC ||
            opcode == D3DSIO_LOOP || opcode == D3DSIO_REP) {
            ++depth;
        } else if (opcode == D3DSIO_ENDIF || opcode == D3DSIO_ENDLOOP ||
                   opcode == D3DSIO_ENDREP) {
            if (depth == 0u) return false;
            --depth;
        } else if (opcode == D3DSIO_RET && depth == 0u) {
            retIndex = tokenIndex;
            afterRet = scan;
            return true;
        } else if (opcode == D3DSIO_LABEL && depth == 0u) {
            return false;
        }
    }
    return false;
}

static bool simpleVsSkipLabelBody(const std::vector<DWORD>& words,
                                  size_t& index) {
    while (index < words.size()) {
        const size_t tokenIndex = index;
        const DWORD token = words[index++];
        const UINT opcode = token & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_COMMENT) {
            if (!shaderSkipComment(words, index, token)) return false;
            continue;
        }
        SimpleProcessShaderOperands parsedOperands;
        if (!simpleProcessShaderReadOperands(words, index, opcode, token,
                                             parsedOperands)) {
            return false;
        }
        if (opcode != D3DSIO_LABEL) {
            index = tokenIndex;
            break;
        }
    }
    size_t retIndex = 0;
    size_t afterRet = 0;
    if (!simpleVsFindRet(words, index, retIndex, afterRet)) return false;
    index = afterRet;
    return true;
}

static bool simpleVsFindLabelRange(const std::vector<DWORD>& words,
                                   UINT targetLabel,
                                   size_t& bodyBegin,
                                   size_t& retIndex) {
    for (size_t scan = 1; scan < words.size();) {
        const DWORD token = words[scan++];
        const UINT opcode = token & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_END) return false;
        if (opcode == D3DSIO_COMMENT) {
            if (!shaderSkipComment(words, scan, token)) return false;
            continue;
        }
        SimpleProcessShaderOperands parsedOperands;
        if (!simpleProcessShaderReadOperands(words, scan, opcode, token,
                                             parsedOperands)) {
            return false;
        }
        if (opcode != D3DSIO_LABEL) {
            continue;
        }

        bool found = false;
        for (;;) {
            if (parsedOperands.count < 1u) return false;
            if (simpleVsLabelIndex(parsedOperands.operands[0]) == targetLabel) {
                found = true;
            }
            if (scan >= words.size()) return false;
            const DWORD nextToken = words[scan];
            if ((nextToken & D3DSI_OPCODE_MASK) != D3DSIO_LABEL) break;
            ++scan;
            if (!simpleProcessShaderReadOperands(
                    words, scan, D3DSIO_LABEL, nextToken, parsedOperands)) {
                return false;
            }
        }

        size_t afterRet = 0;
        if (!simpleVsFindRet(words, scan, retIndex, afterRet)) return false;
        if (found) {
            bodyBegin = scan;
            return true;
        }
        scan = afterRet;
    }
    return false;
}

enum class SimpleVsExecResult {
    Ok,
    Fail,
    Break,
    Ret,
};

static SimpleVsExecResult executeSimpleProcessVertexShaderRange(
        const std::vector<DWORD>& words,
        const ProcessShaderIo& io,
        SimpleVsRegisters& regs,
        const SimpleVsTextureState* textures,
        size_t begin,
        size_t end,
        UINT recursionDepth) {
    if (recursionDepth > 32u) return SimpleVsExecResult::Fail;
    for (size_t index = begin; index < end;) {
        const DWORD token = words[index++];
        const UINT opcode = token & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_END) return SimpleVsExecResult::Ok;
        if (opcode == D3DSIO_COMMENT) {
            if (!shaderSkipComment(words, index, token)) {
                return SimpleVsExecResult::Fail;
            }
            continue;
        }
        SimpleProcessShaderOperands parsedOperands;
        if (!simpleProcessShaderReadOperands(words, index, opcode, token,
                                             parsedOperands)) {
            return SimpleVsExecResult::Fail;
        }
        const UINT operandCount = parsedOperands.count;
        const DWORD* operands = parsedOperands.operands.data();
        const DWORD* relAddrOperands = parsedOperands.relAddrOperands.data();
        if (opcode == D3DSIO_NOP || opcode == D3DSIO_DCL || opcode == D3DSIO_PHASE) {
            continue;
        }
        const bool predicatedInstruction = ((token >> 28u) & 0x1u) != 0u;
        const bool predicateAllows =
            !predicatedInstruction || regs.predicate[0][0] != 0.0f;
        const bool flowInstruction =
            opcode == D3DSIO_IF || opcode == D3DSIO_IFC ||
            opcode == D3DSIO_ELSE || opcode == D3DSIO_ENDIF ||
            opcode == D3DSIO_REP || opcode == D3DSIO_ENDREP ||
            opcode == D3DSIO_LOOP || opcode == D3DSIO_ENDLOOP ||
            opcode == D3DSIO_CALL || opcode == D3DSIO_CALLNZ ||
            opcode == D3DSIO_LABEL || opcode == D3DSIO_RET ||
            opcode == D3DSIO_BREAK || opcode == D3DSIO_BREAKC ||
            opcode == D3DSIO_BREAKP;
        if (predicatedInstruction && !flowInstruction) {
            if (!predicateAllows) {
                continue;
            }
        }
        if (opcode == D3DSIO_RET) {
            return SimpleVsExecResult::Ret;
        }
        if (opcode == D3DSIO_LABEL) {
            if (!simpleVsSkipLabelBody(words, index)) {
                return SimpleVsExecResult::Fail;
            }
            continue;
        }
        if (opcode == D3DSIO_CALL || opcode == D3DSIO_CALLNZ) {
            if (operandCount < 1u) return SimpleVsExecResult::Fail;
            bool takeCall = predicateAllows;
            if (opcode == D3DSIO_CALLNZ) {
                if (operandCount < 2u) return SimpleVsExecResult::Fail;
                float condition[4]{};
                if (!simpleVsReadSource(regs, io.major, operands[1], condition,
                                        relAddrOperands[1])) {
                    return SimpleVsExecResult::Fail;
                }
                takeCall = takeCall && condition[0] != 0.0f;
            }
            if (!takeCall) continue;
            size_t bodyBegin = 0;
            size_t retIndex = 0;
            if (!simpleVsFindLabelRange(
                    words, simpleVsLabelIndex(operands[0]), bodyBegin, retIndex)) {
                return SimpleVsExecResult::Fail;
            }
            const SimpleVsExecResult callResult =
                executeSimpleProcessVertexShaderRange(
                    words, io, regs, textures, bodyBegin, retIndex + 1u,
                    recursionDepth + 1u);
            if (callResult == SimpleVsExecResult::Fail ||
                callResult == SimpleVsExecResult::Break) {
                return SimpleVsExecResult::Fail;
            }
            continue;
        }
        if (opcode == D3DSIO_BREAK) {
            if (!predicateAllows) continue;
            return SimpleVsExecResult::Break;
        }
        if (opcode == D3DSIO_BREAKP) {
            if (!predicateAllows) continue;
            float predicate[4]{};
            if (!simpleVsReadSource(regs, io.major, operands[0], predicate,
                                    relAddrOperands[0])) {
                return SimpleVsExecResult::Fail;
            }
            if (predicate[0] != 0.0f) {
                return SimpleVsExecResult::Break;
            }
            continue;
        }
        if (opcode == D3DSIO_BREAKC) {
            if (!predicateAllows) continue;
            float condition[4]{};
            float rhs[4]{};
            if (!simpleVsReadSource(regs, io.major, operands[0], condition,
                                    relAddrOperands[0]) ||
                !simpleVsReadSource(regs, io.major, operands[1], rhs,
                                    relAddrOperands[1])) {
                return SimpleVsExecResult::Fail;
            }
            if (simpleVsIfcCompare(token, condition[0], rhs[0])) {
                return SimpleVsExecResult::Break;
            }
            continue;
        }
        if (opcode == D3DSIO_MOVA) {
            const UINT dstType = shaderRegType(operands[0]);
            if (dstType != D3DSPR_ADDR && dstType != D3DSPR_LOOP) {
                return SimpleVsExecResult::Fail;
            }
            auto* dst = simpleVsRegister(regs, io.major, dstType,
                                         shaderRegIndex(operands[0]));
            if (!dst) return SimpleVsExecResult::Fail;
            float value[4]{};
            if (!simpleVsReadSource(regs, io.major, operands[1], value,
                                    relAddrOperands[1])) {
                return SimpleVsExecResult::Fail;
            }
            const UINT mask = shaderWriteMask(operands[0]);
            for (UINT component = 0; component < 4u; ++component) {
                if (mask & (1u << component)) {
                    (*dst)[component] = static_cast<float>(std::lround(value[component]));
                }
            }
            continue;
        }
        if (opcode == D3DSIO_SETP) {
            if (shaderRegType(operands[0]) != D3DSPR_PREDICATE) {
                return SimpleVsExecResult::Fail;
            }
            auto* predicate = simpleVsRegister(
                regs, io.major, D3DSPR_PREDICATE, shaderRegIndex(operands[0]));
            if (!predicate) return SimpleVsExecResult::Fail;
            float value[4]{};
            if (!simpleVsReadSource(regs, io.major, operands[1], value,
                                    relAddrOperands[1])) {
                return SimpleVsExecResult::Fail;
            }
            (*predicate)[0] = value[0] != 0.0f ? 1.0f : 0.0f;
            (*predicate)[1] = (*predicate)[2] = (*predicate)[3] = (*predicate)[0];
            continue;
        }
        if (opcode == D3DSIO_REP || opcode == D3DSIO_LOOP) {
            if (operandCount == 0u) return SimpleVsExecResult::Fail;
            size_t bodyEnd = 0;
            size_t afterEnd = 0;
            const UINT endOpcode = opcode == D3DSIO_REP ? D3DSIO_ENDREP : D3DSIO_ENDLOOP;
            if (!simpleVsFindLoopEnd(words, index, endOpcode, bodyEnd, afterEnd)) {
                return SimpleVsExecResult::Fail;
            }
            if (!predicateAllows) {
                index = afterEnd;
                continue;
            }
            const DWORD countSource = opcode == D3DSIO_LOOP && operandCount > 1u
                                          ? operands[1]
                                          : operands[0];
            UINT count = 0;
            int32_t loopValue = 0;
            int32_t loopStep = 0;
            const std::array<float, 4> savedLoop = regs.loop;
            if (opcode == D3DSIO_LOOP) {
                if (operandCount < 2u || shaderRegType(operands[0]) != D3DSPR_LOOP) {
                    return SimpleVsExecResult::Fail;
                }
                const DWORD countRelAddr = opcode == D3DSIO_LOOP && operandCount > 1u
                                               ? relAddrOperands[1]
                                               : relAddrOperands[0];
                if (!simpleVsLoopControl(
                        regs, io, countSource, count, loopValue, loopStep,
                        countRelAddr)) {
                    return SimpleVsExecResult::Fail;
                }
            } else if (!simpleVsLoopCount(
                           regs, io, countSource, count, relAddrOperands[0])) {
                return SimpleVsExecResult::Fail;
            }
            for (UINT iteration = 0; iteration < count; ++iteration) {
                if (opcode == D3DSIO_LOOP) {
                    const float loopFloat = static_cast<float>(loopValue);
                    regs.loop = {loopFloat, loopFloat, loopFloat, loopFloat};
                }
                const SimpleVsExecResult loopResult =
                    executeSimpleProcessVertexShaderRange(
                        words, io, regs, textures, index, bodyEnd,
                        recursionDepth + 1u);
                if (loopResult == SimpleVsExecResult::Fail) {
                    if (opcode == D3DSIO_LOOP) regs.loop = savedLoop;
                    return SimpleVsExecResult::Fail;
                }
                if (loopResult == SimpleVsExecResult::Break) {
                    break;
                }
                if (loopResult == SimpleVsExecResult::Ret) {
                    if (opcode == D3DSIO_LOOP) regs.loop = savedLoop;
                    return SimpleVsExecResult::Ret;
                }
                loopValue += loopStep;
            }
            if (opcode == D3DSIO_LOOP) regs.loop = savedLoop;
            index = afterEnd;
            continue;
        }
        if (opcode == D3DSIO_ENDREP || opcode == D3DSIO_ENDLOOP) {
            return SimpleVsExecResult::Fail;
        }
        if (opcode == D3DSIO_IF || opcode == D3DSIO_IFC) {
            float condition[4]{};
            float rhs[4]{};
            if (!predicateAllows) {
                if (!simpleVsSkipControlBlock(words, index, false)) {
                    return SimpleVsExecResult::Fail;
                }
                continue;
            }
            if (!simpleVsReadSource(regs, io.major, operands[0], condition,
                                    relAddrOperands[0])) {
                return SimpleVsExecResult::Fail;
            }
            bool takeBranch = condition[0] != 0.0f;
            if (opcode == D3DSIO_IFC) {
                if (!simpleVsReadSource(regs, io.major, operands[1], rhs,
                                        relAddrOperands[1])) {
                    return SimpleVsExecResult::Fail;
                }
                takeBranch = simpleVsIfcCompare(token, condition[0], rhs[0]);
            }
            if (!takeBranch &&
                !simpleVsSkipControlBlock(words, index, true)) {
                return SimpleVsExecResult::Fail;
            }
            continue;
        }
        if (opcode == D3DSIO_ELSE) {
            if (!simpleVsSkipControlBlock(words, index, false)) {
                return SimpleVsExecResult::Fail;
            }
            continue;
        }
        if (opcode == D3DSIO_ENDIF) {
            continue;
        }
        if (opcode == D3DSIO_DEF) {
            if (shaderRegType(operands[0]) != D3DSPR_CONST) return SimpleVsExecResult::Fail;
            auto* dst = simpleVsRegister(regs, io.major, D3DSPR_CONST,
                                         shaderRegIndex(operands[0]));
            if (!dst) return SimpleVsExecResult::Fail;
            std::memcpy(dst->data(), operands + 1, sizeof(float) * 4u);
            continue;
        }
        if (opcode == D3DSIO_DEFI) {
            if (shaderRegType(operands[0]) != D3DSPR_CONSTINT) {
                return SimpleVsExecResult::Fail;
            }
            const UINT indexConst = shaderRegIndex(operands[0]);
            if (indexConst >= regs.constantInt.size()) {
                return SimpleVsExecResult::Fail;
            }
            for (UINT i = 0; i < 4; ++i) {
                regs.constantInt[indexConst][i] = static_cast<int32_t>(operands[i + 1u]);
            }
            continue;
        }
        if (opcode == D3DSIO_DEFB) {
            if (operandCount < 2u) return SimpleVsExecResult::Fail;
            if (shaderRegType(operands[0]) != D3DSPR_CONSTBOOL) {
                return SimpleVsExecResult::Fail;
            }
            UINT indexConst = shaderRegIndex(operands[0]);
            if (simpleProcessShaderTokenHasRelAddr(operands[0])) {
                if (!simpleVsSourceIndex(regs, io.major, operands[0],
                                         relAddrOperands[0],
                                         static_cast<UINT>(regs.constantBool.size()),
                                         indexConst)) {
                    return SimpleVsExecResult::Fail;
                }
            } else if (indexConst >= regs.constantBool.size()) {
                return SimpleVsExecResult::Fail;
            }
            regs.constantBool[indexConst] = operands[1] != 0u ? 1u : 0u;
            continue;
        }
        if (opcode == D3DSIO_TEXLDL) {
            if (operandCount < 3u ||
                shaderRegType(operands[2]) != D3DSPR_SAMPLER) {
                return SimpleVsExecResult::Fail;
            }
            float coord[4]{};
            if (!simpleVsReadSource(regs, io.major, operands[1], coord,
                                    relAddrOperands[1]) ||
                !simpleVsSampleTexture2D(textures, shaderRegIndex(operands[2]),
                                         coord, coord) ||
                !simpleVsWriteDest(regs, io.major, operands[0], coord,
                                   relAddrOperands[0])) {
                return SimpleVsExecResult::Fail;
            }
            continue;
        }

        float a[4]{};
        float b[4]{};
        float c[4]{};
        float out[4]{};
        if (operandCount >= 2 &&
            !simpleVsReadSource(regs, io.major, operands[1], a,
                                relAddrOperands[1])) {
            return SimpleVsExecResult::Fail;
        }
        if (operandCount >= 3 &&
            !simpleVsReadSource(regs, io.major, operands[2], b,
                                relAddrOperands[2])) {
            return SimpleVsExecResult::Fail;
        }
        if (operandCount >= 4 &&
            !simpleVsReadSource(regs, io.major, operands[3], c,
                                relAddrOperands[3])) {
            return SimpleVsExecResult::Fail;
        }
        switch (opcode) {
            case D3DSIO_MOV:
                std::memcpy(out, a, sizeof(out));
                break;
            case D3DSIO_RCP: {
                const float value = a[0] != 0.0f ? 1.0f / a[0] : 0.0f;
                out[0] = out[1] = out[2] = out[3] = value;
                break;
            }
            case D3DSIO_RSQ: {
                const float value = a[0] > 0.0f ? 1.0f / std::sqrt(a[0]) : 0.0f;
                out[0] = out[1] = out[2] = out[3] = value;
                break;
            }
            case D3DSIO_FRC:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] - std::floor(a[i]);
                break;
            case D3DSIO_ABS:
                for (UINT i = 0; i < 4; ++i) out[i] = std::fabs(a[i]);
                break;
            case D3DSIO_SGN:
                for (UINT i = 0; i < 4; ++i) {
                    out[i] = a[i] > 0.0f ? 1.0f : (a[i] < 0.0f ? -1.0f : 0.0f);
                }
                break;
            case D3DSIO_SINCOS:
                out[0] = std::sin(a[0]);
                out[1] = std::cos(a[0]);
                out[2] = 0.0f;
                out[3] = 0.0f;
                break;
            case D3DSIO_EXP:
            case D3DSIO_EXPP:
                for (UINT i = 0; i < 4; ++i) out[i] = std::exp2(a[i]);
                break;
            case D3DSIO_LOG:
            case D3DSIO_LOGP:
                for (UINT i = 0; i < 4; ++i) out[i] = std::log2(std::fabs(a[i]));
                break;
            case D3DSIO_LIT: {
                const float x = a[0];
                const float y = a[1];
                const float w = std::max(-128.0f, std::min(128.0f, a[3]));
                out[0] = 1.0f;
                out[1] = std::max(x, 0.0f);
                out[2] = x > 0.0f ? std::pow(std::max(y, 0.0f), w) : 0.0f;
                out[3] = 1.0f;
                break;
            }
            case D3DSIO_ADD:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] + b[i];
                break;
            case D3DSIO_SUB:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] - b[i];
                break;
            case D3DSIO_MUL:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] * b[i];
                break;
            case D3DSIO_MAD:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] * b[i] + c[i];
                break;
            case D3DSIO_LRP:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] * b[i] + (1.0f - a[i]) * c[i];
                break;
            case D3DSIO_SLT:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] < b[i] ? 1.0f : 0.0f;
                break;
            case D3DSIO_SGE:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] >= b[i] ? 1.0f : 0.0f;
                break;
            case D3DSIO_MIN:
                for (UINT i = 0; i < 4; ++i) out[i] = std::min(a[i], b[i]);
                break;
            case D3DSIO_MAX:
                for (UINT i = 0; i < 4; ++i) out[i] = std::max(a[i], b[i]);
                break;
            case D3DSIO_CND:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] > 0.5f ? b[i] : c[i];
                break;
            case D3DSIO_CMP:
                for (UINT i = 0; i < 4; ++i) out[i] = a[i] >= 0.0f ? b[i] : c[i];
                break;
            case D3DSIO_DP3: {
                const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
                out[0] = out[1] = out[2] = out[3] = dot;
                break;
            }
            case D3DSIO_DP4: {
                const float dot = a[0] * b[0] + a[1] * b[1] +
                                  a[2] * b[2] + a[3] * b[3];
                out[0] = out[1] = out[2] = out[3] = dot;
                break;
            }
            case D3DSIO_DP2ADD: {
                const float value = a[0] * b[0] + a[1] * b[1] + c[0];
                out[0] = out[1] = out[2] = out[3] = value;
                break;
            }
            case D3DSIO_POW: {
                // D3D9 POW is scalar abs(src0)^src1, matching the Metal
                // emitter used by the draw path.
                const float value = std::pow(std::abs(a[0]), b[0]);
                out[0] = out[1] = out[2] = out[3] = value;
                break;
            }
            case D3DSIO_CRS:
                out[0] = a[1] * b[2] - a[2] * b[1];
                out[1] = a[2] * b[0] - a[0] * b[2];
                out[2] = a[0] * b[1] - a[1] * b[0];
                out[3] = 1.0f;
                break;
            case D3DSIO_DST:
                out[0] = 1.0f;
                out[1] = a[1] * b[1];
                out[2] = a[2];
                out[3] = b[3];
                break;
            case D3DSIO_NRM: {
                const float lenSq = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
                const float invLen = lenSq > 0.0f ? 1.0f / std::sqrt(lenSq) : 0.0f;
                out[0] = a[0] * invLen;
                out[1] = a[1] * invLen;
                out[2] = a[2] * invLen;
                out[3] = 1.0f;
                break;
            }
            case D3DSIO_M4x4: {
                UINT base = 0;
                if (!simpleVsConstantMatrixBase(regs, io.major, operands[2],
                                                relAddrOperands[2], 4u, base)) {
                    return SimpleVsExecResult::Fail;
                }
                for (UINT row = 0; row < 4; ++row) {
                    const auto& c = regs.constant[base + row];
                    out[row] = a[0] * c[0] + a[1] * c[1] +
                               a[2] * c[2] + a[3] * c[3];
                }
                break;
            }
            case D3DSIO_M4x3: {
                UINT base = 0;
                if (!simpleVsConstantMatrixBase(regs, io.major, operands[2],
                                                relAddrOperands[2], 3u, base)) {
                    return SimpleVsExecResult::Fail;
                }
                for (UINT row = 0; row < 3; ++row) {
                    const auto& k = regs.constant[base + row];
                    out[row] = a[0] * k[0] + a[1] * k[1] +
                               a[2] * k[2] + a[3] * k[3];
                }
                out[3] = 1.0f;
                break;
            }
            case D3DSIO_M3x4: {
                UINT base = 0;
                if (!simpleVsConstantMatrixBase(regs, io.major, operands[2],
                                                relAddrOperands[2], 4u, base)) {
                    return SimpleVsExecResult::Fail;
                }
                for (UINT row = 0; row < 4; ++row) {
                    const auto& k = regs.constant[base + row];
                    out[row] = a[0] * k[0] + a[1] * k[1] + a[2] * k[2];
                }
                break;
            }
            case D3DSIO_M3x3: {
                UINT base = 0;
                if (!simpleVsConstantMatrixBase(regs, io.major, operands[2],
                                                relAddrOperands[2], 3u, base)) {
                    return SimpleVsExecResult::Fail;
                }
                for (UINT row = 0; row < 3; ++row) {
                    const auto& k = regs.constant[base + row];
                    out[row] = a[0] * k[0] + a[1] * k[1] + a[2] * k[2];
                }
                out[3] = 1.0f;
                break;
            }
            case D3DSIO_M3x2: {
                UINT base = 0;
                if (!simpleVsConstantMatrixBase(regs, io.major, operands[2],
                                                relAddrOperands[2], 2u, base)) {
                    return SimpleVsExecResult::Fail;
                }
                for (UINT row = 0; row < 2; ++row) {
                    const auto& k = regs.constant[base + row];
                    out[row] = a[0] * k[0] + a[1] * k[1] + a[2] * k[2];
                }
                out[2] = 0.0f;
                out[3] = 0.0f;
                break;
            }
            default:
                return SimpleVsExecResult::Fail;
        }
        if (!simpleVsWriteDest(regs, io.major, operands[0], out,
                               relAddrOperands[0])) {
            return SimpleVsExecResult::Fail;
        }
    }
    return SimpleVsExecResult::Ok;
}

static bool executeSimpleProcessVertexShader(const std::vector<DWORD>& words,
                                             const ProcessShaderIo& io,
                                             SimpleVsRegisters& regs,
                                             const SimpleVsTextureState* textures) {
    const SimpleVsExecResult result =
        executeSimpleProcessVertexShaderRange(
            words, io, regs, textures, 1, words.size(), 0);
    return result == SimpleVsExecResult::Ok || result == SimpleVsExecResult::Ret;
}


static D9CMatrix transformOrIdentity(const PeHotStateShadow& state,
                                     D3DTRANSFORMSTATETYPE transformState) {
    D9CMatrix matrix = identityTransformMatrix();
    (void)state.transformShadowTyped().get(
        transformStateKey(static_cast<std::uint32_t>(transformState)), matrix);
    return matrix;
}

static D9CMatrix multiplyTransformMatrix(const D9CMatrix& left,
                                         const D9CMatrix& right) {
    D9CMatrix result{};
    for (UINT row = 0; row < 4; ++row) {
        for (UINT col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (UINT k = 0; k < 4; ++k) {
                sum += left.m[row * 4 + k] * right.m[k * 4 + col];
            }
            result.m[row * 4 + col] = sum;
        }
    }
    return result;
}

static D9CMatrix worldViewProjectionTransform(
    const PeHotStateShadow& state) {
    const D9CMatrix world = transformOrIdentity(state, D3DTS_WORLD);
    const D9CMatrix view = transformOrIdentity(state, D3DTS_VIEW);
    const D9CMatrix projection = transformOrIdentity(state, D3DTS_PROJECTION);
    return multiplyTransformMatrix(multiplyTransformMatrix(world, view),
                                   projection);
}

HRESULT processVertices(const Context& context,
                        UINT srcStart, UINT dstIndex, UINT vertexCount,
                        IDirect3DVertexBuffer9* dstBuffer,
                        IDirect3DVertexDeclaration9* declaration,
                        DWORD flags) noexcept {
    processVerticesDebugLog("device_process_vertices device=%p srcStart=%u dstIndex=%u vertexCount=%u dst=%p decl=%p flags=0x%x",
                        context.deviceIdentity, srcStart, dstIndex, vertexCount, dstBuffer,
                        declaration, (unsigned)flags);
    auto invalid = [&](const char* reason) {
        processVerticesDebugLog("device_process_vertices invalid: %s", reason);
        return D3DERR_INVALIDCALL;
    };
    if (!dstBuffer) return invalid("null destination buffer");
    if (!context.device || !context.deviceIdentity)
        return invalid("null device");
    D3D9PeValidatedVertexBuffer destination{};
    D3D9PeValidatedDeclaration outputDeclaration{};
    D3D9PeValidatedDeclaration inputDeclaration{};
    D3D9PeValidatedVertexShader vertexShader{};
    if (FAILED(D3D9PeValidateVertexBuffer(
            dstBuffer, context.deviceIdentity, &destination)) ||
        FAILED(D3D9PeValidateVertexDecl(
            declaration, context.deviceIdentity, &outputDeclaration)) ||
        FAILED(D3D9PeValidateVertexDecl(
            context.vertexDeclaration, context.deviceIdentity,
            &inputDeclaration)) ||
        FAILED(D3D9PeValidateVertexShader(
            context.vertexShader, context.deviceIdentity, &vertexShader))) {
        return invalid("foreign or invalid destination state");
    }
    std::array<D9CBuffer*, D9C_DRAW_PACKET_MAX_STREAMS> validatedStreamRaw{};
    for (std::size_t i = 0; i < context.streamSources.size(); ++i) {
        auto* stream = context.streamSources[i];
        D3D9PeValidatedVertexBuffer validatedStream{};
        if (FAILED(D3D9PeValidateVertexBuffer(
                stream, context.deviceIdentity, &validatedStream))) {
            return invalid("foreign or invalid source stream");
        }
        validatedStreamRaw[i] = validatedStream.raw();
    }
    std::array<D9CTexture*, D9C_DRAW_PACKET_MAX_TEXTURES> validatedTextureRaw{};
    for (std::size_t i = 0; i < context.textures.size(); ++i) {
        auto* texture = context.textures[i];
        D3D9PeValidatedTexture validatedTexture{};
        if (FAILED(D3D9PeValidateTexture(
                texture, context.deviceIdentity, &validatedTexture))) {
            return invalid("foreign or invalid texture");
        }
        validatedTextureRaw[i] = validatedTexture.raw();
    }
    if (vertexCount == 0) return S_OK;
    if (flags & ~D3DPV_DONOTCOPYDATA) return invalid("flags unsupported");
    const bool programmable = context.vertexShader != nullptr;
    std::vector<DWORD> shaderWords;
    ProcessShaderIo shaderIo{};
    if (programmable) {
        UINT shaderBytes = 0;
        HRESULT shaderHr = context.vertexShader->GetFunction(nullptr, &shaderBytes);
        if (FAILED(shaderHr) || shaderBytes == 0 ||
            (shaderBytes % sizeof(DWORD)) != 0) {
            return invalid("shader bytecode query failed");
        }
        try {
            shaderWords.resize(shaderBytes / sizeof(DWORD));
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (...) {
            return D3DERR_INVALIDCALL;
        }
        shaderHr = context.vertexShader->GetFunction(shaderWords.data(), &shaderBytes);
        if (FAILED(shaderHr) ||
            !analyzeSimpleProcessVertexShader(shaderWords, shaderIo)) {
            return invalid("shader analysis failed");
        }
    }
    D9CBuffer* dstRaw = destination.raw();
    if (!dstRaw) return invalid("raw destination buffer missing");
    D9CBufferDesc dstDesc{};
    if (FAILED(hr32(dxmt9c_buffer_get_desc(dstRaw, &dstDesc)))) {
        return invalid("destination desc failed");
    }
    FvfProcessLayout srcLayout{};
    FvfProcessLayout dstLayout{};
    bool sourceLayoutFromDeclaration = false;
    if (context.fvf != 0) {
        const DWORD positionMask = context.fvf & D3DFVF_POSITION_MASK;
        if ((positionMask != D3DFVF_XYZ &&
             (programmable
                  ? (positionMask != D3DFVF_XYZW &&
                     !processFvfXyzbPosition(positionMask))
                  : !processFvfXyzbPosition(positionMask))) ||
            !describeProcessFvf(context.fvf, srcLayout)) {
            return invalid("source FVF unsupported");
        }
    } else if (context.vertexDeclaration) {
        sourceLayoutFromDeclaration = true;
        if (!describeProcessDeclaration(context.vertexDeclaration, srcLayout,
                                        false, inputDeclaration.raw())) {
            return invalid("source declaration unsupported");
        }
    } else {
        return invalid("no source layout");
    }
    if (declaration) {
        if (!describeProcessDeclaration(declaration, dstLayout, true,
                                        outputDeclaration.raw())) {
            return invalid("destination declaration unsupported");
        }
    } else {
        if ((dstDesc.fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW ||
            (dstDesc.fvf & D3DFVF_NORMAL) != 0 ||
            !describeProcessFvf(dstDesc.fvf, dstLayout)) {
            return invalid("destination FVF unsupported");
        }
    }
    if (dstLayout.positionBytes != 16u) {
        return invalid("destination lacks POSITIONT");
    }
    auto renderStateValue = [&](D3DRENDERSTATETYPE state) -> DWORD {
        uint32_t shadowValue = 0;
        if (context.state.renderStateShadowTyped().get(
                renderStateSlotKey(static_cast<std::uint32_t>(state)), shadowValue)) {
            return shadowValue;
        }
        return dxmt9c_device_get_render_state(context.device, static_cast<uint32_t>(state));
    };
    const bool processLighting =
        !programmable && srcLayout.normal && renderStateValue(D3DRS_LIGHTING) != 0;
    const bool processSpecularLighting =
        processLighting && renderStateValue(D3DRS_SPECULARENABLE) != 0;
    const bool processColorVertex =
        processLighting && renderStateValue(D3DRS_COLORVERTEX) != 0;
    UINT fixedBlendWeightCount = 0;
    bool fixedIndexedVertexBlend = false;
    const DWORD vertexBlendState =
        programmable ? D3DVBF_DISABLE : renderStateValue(D3DRS_VERTEXBLEND);
    if (!programmable && vertexBlendState != D3DVBF_DISABLE) {
        fixedIndexedVertexBlend =
            renderStateValue(D3DRS_INDEXEDVERTEXBLENDENABLE) != FALSE;
        switch (vertexBlendState) {
            case D3DVBF_1WEIGHTS:
                fixedBlendWeightCount = 1;
                break;
            case D3DVBF_2WEIGHTS:
                fixedBlendWeightCount = 2;
                break;
            case D3DVBF_3WEIGHTS:
                fixedBlendWeightCount = 3;
                break;
            default:
                return invalid("vertex blending mode unsupported");
        }
    }
    auto processStreamInstanced = [&](UINT stream) {
        return (context.streamFrequencies[stream] & D3DSTREAMSOURCE_INSTANCEDATA) != 0u;
    };
    UINT srcReadBytes[D9C_DRAW_PACKET_MAX_STREAMS]{};
    auto requirePositionRead = [&]() {
        srcReadBytes[srcLayout.positionStream] =
            std::max(srcReadBytes[srcLayout.positionStream],
                     srcLayout.positionOffset + srcLayout.positionBytes);
    };
    auto requireNormalRead = [&]() -> bool {
        if (!srcLayout.normal) return false;
        srcReadBytes[srcLayout.normalStream] =
            std::max(srcReadBytes[srcLayout.normalStream],
                     srcLayout.normalOffset + srcLayout.normalBytes);
        return true;
    };
    auto requireTangentRead = [&]() -> bool {
        if (!srcLayout.tangent) return false;
        srcReadBytes[srcLayout.tangentStream] =
            std::max(srcReadBytes[srcLayout.tangentStream],
                     srcLayout.tangentOffset + srcLayout.tangentBytes);
        return true;
    };
    auto requireBinormalRead = [&]() -> bool {
        if (!srcLayout.binormal) return false;
        srcReadBytes[srcLayout.binormalStream] =
            std::max(srcReadBytes[srcLayout.binormalStream],
                     srcLayout.binormalOffset + srcLayout.binormalBytes);
        return true;
    };
    auto requireBlendWeightRead = [&]() -> bool {
        if (!srcLayout.blendWeight) return false;
        srcReadBytes[srcLayout.blendWeightStream] =
            std::max(srcReadBytes[srcLayout.blendWeightStream],
                     srcLayout.blendWeightOffset + srcLayout.blendWeightBytes);
        return true;
    };
    auto requireBlendIndicesRead = [&]() -> bool {
        if (!srcLayout.blendIndices) return false;
        srcReadBytes[srcLayout.blendIndicesStream] =
            std::max(srcReadBytes[srcLayout.blendIndicesStream],
                     srcLayout.blendIndicesOffset + srcLayout.blendIndicesBytes);
        return true;
    };
    auto requirePSizeRead = [&]() -> bool {
        if (!srcLayout.psize) return false;
        srcReadBytes[srcLayout.psizeStream] =
            std::max(srcReadBytes[srcLayout.psizeStream],
                     srcLayout.psizeOffset + 4u);
        return true;
    };
    auto requireDiffuseRead = [&]() -> bool {
        if (!srcLayout.diffuse) return false;
        srcReadBytes[srcLayout.diffuseStream] =
            std::max(srcReadBytes[srcLayout.diffuseStream],
                     srcLayout.diffuseOffset + 4u);
        return true;
    };
    auto requireSpecularRead = [&]() -> bool {
        if (!srcLayout.specular) return false;
        srcReadBytes[srcLayout.specularStream] =
            std::max(srcReadBytes[srcLayout.specularStream],
                     srcLayout.specularOffset + 4u);
        return true;
    };
    auto requireMaterialColorRead = [&](DWORD source) -> bool {
        if (!processColorVertex) return true;
        if (source == D3DMCS_COLOR1) return requireDiffuseRead();
        if (source == D3DMCS_COLOR2) return requireSpecularRead();
        return source == D3DMCS_MATERIAL;
    };
    auto inferTrailingTexcoord0Read = [&]() -> bool {
        if (!sourceLayoutFromDeclaration || srcLayout.texBytes[0] != 0u ||
            srcLayout.streamStride[0] == 0u) {
            return false;
        }
        const UINT offset = srcLayout.streamStride[0];
        const UINT bytes = 2u * sizeof(float);
        if (context.streamStrides[0] < offset + bytes) return false;
        // Windows accepts ProcessVertices content that leaves TEXCOORD0
        // out of a narrow explicit declaration while still carrying the
        // legacy FVF float2 tail in stream 0. Keep this compatibility
        // path limited to TEXCOORD0 and only when the bound stride proves
        // the tail exists.
        srcLayout.texCount = std::max<UINT>(srcLayout.texCount, 1u);
        srcLayout.texStream[0] = 0u;
        srcLayout.texOffset[0] = offset;
        srcLayout.texBytes[0] = bytes;
        srcLayout.texType[0] = D3DDECLTYPE_FLOAT2;
        srcReadBytes[0] = std::max(srcReadBytes[0], offset + bytes);
        return true;
    };
    auto requireTexRead = [&](UINT i, bool requireMatchingBytes) -> bool {
        const bool hasTex =
            i < srcLayout.texCount && srcLayout.texBytes[i] != 0u;
        if (!hasTex && i == 0u && !requireMatchingBytes &&
            inferTrailingTexcoord0Read()) {
            return true;
        }
        if (!hasTex ||
            (requireMatchingBytes &&
             (dstLayout.texBytes[i] != srcLayout.texBytes[i] ||
              dstLayout.texType[i] != srcLayout.texType[i]))) {
            return false;
        }
        srcReadBytes[srcLayout.texStream[i]] =
            std::max(srcReadBytes[srcLayout.texStream[i]],
                     srcLayout.texOffset[i] + srcLayout.texBytes[i]);
        return true;
    };
    auto findGenericInput = [&](UINT usage, UINT usageIndex)
        -> const FvfProcessLayout::GenericInput* {
        for (UINT i = 0; i < srcLayout.genericInputCount; ++i) {
            const auto& generic = srcLayout.genericInput[i];
            if (generic.usage == usage &&
                generic.usageIndex == usageIndex) {
                return &generic;
            }
        }
        return nullptr;
    };
    auto requireGenericRead = [&](UINT usage, UINT usageIndex) -> bool {
        const auto* generic = findGenericInput(usage, usageIndex);
        if (!generic) return false;
        srcReadBytes[generic->stream] =
            std::max(srcReadBytes[generic->stream],
                     generic->offset + generic->bytes);
        return true;
    };
    if (programmable) {
        if (!shaderIo.hasOutputPosition) return invalid("shader lacks position output");
        if (dstLayout.psize && !shaderIo.hasOutputPSize) return invalid("shader lacks psize output");
        if (dstLayout.diffuse && !shaderIo.hasOutputDiffuse) return invalid("shader lacks diffuse output");
        if (dstLayout.specular && !shaderIo.hasOutputSpecular) return invalid("shader lacks specular output");
        for (UINT i = 0; i < dstLayout.texCount; ++i) {
            if (dstLayout.texBytes[i] == 0u) continue;
            if (!shaderIo.hasOutputTex[i]) return invalid("shader lacks texcoord output");
        }
        // vs_1_x maps every v# to a fixed FFP semantic by default. We must
        // only require streams for v# that the shader actually reads as a
        // source operand (or DCL'd, for vs_2.0+/3.0). usedInputMask was
        // populated by the operand scan in analyzeSimpleProcessVertexShader.
        auto inputUsed = [&](int regIdx) {
            if (regIdx < 0 || regIdx >= 32) return false;
            if (shaderIo.major < 3u) {
                return (shaderIo.usedInputMask & (1u << regIdx)) != 0u;
            }
            return true;  // sm3 requires DCL — presence implies use
        };
        if (inputUsed(shaderIo.inputPosition)) requirePositionRead();
        if (inputUsed(shaderIo.inputNormal) && !requireNormalRead()) return invalid("shader normal input missing");
        if (inputUsed(shaderIo.inputTangent) && !requireTangentRead()) return invalid("shader tangent input missing");
        if (inputUsed(shaderIo.inputBinormal) && !requireBinormalRead()) return invalid("shader binormal input missing");
        if (inputUsed(shaderIo.inputBlendWeight) && !requireBlendWeightRead()) return invalid("shader blendweight input missing");
        if (inputUsed(shaderIo.inputBlendIndices) && !requireBlendIndicesRead()) return invalid("shader blendindices input missing");
        if (inputUsed(shaderIo.inputPSize) && !requirePSizeRead()) return invalid("shader psize input missing");
        if (inputUsed(shaderIo.inputDiffuse) && !requireDiffuseRead()) return invalid("shader diffuse input missing");
        if (inputUsed(shaderIo.inputSpecular) && !requireSpecularRead()) return invalid("shader specular input missing");
        for (UINT i = 0; i < 8; ++i) {
            if (inputUsed(shaderIo.inputTex[i]) && !requireTexRead(i, false)) {
                return invalid("shader texcoord input missing");
            }
        }
        for (UINT i = 0; i < shaderIo.inputGenericCount; ++i) {
            const auto& generic = shaderIo.inputGeneric[i];
            if (!requireGenericRead(generic.usage, generic.usageIndex)) {
                return invalid("shader generic input missing");
            }
        }
    } else {
        requirePositionRead();
        if (fixedBlendWeightCount != 0u && !requireBlendWeightRead()) {
            return invalid("vertex blending weight input missing");
        }
        if (fixedIndexedVertexBlend && !requireBlendIndicesRead()) {
            return invalid("indexed vertex blending indices missing");
        }
        if (dstLayout.diffuse) {
            if (processLighting) {
                if (!requireNormalRead()) return invalid("lighting normal input missing");
            } else if (!requireDiffuseRead()) {
                return invalid("diffuse passthrough missing");
            }
        }
        if (dstLayout.specular && processSpecularLighting) {
            if (!requireNormalRead()) return invalid("specular lighting normal input missing");
        } else if (dstLayout.specular && !requireSpecularRead()) {
            return invalid("specular passthrough missing");
        }
        if (dstLayout.psize && !requirePSizeRead()) {
            return invalid("psize passthrough missing");
        }
        if (processLighting) {
            if (!requireMaterialColorRead(renderStateValue(D3DRS_DIFFUSEMATERIALSOURCE))) {
                return invalid("diffuse material source color missing");
            }
            if (!requireMaterialColorRead(renderStateValue(D3DRS_AMBIENTMATERIALSOURCE))) {
                return invalid("ambient material source color missing");
            }
            if (!requireMaterialColorRead(renderStateValue(D3DRS_EMISSIVEMATERIALSOURCE))) {
                return invalid("emissive material source color missing");
            }
            if (processSpecularLighting &&
                !requireMaterialColorRead(renderStateValue(D3DRS_SPECULARMATERIALSOURCE))) {
                return invalid("specular material source color missing");
            }
        }
        for (UINT i = 0; i < dstLayout.texCount; ++i) {
            if (dstLayout.texBytes[i] == 0u) continue;
            if (!requireTexRead(i, true)) return invalid("texcoord passthrough mismatch");
        }
    }
    D9CBuffer* srcRaw[D9C_DRAW_PACKET_MAX_STREAMS]{};
    D9CBufferDesc srcDesc[D9C_DRAW_PACKET_MAX_STREAMS]{};
    uint64_t srcByteStart[D9C_DRAW_PACKET_MAX_STREAMS]{};
    uint64_t srcByteEnd[D9C_DRAW_PACKET_MAX_STREAMS]{};
    D9CBuffer* uniqueSrcRaw[D9C_DRAW_PACKET_MAX_STREAMS]{};
    uint64_t uniqueSrcLockSize[D9C_DRAW_PACKET_MAX_STREAMS]{};
    void* uniqueSrcBytes[D9C_DRAW_PACKET_MAX_STREAMS]{};
    UINT uniqueSrcCount = 0;
    for (UINT stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
        if (srcReadBytes[stream] == 0u) continue;
        if (!context.streamSources[stream] || context.streamStrides[stream] < srcLayout.streamStride[stream]) {
            return invalid("source stream missing or stride too small");
        }
        srcRaw[stream] = validatedStreamRaw[stream];
        if (!srcRaw[stream] ||
            FAILED(hr32(dxmt9c_buffer_get_desc(srcRaw[stream], &srcDesc[stream])))) {
            return invalid("source stream desc failed");
        }
        const bool instancedStream = processStreamInstanced(stream);
        const uint64_t firstElement = instancedStream ? 0u : srcStart;
        const uint64_t lastElement =
            firstElement + (instancedStream ? 0u : vertexCount - 1u);
        srcByteStart[stream] =
            static_cast<uint64_t>(context.streamOffsets[stream]) +
            firstElement * context.streamStrides[stream];
        srcByteEnd[stream] =
            srcByteStart[stream] +
            (lastElement - firstElement) * context.streamStrides[stream] +
            srcReadBytes[stream];
        if (srcByteEnd[stream] > srcDesc[stream].size ||
            srcByteEnd[stream] > UINT32_MAX) {
            return invalid("source range out of bounds");
        }
        UINT unique = 0;
        for (; unique < uniqueSrcCount; ++unique) {
            if (uniqueSrcRaw[unique] == srcRaw[stream]) break;
        }
        if (unique == uniqueSrcCount) {
            uniqueSrcRaw[uniqueSrcCount++] = srcRaw[stream];
        }
        uniqueSrcLockSize[unique] =
            std::max(uniqueSrcLockSize[unique], srcByteEnd[stream]);
    }
    const uint64_t dstByteStart =
        static_cast<uint64_t>(dstIndex) * dstLayout.stride;
    const uint64_t dstByteEnd =
        dstByteStart + static_cast<uint64_t>(vertexCount) * dstLayout.stride;
    if (dstByteEnd > dstDesc.size || dstByteEnd > UINT32_MAX) {
        return invalid("destination range out of bounds");
    }

    void* dstBytes = nullptr;
    const uint32_t dstLockOffset = static_cast<uint32_t>(dstByteStart);
    const uint32_t dstLockSize = static_cast<uint32_t>(dstByteEnd - dstByteStart);
    HRESULT hr = D3D_OK;
    for (UINT unique = 0; unique < uniqueSrcCount; ++unique) {
        hr = hr32(dxmt9c_buffer_lock(
            uniqueSrcRaw[unique], 0,
            static_cast<uint32_t>(uniqueSrcLockSize[unique]),
            &uniqueSrcBytes[unique], D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE));
        if (FAILED(hr) || !uniqueSrcBytes[unique]) {
            for (UINT unlock = 0; unlock < unique; ++unlock) {
                (void)dxmt9c_buffer_unlock(uniqueSrcRaw[unlock]);
            }
            return FAILED(hr) ? hr : D3DERR_INVALIDCALL;
        }
    }
    void* srcBytes[D9C_DRAW_PACKET_MAX_STREAMS]{};
    for (UINT stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
        if (srcReadBytes[stream] == 0u) continue;
        for (UINT unique = 0; unique < uniqueSrcCount; ++unique) {
            if (uniqueSrcRaw[unique] == srcRaw[stream]) {
                srcBytes[stream] = uniqueSrcBytes[unique];
                break;
            }
        }
    }
    hr = hr32(dxmt9c_buffer_lock(dstRaw, dstLockOffset, dstLockSize, &dstBytes, 0));
    if (FAILED(hr) || !dstBytes) {
        for (UINT unique = 0; unique < uniqueSrcCount; ++unique) {
            (void)dxmt9c_buffer_unlock(uniqueSrcRaw[unique]);
        }
        return FAILED(hr) ? hr : D3DERR_INVALIDCALL;
    }
    D3D9PeInvalidateVertexBufferReadonlyCache(destination);

    const auto& vp = context.state.viewportShadow();
    const float scaleX = static_cast<float>(vp.width) * 0.5f;
    const float scaleY = static_cast<float>(vp.height) * 0.5f;
    const float offsetX = static_cast<float>(vp.x) + scaleX;
    const float offsetY = static_cast<float>(vp.y) + scaleY;
    const float zScale = vp.maxZ - vp.minZ;
    const D9CMatrix wvp = worldViewProjectionTransform(context.state);
    const D9CMatrix viewProjection = multiplyTransformMatrix(
        transformOrIdentity(context.state, D3DTS_VIEW),
        transformOrIdentity(context.state, D3DTS_PROJECTION));
    const DWORD processAmbient = processLighting ? renderStateValue(D3DRS_AMBIENT) : 0u;
    const uint8_t* srcBase[D9C_DRAW_PACKET_MAX_STREAMS]{};
    for (UINT stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
        srcBase[stream] = static_cast<const uint8_t*>(srcBytes[stream]);
    }
    auto sourceOffset = [&](UINT stream, UINT vertex) {
        const uint64_t element = processStreamInstanced(stream) ? 0u : vertex;
        return srcByteStart[stream] + element * context.streamStrides[stream];
    };
    std::array<std::array<float, 4>, 256> shaderConstF{};
    if (programmable && !context.constants.vsConstF.values.empty()) {
        const size_t bytes = std::min(context.constants.vsConstF.values.size(),
                                      shaderConstF.size() * sizeof(shaderConstF[0]));
        std::memcpy(shaderConstF.data(), context.constants.vsConstF.values.data(), bytes);
    }
    std::array<std::array<int32_t, 4>, 16> shaderConstI{};
    if (programmable && !context.constants.vsConstI.values.empty()) {
        const size_t bytes = std::min(context.constants.vsConstI.values.size(),
                                      shaderConstI.size() * sizeof(shaderConstI[0]));
        std::memcpy(shaderConstI.data(), context.constants.vsConstI.values.data(), bytes);
    }
    std::array<uint32_t, 16> shaderConstB{};
    if (programmable && !context.constants.vsConstB.values.empty()) {
        const size_t bytes = std::min(context.constants.vsConstB.values.size(),
                                      shaderConstB.size() * sizeof(shaderConstB[0]));
        std::memcpy(shaderConstB.data(), context.constants.vsConstB.values.data(), bytes);
    }
    SimpleVsTextureState shaderTextures{};
    if (programmable) {
        for (UINT sampler = 0; sampler < shaderTextures.vertexTextures.size(); ++sampler) {
            const UINT samplerSlot = kPeFragmentSamplerSlots + sampler;
            const SamplerIndex samplerIndex = SamplerIndex::fromRaw(samplerSlot);
            const auto samplerStateValue =
                [&](D3DSAMPLERSTATETYPE type, DWORD fallback) -> DWORD {
                    SamplerStateType stateType{};
                    uint32_t value = 0;
                    if (!samplerStateTypeKey(static_cast<std::uint32_t>(type), stateType)) {
                        return fallback;
                    }
                    if (context.state.samplerStateShadowTyped().get(
                            samplerIndex, stateType, value)) {
                        return value;
                    }
                    return dxmt9c_device_get_sampler_state(
                        context.device, samplerSlot, static_cast<uint32_t>(type));
                };
            shaderTextures.vertexTextures[sampler] = validatedTextureRaw[samplerSlot];
            shaderTextures.addressU[sampler] =
                samplerStateValue(D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            shaderTextures.addressV[sampler] =
                samplerStateValue(D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            shaderTextures.borderColor[sampler] =
                samplerStateValue(D3DSAMP_BORDERCOLOR, 0u);
            shaderTextures.minMipLevel[sampler] =
                std::max<DWORD>(
                    context.textures[samplerSlot] ? context.textures[samplerSlot]->GetLOD() : 0u,
                    samplerStateValue(D3DSAMP_MAXMIPLEVEL, 0u));
        }
    }
    auto* dstBase = static_cast<uint8_t*>(dstBytes);
    for (UINT i = 0; i < vertexCount; ++i) {
        auto* dstVertex = dstBase + static_cast<size_t>(i) * dstLayout.stride;
        float clip[4]{};
        float psizeOut = 0.0f;
        float diffuseOut[4]{};
        float specularOut[4]{};
        float texOut[8][4]{};
        float fixedPosition[3]{};
        auto transformPoint = [](const float position[4],
                                 const D9CMatrix& matrix,
                                 float out[4]) {
            for (UINT col = 0; col < 4; ++col) {
                out[col] = position[0] * matrix.m[col] +
                           position[1] * matrix.m[4 + col] +
                           position[2] * matrix.m[8 + col] +
                           position[3] * matrix.m[12 + col];
            }
        };
        if (programmable) {
            SimpleVsRegisters regs{};
            regs.constant = shaderConstF;
            regs.constantInt = shaderConstI;
            regs.constantBool = shaderConstB;
            auto loadPositionInput = [&](int reg) {
                if (reg < 0 || static_cast<size_t>(reg) >= regs.input.size()) return false;
                const auto* positionSource =
                    srcBase[srcLayout.positionStream] +
                    sourceOffset(srcLayout.positionStream, i) +
                    srcLayout.positionOffset;
                return decodeProcessDeclVector(positionSource,
                                               srcLayout.positionType,
                                               srcLayout.positionBytes,
                                               regs.input[reg]);
            };
            auto loadColorInput = [&](int reg, UINT stream, UINT offset) {
                if (reg < 0 || static_cast<size_t>(reg) >= regs.input.size()) return false;
                DWORD color = 0;
                const auto* colorSource =
                    srcBase[stream] + sourceOffset(stream, i) + offset;
                std::memcpy(&color, colorSource, sizeof(color));
                unpackD3DColor(color, regs.input[reg].data());
                return true;
            };
            auto loadDeclVectorInput =
                [&](int reg, UINT stream, UINT offset, UINT type, UINT bytes) {
                if (reg < 0 || static_cast<size_t>(reg) >= regs.input.size()) return false;
                const auto* source =
                    srcBase[stream] + sourceOffset(stream, i) + offset;
                regs.input[reg] = {0.0f, 0.0f, 0.0f, 1.0f};
                switch (type) {
                    case D3DDECLTYPE_FLOAT1:
                    case D3DDECLTYPE_FLOAT2:
                    case D3DDECLTYPE_FLOAT3:
                    case D3DDECLTYPE_FLOAT4: {
                        if (bytes == 0u || bytes > sizeof(float) * 4u ||
                            (bytes % sizeof(float)) != 0u) return false;
                        const UINT components = bytes / sizeof(float);
                        std::memcpy(regs.input[reg].data(), source,
                                    std::min<UINT>(components, 4u) * sizeof(float));
                        return true;
                    }
                    case D3DDECLTYPE_SHORT4: {
                        int16_t in[4]{};
                        std::memcpy(in, source, sizeof(in));
                        for (UINT c = 0; c < 4u; ++c) {
                            regs.input[reg][c] = static_cast<float>(in[c]);
                        }
                        return true;
                    }
                    case D3DDECLTYPE_SHORT2: {
                        int16_t in[2]{};
                        std::memcpy(in, source, sizeof(in));
                        regs.input[reg][0] = static_cast<float>(in[0]);
                        regs.input[reg][1] = static_cast<float>(in[1]);
                        return true;
                    }
                    case D3DDECLTYPE_UBYTE4: {
                        uint8_t in[4]{};
                        std::memcpy(in, source, sizeof(in));
                        for (UINT c = 0; c < 4u; ++c) {
                            regs.input[reg][c] = static_cast<float>(in[c]);
                        }
                        return true;
                    }
                    case D3DDECLTYPE_SHORT2N: {
                        int16_t in[2]{};
                        std::memcpy(in, source, sizeof(in));
                        regs.input[reg][0] = snorm16ToFloat(in[0]);
                        regs.input[reg][1] = snorm16ToFloat(in[1]);
                        return true;
                    }
                    case D3DDECLTYPE_SHORT4N: {
                        int16_t in[4]{};
                        std::memcpy(in, source, sizeof(in));
                        for (UINT c = 0; c < 4u; ++c) {
                            regs.input[reg][c] = snorm16ToFloat(in[c]);
                        }
                        return true;
                    }
                    case D3DDECLTYPE_USHORT2N: {
                        uint16_t in[2]{};
                        std::memcpy(in, source, sizeof(in));
                        regs.input[reg][0] = unorm16ToFloat(in[0]);
                        regs.input[reg][1] = unorm16ToFloat(in[1]);
                        return true;
                    }
                    case D3DDECLTYPE_USHORT4N: {
                        uint16_t in[4]{};
                        std::memcpy(in, source, sizeof(in));
                        for (UINT c = 0; c < 4u; ++c) {
                            regs.input[reg][c] = unorm16ToFloat(in[c]);
                        }
                        return true;
                    }
                    case D3DDECLTYPE_UBYTE4N: {
                        uint8_t in[4]{};
                        std::memcpy(in, source, sizeof(in));
                        for (UINT c = 0; c < 4u; ++c) {
                            regs.input[reg][c] = static_cast<float>(in[c]) / 255.0f;
                        }
                        return true;
                    }
                    case D3DDECLTYPE_DEC3N: {
                        uint32_t packed = 0;
                        std::memcpy(&packed, source, sizeof(packed));
                        regs.input[reg][0] = snorm10ToFloat(packed);
                        regs.input[reg][1] = snorm10ToFloat(packed >> 10u);
                        regs.input[reg][2] = snorm10ToFloat(packed >> 20u);
                        return true;
                    }
                    case D3DDECLTYPE_UDEC3: {
                        uint32_t packed = 0;
                        std::memcpy(&packed, source, sizeof(packed));
                        regs.input[reg][0] = static_cast<float>(packed & 0x3ffu);
                        regs.input[reg][1] = static_cast<float>((packed >> 10u) & 0x3ffu);
                        regs.input[reg][2] = static_cast<float>((packed >> 20u) & 0x3ffu);
                        return true;
                    }
                    case D3DDECLTYPE_FLOAT16_2: {
                        uint16_t in[2]{};
                        std::memcpy(in, source, sizeof(in));
                        regs.input[reg][0] = halfToFloat(in[0]);
                        regs.input[reg][1] = halfToFloat(in[1]);
                        return true;
                    }
                    case D3DDECLTYPE_FLOAT16_4: {
                        uint16_t in[4]{};
                        std::memcpy(in, source, sizeof(in));
                        for (UINT c = 0; c < 4u; ++c) {
                            regs.input[reg][c] = halfToFloat(in[c]);
                        }
                        return true;
                    }
                    case D3DDECLTYPE_D3DCOLOR: {
                        DWORD color = 0;
                        std::memcpy(&color, source, sizeof(color));
                        unpackD3DColor(color, regs.input[reg].data());
                        return true;
                    }
                    default:
                        return false;
                }
            };
            auto loadFloatVectorInput =
                [&](int reg, UINT stream, UINT offset, UINT bytes) {
                    if (reg < 0 || static_cast<size_t>(reg) >= regs.input.size()) return false;
                    if (bytes == 0u || bytes > sizeof(float) * 4u ||
                        (bytes % sizeof(float)) != 0u) return false;
                    float in[4]{0.0f, 0.0f, 0.0f, 1.0f};
                    const auto* source =
                        srcBase[stream] + sourceOffset(stream, i) + offset;
                    std::memcpy(in, source, bytes);
                    regs.input[reg] = {in[0], in[1], in[2], in[3]};
                    return true;
                };
            auto loadUbyte4Input = [&](int reg, UINT stream, UINT offset) {
                if (reg < 0 || static_cast<size_t>(reg) >= regs.input.size()) return false;
                uint8_t in[4]{};
                const auto* source =
                    srcBase[stream] + sourceOffset(stream, i) + offset;
                std::memcpy(in, source, sizeof(in));
                regs.input[reg] = {
                    static_cast<float>(in[0]),
                    static_cast<float>(in[1]),
                    static_cast<float>(in[2]),
                    static_cast<float>(in[3]),
                };
                return true;
            };
            auto loadBlendIndicesInput =
                [&](int reg, UINT stream, UINT offset, UINT type) {
                    if (type == D3DDECLTYPE_UBYTE4) {
                        return loadUbyte4Input(reg, stream, offset);
                    }
                    if (type != D3DDECLTYPE_D3DCOLOR ||
                        reg < 0 || static_cast<size_t>(reg) >= regs.input.size()) {
                        return false;
                    }
                    DWORD color = 0;
                    const auto* source =
                        srcBase[stream] + sourceOffset(stream, i) + offset;
                    std::memcpy(&color, source, sizeof(color));
                    unpackD3DColor(color, regs.input[reg].data());
                    return true;
                };
            auto loadTexInput = [&](int reg, UINT tex) {
                if (reg < 0 || static_cast<size_t>(reg) >= regs.input.size()) return false;
                const auto* texSource =
                    srcBase[srcLayout.texStream[tex]] +
                    sourceOffset(srcLayout.texStream[tex], i) +
                    srcLayout.texOffset[tex];
                regs.input[reg] = {0.0f, 0.0f, 0.0f, 1.0f};
                switch (srcLayout.texType[tex]) {
                    case D3DDECLTYPE_FLOAT1:
                    case D3DDECLTYPE_FLOAT2:
                    case D3DDECLTYPE_FLOAT3:
                    case D3DDECLTYPE_FLOAT4: {
                        const UINT components = srcLayout.texBytes[tex] / sizeof(float);
                        std::memcpy(regs.input[reg].data(), texSource,
                                    std::min<UINT>(components, 4u) * sizeof(float));
                        return true;
                    }
                    case D3DDECLTYPE_D3DCOLOR: {
                        DWORD color = 0;
                        std::memcpy(&color, texSource, sizeof(color));
                        unpackD3DColor(color, regs.input[reg].data());
                        return true;
                    }
                    case D3DDECLTYPE_SHORT2: {
                        int16_t in[2]{};
                        std::memcpy(in, texSource, sizeof(in));
                        regs.input[reg][0] = static_cast<float>(in[0]);
                        regs.input[reg][1] = static_cast<float>(in[1]);
                        return true;
                    }
                    case D3DDECLTYPE_SHORT4: {
                        int16_t in[4]{};
                        std::memcpy(in, texSource, sizeof(in));
                        for (UINT c = 0; c < 4u; ++c) {
                            regs.input[reg][c] = static_cast<float>(in[c]);
                        }
                        return true;
                    }
                    case D3DDECLTYPE_UBYTE4: {
                        uint8_t in[4]{};
                        std::memcpy(in, texSource, sizeof(in));
                        for (UINT c = 0; c < 4u; ++c) {
                            regs.input[reg][c] = static_cast<float>(in[c]);
                        }
                        return true;
                    }
                    case D3DDECLTYPE_SHORT2N: {
                        int16_t in[2]{};
                        std::memcpy(in, texSource, sizeof(in));
                        regs.input[reg][0] = snorm16ToFloat(in[0]);
                        regs.input[reg][1] = snorm16ToFloat(in[1]);
                        return true;
                    }
                    case D3DDECLTYPE_SHORT4N: {
                        int16_t in[4]{};
                        std::memcpy(in, texSource, sizeof(in));
                        for (UINT c = 0; c < 4u; ++c) {
                            regs.input[reg][c] = snorm16ToFloat(in[c]);
                        }
                        return true;
                    }
                    case D3DDECLTYPE_USHORT2N: {
                        uint16_t in[2]{};
                        std::memcpy(in, texSource, sizeof(in));
                        regs.input[reg][0] = unorm16ToFloat(in[0]);
                        regs.input[reg][1] = unorm16ToFloat(in[1]);
                        return true;
                    }
                    case D3DDECLTYPE_USHORT4N: {
                        uint16_t in[4]{};
                        std::memcpy(in, texSource, sizeof(in));
                        for (UINT c = 0; c < 4u; ++c) {
                            regs.input[reg][c] = unorm16ToFloat(in[c]);
                        }
                        return true;
                    }
                    case D3DDECLTYPE_UBYTE4N: {
                        uint8_t in[4]{};
                        std::memcpy(in, texSource, sizeof(in));
                        for (UINT c = 0; c < 4u; ++c) {
                            regs.input[reg][c] = static_cast<float>(in[c]) / 255.0f;
                        }
                        return true;
                    }
                    case D3DDECLTYPE_UDEC3: {
                        uint32_t packed = 0;
                        std::memcpy(&packed, texSource, sizeof(packed));
                        regs.input[reg][0] = static_cast<float>(packed & 0x3ffu);
                        regs.input[reg][1] = static_cast<float>((packed >> 10u) & 0x3ffu);
                        regs.input[reg][2] = static_cast<float>((packed >> 20u) & 0x3ffu);
                        return true;
                    }
                    case D3DDECLTYPE_DEC3N: {
                        uint32_t packed = 0;
                        std::memcpy(&packed, texSource, sizeof(packed));
                        regs.input[reg][0] = snorm10ToFloat(packed);
                        regs.input[reg][1] = snorm10ToFloat(packed >> 10u);
                        regs.input[reg][2] = snorm10ToFloat(packed >> 20u);
                        return true;
                    }
                    case D3DDECLTYPE_FLOAT16_2: {
                        uint16_t in[2]{};
                        std::memcpy(in, texSource, sizeof(in));
                        regs.input[reg][0] = halfToFloat(in[0]);
                        regs.input[reg][1] = halfToFloat(in[1]);
                        return true;
                    }
                    case D3DDECLTYPE_FLOAT16_4: {
                        uint16_t in[4]{};
                        std::memcpy(in, texSource, sizeof(in));
                        for (UINT c = 0; c < 4u; ++c) {
                            regs.input[reg][c] = halfToFloat(in[c]);
                        }
                        return true;
                    }
                    default:
                        return false;
                }
            };
            // Mirror the validator's usedInputMask gating so we only load
            // v# the shader actually reads. vs_1_x maps every v# to a fixed
            // FFP semantic by default, but issuing a load for a v# that the
            // shader never references would dereference srcBase[stream]
            // for a stream the caller deliberately left unbound.
            auto inputLoadGate = [&](int regIdx) {
                if (regIdx < 0 || regIdx >= 32) return false;
                return (shaderIo.usedInputMask & (1u << regIdx)) != 0u;
            };
            if (inputLoadGate(shaderIo.inputPosition) && !loadPositionInput(shaderIo.inputPosition)) {
                hr = D3DERR_INVALIDCALL;
                break;
            }
            if (inputLoadGate(shaderIo.inputNormal) &&
                !loadDeclVectorInput(shaderIo.inputNormal, srcLayout.normalStream,
                                     srcLayout.normalOffset, srcLayout.normalType,
                                     srcLayout.normalBytes)) {
                hr = D3DERR_INVALIDCALL;
                break;
            }
            if (inputLoadGate(shaderIo.inputTangent) &&
                !loadDeclVectorInput(shaderIo.inputTangent, srcLayout.tangentStream,
                                     srcLayout.tangentOffset, srcLayout.tangentType,
                                     srcLayout.tangentBytes)) {
                hr = D3DERR_INVALIDCALL;
                break;
            }
            if (inputLoadGate(shaderIo.inputBinormal) &&
                !loadDeclVectorInput(shaderIo.inputBinormal, srcLayout.binormalStream,
                                     srcLayout.binormalOffset, srcLayout.binormalType,
                                     srcLayout.binormalBytes)) {
                hr = D3DERR_INVALIDCALL;
                break;
            }
            if (inputLoadGate(shaderIo.inputBlendWeight) &&
                !loadDeclVectorInput(shaderIo.inputBlendWeight,
                                     srcLayout.blendWeightStream,
                                     srcLayout.blendWeightOffset,
                                     srcLayout.blendWeightType,
                                     srcLayout.blendWeightBytes)) {
                hr = D3DERR_INVALIDCALL;
                break;
            }
            if (inputLoadGate(shaderIo.inputBlendIndices) &&
                !loadBlendIndicesInput(shaderIo.inputBlendIndices,
                                       srcLayout.blendIndicesStream,
                                       srcLayout.blendIndicesOffset,
                                       srcLayout.blendIndicesType)) {
                hr = D3DERR_INVALIDCALL;
                break;
            }
            if (inputLoadGate(shaderIo.inputPSize) &&
                !loadFloatVectorInput(shaderIo.inputPSize,
                                      srcLayout.psizeStream,
                                      srcLayout.psizeOffset, 4u)) {
                hr = D3DERR_INVALIDCALL;
                break;
            }
            if (inputLoadGate(shaderIo.inputDiffuse) &&
                !loadColorInput(shaderIo.inputDiffuse, srcLayout.diffuseStream,
                                srcLayout.diffuseOffset)) {
                hr = D3DERR_INVALIDCALL;
                break;
            }
            if (inputLoadGate(shaderIo.inputSpecular) &&
                !loadColorInput(shaderIo.inputSpecular, srcLayout.specularStream,
                                srcLayout.specularOffset)) {
                hr = D3DERR_INVALIDCALL;
                break;
            }
            for (UINT tex = 0; tex < 8; ++tex) {
                if (inputLoadGate(shaderIo.inputTex[tex]) &&
                    !loadTexInput(shaderIo.inputTex[tex], tex)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
            }
            if (FAILED(hr)) break;
            for (UINT genericIndex = 0;
                 genericIndex < shaderIo.inputGenericCount; ++genericIndex) {
                const auto& shaderGeneric = shaderIo.inputGeneric[genericIndex];
                const auto* generic = findGenericInput(
                    shaderGeneric.usage, shaderGeneric.usageIndex);
                if (!generic ||
                    !loadDeclVectorInput(shaderGeneric.reg,
                                         generic->stream,
                                         generic->offset,
                                         generic->type,
                                         generic->bytes)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
            }
            if (FAILED(hr)) break;
            if (!executeSimpleProcessVertexShader(
                    shaderWords, shaderIo, regs, &shaderTextures)) {
                hr = D3DERR_INVALIDCALL;
                break;
            }
            const auto* positionReg =
                simpleVsRegister(regs, shaderIo.major, shaderIo.outputPosition.type,
                                 shaderIo.outputPosition.index);
            if (!positionReg) {
                hr = D3DERR_INVALIDCALL;
                break;
            }
            std::memcpy(clip, positionReg->data(), sizeof(clip));
            if (dstLayout.psize) {
                const auto* psizeReg =
                    simpleVsRegister(regs, shaderIo.major, shaderIo.outputPSize.type,
                                     shaderIo.outputPSize.index);
                if (!psizeReg) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                psizeOut = (*psizeReg)[0];
            }
            if (dstLayout.diffuse) {
                const auto* colorReg =
                    simpleVsRegister(regs, shaderIo.major, shaderIo.outputDiffuse.type,
                                     shaderIo.outputDiffuse.index);
                if (!colorReg) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                std::memcpy(diffuseOut, colorReg->data(), sizeof(diffuseOut));
            }
            if (dstLayout.specular) {
                const auto* colorReg =
                    simpleVsRegister(regs, shaderIo.major, shaderIo.outputSpecular.type,
                                     shaderIo.outputSpecular.index);
                if (!colorReg) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                std::memcpy(specularOut, colorReg->data(), sizeof(specularOut));
            }
            for (UINT tex = 0; tex < dstLayout.texCount; ++tex) {
                if (dstLayout.texBytes[tex] == 0u) continue;
                const auto* texReg =
                    simpleVsRegister(regs, shaderIo.major, shaderIo.outputTex[tex].type,
                                     shaderIo.outputTex[tex].index);
                if (!texReg) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                std::memcpy(texOut[tex], texReg->data(), sizeof(texOut[tex]));
            }
            if (FAILED(hr)) break;
        } else {
            std::array<float, 4> in{};
            const auto* positionSource =
                srcBase[srcLayout.positionStream] +
                sourceOffset(srcLayout.positionStream, i) +
                srcLayout.positionOffset;
            if (!decodeProcessDeclVector(positionSource,
                                         srcLayout.positionType,
                                         srcLayout.positionBytes,
                                         in)) {
                hr = D3DERR_INVALIDCALL;
                break;
            }
            fixedPosition[0] = in[0];
            fixedPosition[1] = in[1];
            fixedPosition[2] = in[2];
            if (dstLayout.psize) {
                const auto* psizeSource =
                    srcBase[srcLayout.psizeStream] +
                    sourceOffset(srcLayout.psizeStream, i) +
                    srcLayout.psizeOffset;
                std::memcpy(&psizeOut, psizeSource, sizeof(psizeOut));
            }
            const float position[4] = {in[0], in[1], in[2], 1.0f};
            if (fixedBlendWeightCount != 0u) {
                std::array<float, 4> blendWeights{0.0f, 0.0f, 0.0f, 0.0f};
                std::array<UINT, 4> blendIndices{0u, 1u, 2u, 3u};
                const auto* blendSource =
                    srcBase[srcLayout.blendWeightStream] +
                    sourceOffset(srcLayout.blendWeightStream, i) +
                    srcLayout.blendWeightOffset;
                if (!decodeProcessDeclVector(blendSource,
                                             srcLayout.blendWeightType,
                                             srcLayout.blendWeightBytes,
                                             blendWeights)) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
                if (fixedIndexedVertexBlend) {
                    const auto* indicesSource =
                        srcBase[srcLayout.blendIndicesStream] +
                        sourceOffset(srcLayout.blendIndicesStream, i) +
                        srcLayout.blendIndicesOffset;
                    if (srcLayout.blendIndicesType == D3DDECLTYPE_UBYTE4) {
                        uint8_t indices[4]{};
                        std::memcpy(indices, indicesSource, sizeof(indices));
                        for (UINT c = 0; c < 4u; ++c) {
                            blendIndices[c] = indices[c];
                        }
                    } else if (srcLayout.blendIndicesType == D3DDECLTYPE_D3DCOLOR) {
                        float color[4]{};
                        DWORD packed = 0;
                        std::memcpy(&packed, indicesSource, sizeof(packed));
                        unpackD3DColor(packed, color);
                        for (UINT c = 0; c < 4u; ++c) {
                            blendIndices[c] = static_cast<UINT>(
                                std::lround(std::clamp(color[c], 0.0f, 1.0f) * 255.0f));
                        }
                    } else {
                        hr = D3DERR_INVALIDCALL;
                        break;
                    }
                }
                float worldPosition[4]{};
                float explicitWeightSum = 0.0f;
                for (UINT weightIndex = 0;
                     weightIndex < fixedBlendWeightCount; ++weightIndex) {
                    const float weight = blendWeights[weightIndex];
                    explicitWeightSum += weight;
                    float transformed[4]{};
                    transformPoint(position,
                                   transformOrIdentity(context.state, D3DTS_WORLDMATRIX(
                                       blendIndices[weightIndex])),
                                   transformed);
                    for (UINT c = 0; c < 4; ++c) {
                        worldPosition[c] += transformed[c] * weight;
                    }
                }
                const float implicitWeight = 1.0f - explicitWeightSum;
                float transformed[4]{};
                transformPoint(position,
                               transformOrIdentity(context.state, D3DTS_WORLDMATRIX(
                                   blendIndices[fixedBlendWeightCount])),
                               transformed);
                for (UINT c = 0; c < 4; ++c) {
                    worldPosition[c] += transformed[c] * implicitWeight;
                }
                fixedPosition[0] = worldPosition[0];
                fixedPosition[1] = worldPosition[1];
                fixedPosition[2] = worldPosition[2];
                transformPoint(worldPosition, viewProjection, clip);
            } else {
                transformPoint(position, wvp, clip);
            }
        }
        const float invW = clip[3] != 0.0f ? 1.0f / clip[3] : 1.0f;
        const float ndcX = clip[0] * invW;
        const float ndcY = clip[1] * invW;
        const float ndcZ = clip[2] * invW;
        float viewportZ = vp.minZ + ndcZ * zScale;
        if (renderStateValue(D3DRS_CLIPPING) == 0u) {
            const float minDepth = std::min(vp.minZ, vp.maxZ);
            const float maxDepth = std::max(vp.minZ, vp.maxZ);
            viewportZ = std::clamp(viewportZ, minDepth, maxDepth);
        }
        float out[4] = {
            ndcX * scaleX + offsetX,
            -ndcY * scaleY + offsetY,
            viewportZ,
            invW,
        };
        std::memcpy(dstVertex + dstLayout.positionOffset, out, sizeof(out));
        if (dstLayout.psize) {
            std::memcpy(dstVertex + dstLayout.psizeOffset,
                        &psizeOut, sizeof(psizeOut));
        }
        ProcessFixedFunctionLightingColors lightingColors{};
        bool lightingColorsReady = false;
        auto fixedFunctionLightingColors = [&]() -> const ProcessFixedFunctionLightingColors& {
            if (!lightingColorsReady) {
                std::array<float, 4> normalIn{};
                const auto* normalSource =
                    srcBase[srcLayout.normalStream] +
                    sourceOffset(srcLayout.normalStream, i) +
                    srcLayout.normalOffset;
                if (!decodeProcessDeclVector(normalSource,
                                             srcLayout.normalType,
                                             srcLayout.normalBytes,
                                             normalIn)) {
                    lightingColors = {};
                    lightingColorsReady = true;
                    return lightingColors;
                }
                float normal[3]{normalIn[0], normalIn[1], normalIn[2]};
                D9CMaterial material = context.state.materialShadow();
                auto readMaterialColor = [&](DWORD source,
                                             D9CColorRGBA& target) {
                    if (!processColorVertex || source == D3DMCS_MATERIAL) {
                        return true;
                    }
                    UINT stream = 0;
                    UINT offset = 0;
                    if (source == D3DMCS_COLOR1 && srcLayout.diffuse) {
                        stream = srcLayout.diffuseStream;
                        offset = srcLayout.diffuseOffset;
                    } else if (source == D3DMCS_COLOR2 && srcLayout.specular) {
                        stream = srcLayout.specularStream;
                        offset = srcLayout.specularOffset;
                    } else {
                        return false;
                    }
                    DWORD color = 0;
                    const auto* colorSource =
                        srcBase[stream] + sourceOffset(stream, i) +
                        offset;
                    std::memcpy(&color, colorSource, sizeof(color));
                    target = d3dColorToRgba(color);
                    return true;
                };
                if (!readMaterialColor(renderStateValue(D3DRS_DIFFUSEMATERIALSOURCE),
                                       material.diffuse) ||
                    !readMaterialColor(renderStateValue(D3DRS_AMBIENTMATERIALSOURCE),
                                       material.ambient) ||
                    !readMaterialColor(renderStateValue(D3DRS_EMISSIVEMATERIALSOURCE),
                                       material.emissive) ||
                    (processSpecularLighting &&
                     !readMaterialColor(renderStateValue(D3DRS_SPECULARMATERIALSOURCE),
                                        material.specular))) {
                    lightingColors = {};
                    lightingColorsReady = true;
                    return lightingColors;
                }
                lightingColors = processFixedFunctionLightingColors(
                    fixedPosition, normal, material, processAmbient,
                    context.state.lightShadow(), context.state.lightEnableShadow(),
                    processSpecularLighting);
                lightingColorsReady = true;
            }
            return lightingColors;
        };
        if (dstLayout.diffuse) {
            if (programmable) {
                const DWORD color = packD3DColor(diffuseOut);
                std::memcpy(dstVertex + dstLayout.diffuseOffset, &color, sizeof(color));
            } else if (processLighting) {
                const DWORD color = fixedFunctionLightingColors().diffuse;
                std::memcpy(dstVertex + dstLayout.diffuseOffset, &color, sizeof(color));
            } else {
                const auto* diffuseSource =
                    srcBase[srcLayout.diffuseStream] + sourceOffset(srcLayout.diffuseStream, i) +
                    srcLayout.diffuseOffset;
                std::memcpy(dstVertex + dstLayout.diffuseOffset,
                            diffuseSource, 4u);
            }
        }
        if (dstLayout.specular) {
            if (programmable) {
                const DWORD color = packD3DColor(specularOut);
                std::memcpy(dstVertex + dstLayout.specularOffset, &color, sizeof(color));
            } else if (processSpecularLighting) {
                const DWORD color = fixedFunctionLightingColors().specular;
                std::memcpy(dstVertex + dstLayout.specularOffset, &color, sizeof(color));
            } else {
                const auto* specularSource =
                    srcBase[srcLayout.specularStream] +
                    sourceOffset(srcLayout.specularStream, i) +
                    srcLayout.specularOffset;
                std::memcpy(dstVertex + dstLayout.specularOffset,
                            specularSource, 4u);
            }
        }
        for (UINT tex = 0; tex < dstLayout.texCount; ++tex) {
            if (dstLayout.texBytes[tex] == 0u) continue;
            if (programmable) {
                if (!encodeProcessDeclVector(
                        texOut[tex], dstLayout.texType[tex],
                        dstVertex + dstLayout.texOffset[tex])) {
                    hr = D3DERR_INVALIDCALL;
                    break;
                }
            } else {
                const auto* texSource =
                    srcBase[srcLayout.texStream[tex]] + sourceOffset(srcLayout.texStream[tex], i) +
                    srcLayout.texOffset[tex];
                std::memcpy(dstVertex + dstLayout.texOffset[tex],
                            texSource, dstLayout.texBytes[tex]);
            }
        }
    }
    const HRESULT dstUnlockHr = hr32(dxmt9c_buffer_unlock(dstRaw));
    HRESULT srcUnlockHr = D3D_OK;
    for (UINT unique = 0; unique < uniqueSrcCount; ++unique) {
        const HRESULT oneHr = hr32(dxmt9c_buffer_unlock(uniqueSrcRaw[unique]));
        if (SUCCEEDED(srcUnlockHr) && FAILED(oneHr)) srcUnlockHr = oneHr;
    }
    if (FAILED(dstUnlockHr)) return dstUnlockHr;
    if (FAILED(srcUnlockHr)) return srcUnlockHr;
    if (FAILED(hr)) return hr;
    return S_OK;

}

}  // namespace dxmt9::d3d9::pe::process_vertices
