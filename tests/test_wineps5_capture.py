#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Wine's PS5 audio driver recording from the title's microphone, on the host
(tests/wineps5_capture_host.c): the capture endpoint appears with the title's
source, its packets carry the microphone's samples converted to the stream's
format, a muted microphone gives zeros flagged AUDCLNT_BUFFERFLAGS_SILENT and
answers patch 0987's device-mute property, and an overrun is flagged as a
discontinuity.

The driver includes mmdevapi's unixlib.h and the IDL-generated audio headers,
so it needs the pinned Wine checkout (PROSPERO_WINE_SOURCE or
.deps/wine/source) and a configured Wine build tree whose include/ holds the
generated headers (PROSPERO_WINE_BUILD; `make include/audioclient.h
include/mmdeviceapi.h include/propidl.h` there is enough). Without either the
check is skipped with a message. unixlib.h is taken with patch 0987 applied."""
from __future__ import annotations

import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/0987-mmdevapi-report-a-microphone-muted-on-the-device.patch"
WINE_COMMIT = "490f6d5dcbb2a5047345b8af88d114bbcaad69a8"
UNIXLIB = "dlls/mmdevapi/unixlib.h"


def wine_source() -> Path | None:
    candidates = [Path(os.environ["PROSPERO_WINE_SOURCE"])] if os.environ.get("PROSPERO_WINE_SOURCE") else []
    candidates.append(ROOT / ".deps/wine/source")
    for candidate in candidates:
        if (candidate / UNIXLIB).is_file():
            return candidate
    return None


def main() -> int:
    source = wine_source()
    build = Path(os.environ["PROSPERO_WINE_BUILD"]) if os.environ.get("PROSPERO_WINE_BUILD") else None
    if not source or not build or not (build / "include/audioclient.h").is_file():
        print("wineps5 capture skipped: needs the pinned Wine source and PROSPERO_WINE_BUILD "
              "with generated headers")
        return 0
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        # The pinned unixlib.h, with the patch's key.
        pristine = subprocess.run(["git", "-C", str(source), "show", f"{WINE_COMMIT}:{UNIXLIB}"],
                                  capture_output=True, text=True)
        target = work / UNIXLIB
        target.parent.mkdir(parents=True)
        target.write_text(pristine.stdout if pristine.returncode == 0 else (source / UNIXLIB).read_text())
        subprocess.run(["git", "apply", f"--include={UNIXLIB}", str(PATCH)], cwd=work, check=True)
        exe = work / "wineps5_capture_host"
        command = [*cc, "-std=gnu11", "-O1", "-g", "-fPIC", "-rdynamic", "-Wall", "-Wno-unused-parameter",
                   "-Wno-missing-field-initializers", "-D__WINESRC__", "-DWINE_UNIX_LIB", "-D_REENTRANT",
                   f"-I{target.parent}", f"-I{build / 'include'}", f"-I{source / 'include'}",
                   f"-I{ROOT / 'wine/ps5'}", f"-I{ROOT / 'src'}", f"-I{ROOT / 'include'}",
                   str(ROOT / "tests/wineps5_capture_host.c"), str(ROOT / "wine/ps5/pw_wine_sink.c"),
                   str(ROOT / "src/pw_audio_mix.c"), str(ROOT / "src/pw_audio_capture.c"),
                   "-ldl", "-pthread", "-o", str(exe)]
        subprocess.run(command, check=True)
        done = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        assert done.returncode == 0 and "wineps5 capture passed" in done.stdout, done.stdout + done.stderr
    print("wineps5 capture passed: the DualSense microphone records, and its mute reaches the game")
    return 0


if __name__ == "__main__":
    sys.exit(main())
