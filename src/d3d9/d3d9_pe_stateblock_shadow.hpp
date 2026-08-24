#pragma once

#include "d3d9_pe.hpp"
#include "d3d9_pe_state_shadow.hpp"

#include <utility>

// Local COM ownership policies consume only category-qualified references.
// They preserve the exact interface subobject pointer used by the public D3D9
// call and make a cross-kind IUnknown reinterpret_cast unrepresentable.
struct D3D9PeRetainStateBlockRef {
  template<typename Ref>
  void operator()(Ref value) const noexcept {
    if (auto* object = value.raw()) object->AddRef();
  }
};

struct D3D9PeReleaseStateBlockRef {
  template<typename Ref>
  void operator()(Ref value) const noexcept {
    if (auto* object = value.raw()) object->Release();
  }
};

inline constexpr D3D9PeRetainStateBlockRef d3d9PeRetainStateBlockRef{};
inline constexpr D3D9PeReleaseStateBlockRef d3d9PeReleaseStateBlockRef{};

// PE-side snapshot taken by D3D9StateBlockImpl::Capture(). It lives entirely
// in the PE process and never crosses the unix boundary. The snapshot owns
// references to tracked declarations/categories and releases them on
// destruction or overwrite.
class D3D9StateBlockShadow {
 private:
  FixedStateTable<kPeRenderStateSlots> renderStates_{};
  FixedStateMatrix<kPeTextureStageSlots, kPeTextureStageStateSlots>
      textureStageStates_{};
  FixedStateMatrix<kPeSamplerSlots, kPeSamplerStateSlots> samplerStates_{};
  FixedTransformTable transforms_{};
  StateBlockRecorded categories_{};
  PeStateBlockConstRecorded constants_{};
  bool hasVdecl_ = false;
  IDirect3DVertexDeclaration9 *vdecl_ = nullptr;
  bool initialized_ = false;

 public:
  class Writer {
   public:
    RenderStateTableView renderStates() noexcept {
      return RenderStateTableView(shadow_.renderStates_);
    }
    TssTableView textureStageStates() noexcept {
      return TssTableView(shadow_.textureStageStates_);
    }
    SamplerStateTableView samplerStates() noexcept {
      return SamplerStateTableView(shadow_.samplerStates_);
    }
    TypedTransformTableView transforms() noexcept {
      return TypedTransformTableView(shadow_.transforms_);
    }
    StateBlockRecorded::Writer categories() noexcept {
      return shadow_.categories_.writer();
    }
    PeStateBlockConstRecorded& constants() noexcept {
      return shadow_.constants_;
    }
    void setInitialized(bool initialized) noexcept {
      shadow_.initialized_ = initialized;
    }
    void replaceVdecl(bool tracked,
                      IDirect3DVertexDeclaration9 *value) noexcept {
      shadow_.releaseSavedVdecl();
      shadow_.hasVdecl_ = tracked && value != nullptr;
      if (tracked && value) {
        value->AddRef();
        shadow_.vdecl_ = value;
      }
    }
    void discardVdecl() noexcept {
      shadow_.releaseSavedVdecl();
      shadow_.hasVdecl_ = false;
    }

   private:
    explicit Writer(D3D9StateBlockShadow& shadow) noexcept : shadow_(shadow) {}
    D3D9StateBlockShadow& shadow_;
    friend class D3D9StateBlockShadow;
  };

  class Snapshot {
   public:
    ConstRenderStateTableView renderStates() const noexcept {
      return ConstRenderStateTableView(shadow_.renderStates_);
    }
    ConstTssTableView textureStageStates() const noexcept {
      return ConstTssTableView(shadow_.textureStageStates_);
    }
    ConstSamplerStateTableView samplerStates() const noexcept {
      return ConstSamplerStateTableView(shadow_.samplerStates_);
    }
    ConstTypedTransformTableView transforms() const noexcept {
      return ConstTypedTransformTableView(shadow_.transforms_);
    }
    const StateBlockRecorded& categories() const noexcept {
      return shadow_.categories_.snapshot();
    }
    const PeStateBlockConstRecorded& constants() const noexcept {
      return shadow_.constants_;
    }
    bool initialized() const noexcept { return shadow_.initialized_; }
    bool hasVdecl() const noexcept { return shadow_.hasVdecl_; }
    IDirect3DVertexDeclaration9 *vdecl() const noexcept {
      return shadow_.vdecl_;
    }

   private:
    explicit Snapshot(const D3D9StateBlockShadow& shadow) noexcept
        : shadow_(shadow) {}
    const D3D9StateBlockShadow& shadow_;
    friend class D3D9StateBlockShadow;
  };

  Writer writer() noexcept { return Writer(*this); }
  Snapshot snapshot() const noexcept { return Snapshot(*this); }

  D3D9StateBlockShadow() = default;
  D3D9StateBlockShadow(const D3D9StateBlockShadow& other) { *this = other; }
  D3D9StateBlockShadow& operator=(const D3D9StateBlockShadow& other) {
    if (this == &other) return *this;
    releaseSavedVdecl();
    releaseCategoryRefs();
    renderStates_ = other.renderStates_;
    textureStageStates_ = other.textureStageStates_;
    samplerStates_ = other.samplerStates_;
    transforms_ = other.transforms_;
    categories_ = other.categories_;
    constants_ = other.constants_;
    hasVdecl_ = other.hasVdecl_;
    initialized_ = other.initialized_;
    vdecl_ = other.vdecl_;
    if (vdecl_) vdecl_->AddRef();
    addCategoryRefs();
    return *this;
  }
  D3D9StateBlockShadow(D3D9StateBlockShadow&& other) noexcept {
    *this = std::move(other);
  }
  D3D9StateBlockShadow& operator=(D3D9StateBlockShadow&& other) noexcept {
    if (this == &other) return *this;
    releaseSavedVdecl();
    releaseCategoryRefs();
    renderStates_ = other.renderStates_;
    textureStageStates_ = other.textureStageStates_;
    samplerStates_ = other.samplerStates_;
    transforms_ = other.transforms_;
    categories_ = other.categories_;
    constants_ = other.constants_;
    hasVdecl_ = other.hasVdecl_;
    initialized_ = other.initialized_;
    vdecl_ = other.vdecl_;
    other.vdecl_ = nullptr;
    other.hasVdecl_ = false;
    other.initialized_ = false;
    other.categories_.writer().clear();
    return *this;
  }
  ~D3D9StateBlockShadow() {
    releaseSavedVdecl();
    releaseCategoryRefs();
  }

  void copyCategoriesFrom(const StateBlockRecorded& source) noexcept {
    releaseCategoryRefs();
    categories_ = source;
    addCategoryRefs();
  }
  void clearCategoriesOwned() noexcept { releaseCategoryRefs(); }

 private:
  void addCategoryRefs() noexcept {
    categories_.forEachOwnedComRef(d3d9PeRetainStateBlockRef);
  }
  void releaseCategoryRefs() noexcept {
    categories_.forEachOwnedComRef(d3d9PeReleaseStateBlockRef);
    categories_.writer().clear();
  }

  void releaseSavedVdecl() noexcept {
    if (vdecl_) {
      vdecl_->Release();
      vdecl_ = nullptr;
    }
  }
};
