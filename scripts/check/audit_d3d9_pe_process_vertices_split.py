#!/usr/bin/env python3

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
DEVICE = ROOT / "src/d3d9/d3d9_pe_device.cpp"
ENGINE_HPP = ROOT / "src/d3d9/d3d9_pe_process_vertices.hpp"
ENGINE_CPP = ROOT / "src/d3d9/d3d9_pe_process_vertices.cpp"
CHILD_HPP = ROOT / "src/d3d9/d3d9_pe_device_child.hpp"
RECORDER_HPP = ROOT / "src/d3d9/d3d9_pe_recorder.hpp"
MESON = ROOT / "src/d3d9/meson.build"


def fail(message: str) -> None:
    raise AssertionError(message)


def require(source: str, needle: str, label: str) -> None:
    if needle not in source:
        fail(f"missing {label}: {needle!r}")


def forbid(source: str, needle: str, label: str) -> None:
    if needle in source:
        fail(f"unexpected {label}: {needle!r}")


def main() -> int:
    device = DEVICE.read_text()
    engine_hpp = ENGINE_HPP.read_text()
    engine_cpp = ENGINE_CPP.read_text()
    child_hpp = CHILD_HPP.read_text()
    recorder_hpp = RECORDER_HPP.read_text()
    meson = MESON.read_text()

    require(meson, "'d3d9_pe_process_vertices.cpp'", "PE Meson source")
    require(engine_hpp, "struct Context {", "borrowed context")
    require(engine_hpp, "std::span<IDirect3DVertexBuffer9 *const,", "bounded stream span")
    require(engine_hpp, "std::span<IDirect3DBaseTexture9 *const,", "bounded texture span")
    require(engine_hpp, "const PeHotStateShadow &state;", "borrowed state shadow")
    require(engine_hpp, "const PeConstShadowBlock &constants;", "borrowed constant shadow")
    require(engine_hpp, "DWORD flags) noexcept;", "synchronous noexcept API")

    for forbidden in (
        "D3D9DeviceImpl",
        "AddRef(",
        "Release(",
        "#import",
        "CAMetalLayer",
        "MTLDevice",
    ):
        forbid(engine_hpp + engine_cpp, forbidden, "engine ownership leak")

    require(engine_cpp, "HRESULT processVertices(const Context& context,",
            "free-function implementation")
    require(engine_cpp, "for (UINT i = 0; i < vertexCount; ++i)",
            "co-located per-vertex loop")
    require(engine_cpp, "executeSimpleProcessVertexShaderRange(",
            "co-located simple-VS interpreter")
    require(engine_cpp, "describeProcessFvf(", "co-located FVF engine")
    forbid(device, "executeSimpleProcessVertexShaderRange(",
           "interpreter left in device TU")

    wrapper_match = re.search(
        r"^    HRESULT STDMETHODCALLTYPE ProcessVertices\(UINT srcStart, UINT dstIndex,"
        r".*?^    \}$",
        device,
        re.DOTALL | re.MULTILINE,
    )
    if not wrapper_match:
        fail("ProcessVertices STDMETHODCALLTYPE wrapper shape not found")
    wrapper = wrapper_match.group(0)
    if device.count("HRESULT STDMETHODCALLTYPE ProcessVertices(") != 1:
        fail("ProcessVertices STDMETHODCALLTYPE must remain unique")
    ordered = (
        'notePeDeviceCallAfterPresent("ProcessVertices")',
        "if (deviceNotReset_) return D3DERR_DEVICELOST;",
        "const Context context{",
        "return processVertices(",
    )
    positions = [wrapper.find(item) for item in ordered]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        fail("ProcessVertices notification/lost-gate/delegation order changed")
    require(wrapper, "DWORD flags) noexcept override", "exact noexcept override")
    require(wrapper, ".streamSources = std::span<", "borrowed stream context")
    require(wrapper, ".textures = std::span<", "borrowed texture context")

    require(
        device,
        "class D3D9DeviceImpl final : public IDirect3DDevice9Ex, public D3D9PeRecorderFlush",
        "device inheritance layout",
    )
    if device.count("public D3D9PeRecorderFlush") != 1:
        fail("D3D9PeRecorderFlush must have exactly one derived implementation")
    require(
        child_hpp,
        "virtual void NotifyPeFirstCallAfterPresentForChild(",
        "void first-call notification",
    )
    require(
        child_hpp,
        "const char *callName, const void *callerPc = nullptr) noexcept = 0;",
        "pure first-call notification",
    )
    require(
        child_hpp,
        "virtual D3D9PePresentCallSlot PushPeCallScopeForChild(",
        "pure call-scope push",
    )
    require(
        child_hpp,
        "virtual void NotifyPeCallScopeReturnForChild(D3D9PePresentCallSlot slot,",
        "pure call-scope return notification",
    )
    require(
        child_hpp,
        "HRESULT hr) noexcept = 0;",
        "pure return notification",
    )
    require(
        child_hpp,
        "virtual void PopPeCallScopeForChild(D3D9PePresentCallSlot slot) noexcept = 0;",
        "pure call-scope pop",
    )
    # Observer boundary (see agents/rules/codebase_conventions.rules.md): the
    # ~96-byte PE call-tracking sample is diagnostic storage owned by
    # d3d9_pe_device.cpp. Only the register-sized slot handle may cross this
    # header, and the device's own entry note must stay void -- returning the
    # sample by value put it back into 118 hot-path call-site contracts.
    forbid(child_hpp, "D3D9PePresentCallToken", "diagnostic sample in child header")
    require(
        device,
        "void notePeDeviceCallAfterPresent(const char* callName,",
        "void device entry note",
    )
    require(device, "NotifyPeFirstCallAfterPresentForChild(", "first-call override")
    require(device, "NotifyPeCallScopeReturnForChild(", "call-scope return override")
    require(device, "PopPeCallScopeForChild(", "call-scope pop override")

    for dead in (
        "shadowedTextureEquals",
        "peInterAppendFocusCallNameIndex",
        "OtherConst",
    ):
        forbid(device + recorder_hpp, dead, "dead recorder/device helper")
    if re.search(
        r"flushPendingCommandChunk\(\s*PeRecorderFlushReason reason\s*=",
        device,
    ):
        fail("flushPendingCommandChunk retains its unused default argument")

    print("d3d9 PE ProcessVertices split source contract OK")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"audit_d3d9_pe_process_vertices_split: {error}", file=sys.stderr)
        raise SystemExit(1)
