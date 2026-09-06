#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""CLI failures must be visible to automation and must not create output."""

import os
from pathlib import Path
import subprocess
import tempfile


VV = os.environ.get("VV_BIN", "./vaptvupt")


def run(*args):
    return subprocess.run([VV, *args], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=20)


def main():
    checks = 0
    with tempfile.TemporaryDirectory(prefix="vv-cli-contract-") as directory:
        root = Path(directory)
        source = root / "input.bin"
        source.write_bytes(b"VaptVupt buffered output regression.\n" * 4)
        encoded = root / "out.vv"
        decoded = root / "decoded.bin"
        for option in ("-D", "--depth", "-A", "--accel", "-w", "--window", "-T"):
            for value in ("", "invalid", "1junk", "-1", "+1", " 1", "1 ",
                          "4294967296", "999999999999999999999999999999"):
                result = run("-c", option, value, "-o", str(encoded), str(source))
                assert result.returncode == 1, (option, value, result.stderr)
                assert not encoded.exists(), (option, value, "created output")
                checks += 1

        invalid = [
            ["-c", "-m", "balnced"],
            ["-c", "-d"], ["-t", "-b"],
            ["-c", "--bcj", "--bcj-arm64"],
            ["-c", str(source)],  # second positional input is appended below
        ]
        for options in invalid:
            result = run(*options, "-o", str(encoded), str(source))
            assert result.returncode == 1, (options, result.stderr)
            assert not encoded.exists(), (options, "created output")
            checks += 1

        assert run("-c", "-o", str(encoded), str(source)).returncode == 0
        assert run("-d", "-o", str(decoded), str(encoded)).returncode == 0
        assert decoded.read_bytes() == source.read_bytes()
        checks += 1

        if Path("/dev/full").exists():
            for action, path in (("-c", source), ("-d", encoded)):
                result = run(action, "-o", "/dev/full", str(path))
                assert result.returncode == 1, (action, "lost write error", result.stderr)
                assert b"No space left" in result.stderr or b"/dev/full" in result.stderr
                checks += 1
        else:
            print("cli_contract: /dev/full unavailable; write-error probe skipped")

        source.write_bytes(b"")
        assert run("-c", "-o", str(encoded), str(source)).returncode == 0
        assert run("-d", "-o", str(decoded), str(encoded)).returncode == 0
        assert decoded.read_bytes() == b""
        checks += 1

    print(f"cli_contract PASS ({checks} checks)")


if __name__ == "__main__":
    main()
