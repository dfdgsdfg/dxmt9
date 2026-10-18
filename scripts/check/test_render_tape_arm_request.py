#!/usr/bin/env python3
"""Bounded, no-D3D9 Wine test of the production cold arm-request transport."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import select
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wine", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    wine, exe, prefix = args.wine.resolve(), args.exe.resolve(), args.prefix.resolve()
    if not (prefix / "system.reg").is_file():
        parser.error("an existing initialized Wine prefix is required")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    # This explicit local-file diagnostic needs the normal Z: -> / mapping.
    if (prefix / "dosdevices/z:").resolve() != Path("/"):
        parser.error("this test requires the verified Z: -> / drive mapping")
    windows = lambda p: "Z:" + str(p).replace("/", "\\")
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("DXMT", "DXVK", "MVK", "MTL_", "METAL_"))}
    env.update(WINEPREFIX=str(prefix), WINEDEBUG="-all",
               WINEDLLOVERRIDES="mscoree=;mshtml=")
    rows = []

    def run(arguments, expected=0):
        result = subprocess.run([str(wine), str(exe), *arguments], env=env,
                                capture_output=True, text=True, timeout=20)
        rows.append(dict(arguments=arguments, returncode=result.returncode,
                         stdout=result.stdout, stderr=result.stderr))
        if result.returncode != expected:
            raise RuntimeError(f"unexpected helper result: {rows[-1]}")
        return result

    receiver = None
    try:
        run(["--self-test", windows(out / "local.bin")])
        nonce = "0123456789abcdeffedcba9876543210"
        path = windows(out / "cross-process.bin")
        with (out / "receiver.stderr").open("x") as error:
            receiver = subprocess.Popen(
                [str(wine), str(exe), "--receiver-test", path, nonce], env=env,
                stdout=subprocess.PIPE, stderr=error, text=True, bufsize=1)
            before = time.monotonic_ns()
            if not select.select([receiver.stdout], [], [], 20)[0]:
                raise RuntimeError("missing bounded receiver ready receipt")
            line = receiver.stdout.readline().strip()
            match = re.fullmatch(r"receiver_ready pid=(\d+) creation=(\d+)", line)
            if not match:
                raise RuntimeError(f"malformed ready receipt: {line!r}")
            pid, creation = match.groups()
            # The real writer must refuse a recycled PID incarnation before I/O.
            run([path, nonce, pid, str(int(creation) + 1), "5000"], expected=4)
            if (out / "cross-process.bin").exists():
                raise RuntimeError("wrong incarnation created a request")
            run([path, nonce, pid, creation, "5000"])
            stdout, _ = receiver.communicate(timeout=10)
            rows.append(dict(receiver_ready=line, receiver_output=stdout,
                             returncode=receiver.returncode, begin_ns=before,
                             end_ns=time.monotonic_ns()))
            if receiver.returncode != 0 or stdout.strip() != "receiver_accepted":
                raise RuntimeError("cross-process reader did not accept the exact request")
        blob = (out / "cross-process.bin").read_bytes()
        if len(blob) != 64:
            raise RuntimeError("request schema length drift")
        rows.append(dict(request_bytes=len(blob), request_sha256=hashlib.sha256(blob).hexdigest()))
    finally:
        if receiver and receiver.poll() is None:
            receiver.kill()  # Only the exact test child created above.
            receiver.wait(timeout=10)
        (out / "result.json").write_text(json.dumps(dict(
            exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
            wine=str(wine), prefix=str(prefix), rows=rows), indent=2) + "\n")
    print(f"arm-request controlled transport passed: {out}")


if __name__ == "__main__":
    main()
