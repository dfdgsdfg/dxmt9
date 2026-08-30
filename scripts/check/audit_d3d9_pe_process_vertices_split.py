#!/usr/bin/env python3

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
DEVICE = ROOT / "src/d3d9/d3d9_pe_device.cpp"
# The D3D9DeviceImpl declaration/ownership shell lives in this header.
DEVICE_IMPL = ROOT / "src/d3d9/d3d9_pe_device_impl.hpp"
# ProcessVertices, and the rest of the cold COM surface, is DEFINED here since
# step 10 of the hot/cold split; the class header keeps only its declaration.
DEVICE_COLD = ROOT / "src/d3d9/d3d9_pe_device_com_cold.cpp"
# The three remaining D3D9DeviceImpl TUs (DEVICE and DEVICE_IMPL above are the
# other two). `forbid` checks read all five concatenated, so moving offending
# code from any one of them into another cannot defeat them.
DEVICE_SWVP = ROOT / "src/d3d9/d3d9_pe_device_swvp.cpp"
DEVICE_DIAG = ROOT / "src/d3d9/d3d9_pe_device_diag.cpp"
DEVICE_TAPE = ROOT / "src/d3d9/d3d9_pe_device_tape.cpp"
ENGINE_HPP = ROOT / "src/d3d9/d3d9_pe_process_vertices.hpp"
ENGINE_CPP = ROOT / "src/d3d9/d3d9_pe_process_vertices.cpp"
CHILD_HPP = ROOT / "src/d3d9/d3d9_pe_device_child.hpp"
CHILD_CONTEXT_HPP = ROOT / "src/d3d9/d3d9_pe_child_context.hpp"
CHILD_SCOPES_HPP = ROOT / "src/d3d9/d3d9_pe_child_scopes.hpp"
DIAGNOSTIC_OBSERVER_HPP = ROOT / "src/d3d9/d3d9_pe_diagnostic_observer.hpp"
RECORDER_HPP = ROOT / "src/d3d9/d3d9_pe_recorder.hpp"
MESON = ROOT / "src/d3d9/meson.build"
ABI_AUDIT = ROOT / "scripts/check/audit_d3d9_pe_abi_codegen.py"


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
    device_impl = DEVICE_IMPL.read_text()
    device_cold = DEVICE_COLD.read_text()
    # Anything asserted "in the device implementation" must hold across the
    # pair; a forbid that looked at only one half could be defeated by
    # moving the offending code into the other.
    device_all = (device + device_impl + device_cold +
                  DEVICE_SWVP.read_text() + DEVICE_DIAG.read_text() +
                  DEVICE_TAPE.read_text())
    engine_hpp = ENGINE_HPP.read_text()
    engine_cpp = ENGINE_CPP.read_text()
    child_hpp = CHILD_HPP.read_text()
    child_contexts = CHILD_CONTEXT_HPP.read_text()
    child_scopes = CHILD_SCOPES_HPP.read_text()
    diagnostic_observer_hpp = DIAGNOSTIC_OBSERVER_HPP.read_text()
    recorder_hpp = RECORDER_HPP.read_text()
    meson = MESON.read_text()

    require(meson, "'d3d9_pe_process_vertices.cpp'", "PE Meson source")
    require(meson, "'d3d9_pe_device_recorder.cpp'", "recorder owner source")
    require(meson, "'d3d9_pe_device_tape_registry.cpp'", "tape registry owner source")
    require(meson, "'d3d9_pe_device_tape_child.cpp'", "tape child owner source")
    if not ABI_AUDIT.exists():
        fail("missing stable PE ABI/codegen audit script")
    require(meson, "'d3d9_pe_device_com_cold.cpp'", "cold-COM Meson source")
    forbid(meson, "'d3d9_pe_device_hot.cpp'", "retired hot split source")
    forbid(device, '#include "d3d9_pe_device_hot.cpp"', "included source fragment")
    if (ROOT / "src/d3d9/d3d9_pe_device_hot.cpp").exists():
        fail("retired hot split source still exists")
    require(device, '#include "d3d9_pe_device_impl.hpp"',
            "device TU includes the class header")
    require(device_cold, '#include "d3d9_pe_device_impl.hpp"',
            "cold-COM TU includes the class header")
    for fragment in (
        "d3d9_pe_device_state_core.inc.hpp",
        "d3d9_pe_device_state_texture_fvf.inc.hpp",
        "d3d9_pe_device_state_shader_stream.inc.hpp",
    ):
        forbid(device_impl + device, fragment, "retired state fragment")
        if (ROOT / "src/d3d9" / fragment).exists():
            fail(f"retired state fragment still exists: {fragment}")
    if len(device_impl.splitlines()) > 3713:
        fail("D3D9DeviceImpl declaration shell exceeds 3713-line evidence residual")
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
    forbid(device_all, "executeSimpleProcessVertexShaderRange(",
           "interpreter left in device TU")

    # The body lives in the cold-COM TU at file scope, so the closing brace is
    # in column 0 and the name is qualified. Same assertion, new owner.
    wrapper_match = re.search(
        r"^HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::ProcessVertices\(UINT srcStart, "
        r"UINT dstIndex,"
        r".*?^\}$",
        device_cold,
        re.DOTALL | re.MULTILINE,
    )
    if not wrapper_match:
        fail("ProcessVertices STDMETHODCALLTYPE wrapper shape not found")
    wrapper = wrapper_match.group(0)
    if device_cold.count(
            "HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::ProcessVertices(") != 1:
        fail("ProcessVertices STDMETHODCALLTYPE definition must remain unique")
    # The class header keeps exactly one declaration, and it is the one that
    # carries `override`.
    if device_impl.count("HRESULT STDMETHODCALLTYPE ProcessVertices(") != 1:
        fail("ProcessVertices STDMETHODCALLTYPE declaration must remain unique")
    decl_match = re.search(
        r"^    HRESULT STDMETHODCALLTYPE ProcessVertices\(UINT srcStart, UINT dstIndex,"
        r".*?;$",
        device_impl,
        re.DOTALL | re.MULTILINE,
    )
    if not decl_match:
        fail("ProcessVertices declaration shape not found in the class header")
    require(decl_match.group(0), "DWORD flags) noexcept override",
            "exact noexcept override on the declaration")
    ordered = (
        'notePeDeviceCallAfterPresent("ProcessVertices")',
        "if (deviceNotReset_) return D3DERR_DEVICELOST;",
        "const Context context{",
        "return processVertices(",
    )
    positions = [wrapper.find(item) for item in ordered]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        fail("ProcessVertices notification/lost-gate/delegation order changed")
    require(wrapper, "DWORD flags) noexcept {", "exact noexcept definition")
    require(wrapper, ".streamSources = std::span<", "borrowed stream context")
    require(wrapper, ".textures = std::span<", "borrowed texture context")

    require(device_impl, "class D3D9DeviceImpl final : public IDirect3DDevice9Ex",
            "device COM inheritance layout")
    forbid(device_all + child_contexts, "D3D9PeRecorderFlush",
           "removed broad recorder facade")
    key_position = device_impl.find(
        "HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override;")
    state_position = device_impl.find(
        "HRESULT STDMETHODCALLTYPE SetRenderState(")
    if key_position < 0 or state_position < 0 or key_position >= state_position:
        fail("QueryInterface key declaration must precede the state virtuals")
    require(device_impl, "HRESULT FlushPeRecorderForChild() noexcept;",
            "private nonvirtual child flush helper")
    forbid(device_impl, "virtual HRESULT FlushPeRecorderForChild() noexcept;",
           "synthetic child-flush vtable anchor")
    for removed_virtual in (
        "NotifyPeFirstCallAfterPresentForChild",
        "PushPeCallScopeForChild",
        "NotifyPeCallScopeReturnForChild",
        "PopPeCallScopeForChild",
    ):
        forbid(child_hpp + child_contexts + device_all, removed_virtual,
               "retired diagnostic recorder virtual")
    require(child_hpp, '#include "d3d9_pe_child_context.hpp"',
            "typed child context declarations")
    for context_name in (
        "D3D9PeStateBlockContext", "D3D9PeBufferContext",
        "D3D9PeSurfaceTextureContext", "D3D9PeQueryContext",
        "D3D9PePresentationContext", "D3D9PeShaderDeclarationContext",
    ):
        require(child_contexts, f"struct {context_name}",
                f"{context_name} declaration")
    forbid(child_contexts, "virtual ", "non-polymorphic child contexts")
    forbid(child_contexts, "NotifyRenderTapeObjectDefineForChild",
           "object definition remains device-private")
    forbid(child_contexts, "InvalidateStateBlockShadowForChild",
           "shadow invalidation remains device-private")
    for wrapper_source in (
        ROOT / "src/d3d9/d3d9_pe_device_child_buffer.cpp",
        ROOT / "src/d3d9/d3d9_pe_device_child_surface.cpp",
        ROOT / "src/d3d9/d3d9_pe_device_child_misc.cpp",
        ROOT / "src/d3d9/d3d9_pe_device_child_shader.cpp",
    ):
        wrapper_text = wrapper_source.read_text()
        forbid(wrapper_text, "recorder_", "legacy wrapper service field")
        if wrapper_text.count("*context_;") == 0:
            fail(f"{wrapper_source.name} has no typed context field")
    for narrow_source in (
        ROOT / "src/d3d9/d3d9_pe_device_child_buffer.cpp",
        ROOT / "src/d3d9/d3d9_pe_device_child_surface.cpp",
        ROOT / "src/d3d9/d3d9_pe_device_child_shader.cpp",
    ):
        forbid(narrow_source.read_text(), '#include "d3d9_pe_device_child.hpp"',
               "concrete child TU includes umbrella")
    require(child_scopes,
            "D3D9PeChildCallScope(D3D9PeDiagnosticObserver &observer,",
            "enabled-only concrete child observer scope")
    require(child_scopes, "if (!observer) return std::forward<Body>(body)",
            "branch before child observer scope construction")
    require(diagnostic_observer_hpp, "class D3D9PeDiagnosticObserver {",
            "concrete diagnostic observer")
    forbid(diagnostic_observer_hpp, "virtual ",
           "virtual dispatch in diagnostic observer")
    # Observer boundary (see agents/rules/codebase_conventions.rules.md): the
    # ~96-byte PE call-tracking sample is diagnostic storage owned by
    # d3d9_pe_device.cpp. Only the register-sized slot handle may cross this
    # header, and the device's own entry note must stay void -- returning the
    # sample by value put it back into 118 hot-path call-site contracts.
    forbid(child_hpp + child_contexts + child_scopes, "D3D9PePresentCallToken",
           "diagnostic sample in child header")
    require(
        device_impl,
        "void notePeDeviceCallAfterPresent(const char* callName,",
        "void device entry note",
    )
    require(device_impl, "std::unique_ptr<PeDiagnosticsState> diagnostics_{};",
            "nullable cold diagnostics owner")
    require(device,
            "HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::QueryInterface(",
            "QueryInterface key-function definition")
    require(device,
            "HRESULT D3D9DeviceImpl::FlushPeRecorderForChild() noexcept",
            "private child flush helper definition")

    for dead in (
        "shadowedTextureEquals",
        "peInterAppendFocusCallNameIndex",
        "OtherConst",
    ):
        forbid(device_all + recorder_hpp, dead, "dead recorder/device helper")
    if re.search(
        r"flushPendingCommandChunk\(\s*PeRecorderFlushReason reason\s*=",
        device_impl,
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
