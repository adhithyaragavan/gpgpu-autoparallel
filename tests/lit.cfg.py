# lit configuration for P05's verdict regression tests.
#
# Every fixture states its expected verdicts as FileCheck lines directly above
# each loop, so a changed verdict anywhere fails the suite. Run with:
#   .venv/bin/lit -v tests        (from the repo root, after building)
# P05_TOOL overrides the tool path (default: build/p05tool).
import os
import platform
import subprocess

import lit.formats

config.name = "p05"
config.test_format = lit.formats.ShTest()
config.suffixes = [".c", ".test"]
config.test_source_root = os.path.dirname(__file__)
config.excludes = ["lit.cfg.py"]

repo = os.path.dirname(config.test_source_root)
# Keep lit's Output/ and timing files out of the source tree.
config.test_exec_root = os.path.join(repo, "build", "tests")
tool = os.environ.get("P05_TOOL", os.path.join(repo, "build", "p05tool"))
config.substitutions.append(("%p05", tool))

# The fixtures are plain C; macOS needs the SDK sysroot to find system headers.
cflags = ""
if platform.system() == "Darwin":
    sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True,
                         text=True).stdout.strip()
    if not sdk:
        lit_config.fatal("xcrun --show-sdk-path failed; cannot locate the macOS SDK")
    cflags = "-isysroot " + sdk
config.substitutions.append(("%cflags", cflags))

# FileCheck: honour an explicit override, else whatever is on PATH, else the
# Homebrew LLVM install used for development. On Linux, apt.llvm.org installs it
# only as /usr/lib/llvm-<N>/bin/FileCheck, so set FILECHECK there (CI does).
filecheck = os.environ.get("FILECHECK")
if filecheck:
    config.substitutions.append(("FileCheck", filecheck))
config.environment["PATH"] = os.pathsep.join(
    [os.environ.get("PATH", ""), "/opt/homebrew/opt/llvm/bin"])
