#include "dxmt9_format_convert.hpp"

namespace dxmt9::convert {

using core::BlendFactor;
using core::BlendOp;
using core::CompareFunc;
using core::CullMode;
using core::Format;
using core::IndexType;
using core::Pool;
using core::PrimitiveType;
using core::StencilOp;
using core::SurfaceDesc;
using core::TextureDesc;
using core::TextureType;
using core::UsageDepthStencil;
using core::UsageDynamic;
using core::UsageRenderTarget;

WMTPixelFormat toPixelFormat(Format format, const core::BackendLimits& limits) {
  switch (format) {
    case Format::A8R8G8B8:
    case Format::X8R8G8B8:
      return WMTPixelFormatBGRA8Unorm;
    case Format::A8B8G8R8:
    case Format::X8B8G8R8:
      return WMTPixelFormatRGBA8Unorm;
    case Format::R5G6B5:
      return WMTPixelFormatB5G6R5Unorm;
    case Format::A1R5G5B5:
    case Format::X1R5G5B5:
      return WMTPixelFormatBGR5A1Unorm;
    case Format::A4R4G4B4:
      return WMTPixelFormatABGR4Unorm;
    case Format::A8:
      return WMTPixelFormatA8Unorm;
    case Format::A16B16G16R16F:
      return WMTPixelFormatRGBA16Float;
    case Format::A32B32G32R32F:
      return WMTPixelFormatRGBA32Float;
    case Format::G16R16F:
      return WMTPixelFormatRG16Float;
    case Format::R16F:
      return WMTPixelFormatR16Float;
    case Format::G32R32F:
      return WMTPixelFormatRG32Float;
    case Format::R32F:
      return WMTPixelFormatR32Float;
    case Format::A16B16G16R16:
      return WMTPixelFormatRGBA16Unorm;
    case Format::G16R16:
      return WMTPixelFormatRG16Unorm;
    case Format::A2R10G10B10:
      return WMTPixelFormatRGB10A2Unorm;
    case Format::A2B10G10R10:
      return WMTPixelFormatBGR10A2Unorm;
    case Format::L8:
      return WMTPixelFormatR8Unorm;
    case Format::L16:
      return WMTPixelFormatR16Unorm;
    case Format::A8L8:
      return WMTPixelFormatRG8Unorm;
    case Format::V8U8:
      return WMTPixelFormatRG8Snorm;
    case Format::Q8W8V8U8:
      return WMTPixelFormatRGBA8Snorm;
    case Format::V16U16:
      return WMTPixelFormatRG16Snorm;
    case Format::D24S8:
    case Format::D24X8:
      if (limits.supportsDepth24Stencil8) {
        return WMTPixelFormatDepth24Unorm_Stencil8;
      }
      return limits.supportsDepth32FloatStencil8 ? WMTPixelFormatDepth32Float_Stencil8
                                                 : WMTPixelFormatDepth32Float;
    case Format::D16:
    case Format::D16_LOCKABLE:
      return WMTPixelFormatDepth16Unorm;
    case Format::D32:
    case Format::D32F_LOCKABLE:
      return WMTPixelFormatDepth32Float;
    case Format::D24FS8:
      return WMTPixelFormatDepth32Float_Stencil8;
    default:
      return WMTPixelFormatBGRA8Unorm;
  }
}

bool formatHasStencilAspect(Format format) {
  switch (format) {
    case Format::D24S8:
    case Format::D24FS8:
      return true;
    default:
      return false;
  }
}

bool formatHasDepthAspect(Format format) {
  switch (format) {
    case Format::D24S8:
    case Format::D24X8:
    case Format::D16:
    case Format::D32:
    case Format::D32F_LOCKABLE:
    case Format::D16_LOCKABLE:
    case Format::D24FS8:
      return true;
    default:
      return false;
  }
}

WMTTextureType toTextureType(TextureType type, bool multisample) {
  switch (type) {
    case TextureType::TwoD:
      return multisample ? WMTTextureType2DMultisample : WMTTextureType2D;
    case TextureType::Cube:
      return WMTTextureTypeCube;
    case TextureType::Volume:
      return WMTTextureType3D;
    case TextureType::Array2D:
      return multisample ? WMTTextureType2DMultisampleArray : WMTTextureType2DArray;
  }
  return multisample ? WMTTextureType2DMultisample : WMTTextureType2D;
}

WMTResourceOptions toResourceOptions(Pool pool, u32 usage) {
  if (pool == Pool::SystemMem || pool == Pool::Scratch || pool == Pool::Managed) {
    return WMTResourceStorageModeShared;
  }
  return (usage & UsageDynamic) != 0 ? WMTResourceStorageModeShared : WMTResourceStorageModePrivate;
}

WMTTextureUsage toTextureUsage(const SurfaceDesc& desc) {
  WMTTextureUsage usage = WMTTextureUsageUnknown;
  if (desc.renderTarget || desc.depthStencil) {
    usage = static_cast<WMTTextureUsage>(usage | WMTTextureUsageRenderTarget);
  }
  usage = static_cast<WMTTextureUsage>(usage | WMTTextureUsageShaderRead);
  return usage;
}

WMTTextureUsage toTextureUsage(const TextureDesc& desc) {
  WMTTextureUsage usage = WMTTextureUsageShaderRead;
  if ((desc.usage & UsageRenderTarget) != 0 || (desc.usage & UsageDepthStencil) != 0) {
    usage = static_cast<WMTTextureUsage>(usage | WMTTextureUsageRenderTarget);
  }
  return usage;
}

WMTPrimitiveType toPrimitiveType(PrimitiveType type) {
  switch (type) {
    case PrimitiveType::PointList:
      return WMTPrimitiveTypePoint;
    case PrimitiveType::LineList:
      return WMTPrimitiveTypeLine;
    case PrimitiveType::LineStrip:
      return WMTPrimitiveTypeLineStrip;
    case PrimitiveType::TriangleList:
    case PrimitiveType::TriangleFan:
      return WMTPrimitiveTypeTriangle;
    case PrimitiveType::TriangleStrip:
      return WMTPrimitiveTypeTriangleStrip;
  }
  return WMTPrimitiveTypeTriangle;
}

WMTIndexType toIndexType(IndexType type) {
  return type == IndexType::UInt32 ? WMTIndexTypeUInt32 : WMTIndexTypeUInt16;
}

WMTCompareFunction toCompareFunction(u32 value) {
  switch (static_cast<CompareFunc>(value)) {
    case CompareFunc::Never:        return WMTCompareFunctionNever;
    case CompareFunc::Less:         return WMTCompareFunctionLess;
    case CompareFunc::Equal:        return WMTCompareFunctionEqual;
    case CompareFunc::LessEqual:    return WMTCompareFunctionLessEqual;
    case CompareFunc::Greater:      return WMTCompareFunctionGreater;
    case CompareFunc::NotEqual:     return WMTCompareFunctionNotEqual;
    case CompareFunc::GreaterEqual: return WMTCompareFunctionGreaterEqual;
    case CompareFunc::Always:       return WMTCompareFunctionAlways;
  }
  return WMTCompareFunctionAlways;
}

WMTBlendOperation toBlendOperation(u32 value) {
  switch (static_cast<BlendOp>(value)) {
    case BlendOp::Add:         return WMTBlendOperationAdd;
    case BlendOp::Subtract:    return WMTBlendOperationSubtract;
    case BlendOp::RevSubtract: return WMTBlendOperationReverseSubtract;
    case BlendOp::Min:         return WMTBlendOperationMin;
    case BlendOp::Max:         return WMTBlendOperationMax;
  }
  return WMTBlendOperationAdd;
}

WMTBlendFactor toBlendFactor(u32 value) {
  switch (static_cast<BlendFactor>(value)) {
    case BlendFactor::Zero:            return WMTBlendFactorZero;
    case BlendFactor::One:             return WMTBlendFactorOne;
    case BlendFactor::SrcColor:        return WMTBlendFactorSourceColor;
    case BlendFactor::InvSrcColor:     return WMTBlendFactorOneMinusSourceColor;
    case BlendFactor::SrcAlpha:        return WMTBlendFactorSourceAlpha;
    case BlendFactor::InvSrcAlpha:     return WMTBlendFactorOneMinusSourceAlpha;
    case BlendFactor::DestAlpha:       return WMTBlendFactorDestinationAlpha;
    case BlendFactor::InvDestAlpha:    return WMTBlendFactorOneMinusDestinationAlpha;
    case BlendFactor::DestColor:       return WMTBlendFactorDestinationColor;
    case BlendFactor::InvDestColor:    return WMTBlendFactorOneMinusDestinationColor;
    case BlendFactor::SrcAlphaSat:     return WMTBlendFactorSourceAlphaSaturated;
    case BlendFactor::BothSrcAlpha:    return WMTBlendFactorBlendAlpha;
    case BlendFactor::BothInvSrcAlpha: return WMTBlendFactorOneMinusBlendAlpha;
    case BlendFactor::BlendFactor:     return WMTBlendFactorBlendColor;
    case BlendFactor::InvBlendFactor:  return WMTBlendFactorOneMinusBlendColor;
  }
  return WMTBlendFactorOne;
}

WMTCullMode toCullMode(u32 value) {
  switch (static_cast<CullMode>(value)) {
    case CullMode::None: return WMTCullModeNone;
    case CullMode::Cw:   return WMTCullModeBack;
    case CullMode::Ccw:  return WMTCullModeFront;
  }
  return WMTCullModeNone;
}

WMTStencilOperation toStencilOperation(u32 value) {
  switch (static_cast<StencilOp>(value)) {
    case StencilOp::Keep:    return WMTStencilOperationKeep;
    case StencilOp::Zero:    return WMTStencilOperationZero;
    case StencilOp::Replace: return WMTStencilOperationReplace;
    case StencilOp::IncrSat: return WMTStencilOperationIncrementClamp;
    case StencilOp::DecrSat: return WMTStencilOperationDecrementClamp;
    case StencilOp::Invert:  return WMTStencilOperationInvert;
    case StencilOp::Incr:    return WMTStencilOperationIncrementWrap;
    case StencilOp::Decr:    return WMTStencilOperationDecrementWrap;
  }
  return WMTStencilOperationKeep;
}

std::uint8_t toColorWriteMask(u32 value) {
  std::uint8_t mask = 0;
  if ((value & 0x1u) != 0) mask |= WMTColorWriteMaskRed;
  if ((value & 0x2u) != 0) mask |= WMTColorWriteMaskGreen;
  if ((value & 0x4u) != 0) mask |= WMTColorWriteMaskBlue;
  if ((value & 0x8u) != 0) mask |= WMTColorWriteMaskAlpha;
  return mask == 0 ? WMTColorWriteMaskAll : mask;
}

}  // namespace dxmt9::convert
