"""Pin removal of retired selectors and their exclusive runtime scaffolding.

This is a source-surface audit, not a rendering/lifetime proof. Native
transition tests and the independent Metal rebuild oracle cover semantics.
"""
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[2]
    retired = (
        "DXMT9_REPLAY_PRECISE_UNIFORM_INVALIDATION",
        "DXMT9_LOCK_SCOPE_REFINEMENT",
        "DXMT9_PREFETCH_UNPUBLISHED_SLOT_PSO",
        "DXMT9_ARGBUF_CBUF_CONTENT_HASH",
        "lockScopeRefinementEnabled",
        "unpublishedSlotPsoPrefetchEnabled",
        "prefetchCurrentWritingSlotPipelines",
        "pipelinePrefetchCommandCursor",
        "countUnpublishedSlotPsoPrefetchCpuTime",
        "unpublishedSlotPsoPrefetchCpu",
        "argbufCbufContentHashEnabled",
        "hashConstantBufferBytes",
        "countEncodeDrawArgbufCbufBindingHash",
        "encodeDrawArgbufCbufBindingHash",
        "contentMatches",
        "hasMatchingBinding",
    )
    violations = []
    for directory in (root / "src", root / "include"):
        for path in directory.rglob("*"):
            if path.suffix not in {".h", ".hpp", ".c", ".cpp", ".mm"}:
                continue
            text = path.read_text(encoding="utf-8")
            for symbol in retired:
                if symbol in text:
                    violations.append(f"{path.relative_to(root)}: {symbol}")
    assert not violations, "retired runtime surface remains:\n" + "\n".join(violations)
    print("runtime policy retirement: four selectors and exclusive scaffolding absent")


if __name__ == "__main__":
    main()
