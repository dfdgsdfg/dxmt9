#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile


def run(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, check=check, text=True, capture_output=True)


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: test_render_tape_provider_cli.py <fixture> <provider> "
            "<validator> <runner>"
        )
    fixture = pathlib.Path(sys.argv[1])
    provider = pathlib.Path(sys.argv[2])
    validator = pathlib.Path(sys.argv[3])
    runner = pathlib.Path(sys.argv[4])
    bundle_tool = runner
    with tempfile.TemporaryDirectory(prefix="dxmt9-render-tape-provider-") as root:
        root_path = pathlib.Path(root)
        source = root_path / "source"
        bundle = root_path / "bundle"
        run(str(fixture), "--write-production-fixture", str(source))
        run(
            sys.executable,
            str(bundle_tool),
            "pack",
            "--events",
            str(source / "events.bin"),
            "--output-dir",
            str(bundle),
            "--validator",
            str(validator),
        )
        result = json.loads(
            run(
                sys.executable,
                str(runner),
                "provider-replay",
                str(bundle),
                "--provider",
                str(provider),
            ).stdout
        )
        assert result["status"] == "complete"
        assert result["provider_exit_code"] == 0
        assert result["coverage"]["event_count"] == 4
        assert result["validity"]["output_readback"] is True
        assert result["validity"]["expected_digest_captured"] is True
        assert result["validity"]["expected_digest_matched"] is True
        assert result["scope"] == {
            "output_oracle": True,
            "production_capture": False,
            "production_provider_replay": True,
        }
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
