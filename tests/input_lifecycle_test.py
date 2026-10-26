"""Run the production Swift input policy against the C bridge on an Apple host."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
dest = Path(sys.argv[1]).resolve()
dest.mkdir(parents=True, exist_ok=True)
objects = []
for source in ("runtime/guest_cursor.c", "ios/NFSU2iOS/Native/NFSU2InputBridge.c",
               "tests/input_lifecycle_memory.c"):
    output = dest / (Path(source).stem + "-input-lifecycle.o")
    subprocess.run([os.environ.get("CC", "clang"), "-O2", "-std=gnu11", "-Iruntime",
                    "-fno-strict-aliasing", "-c", source, "-o", str(output)],
                   cwd=root, check=True, timeout=120)
    objects.append(str(output))
exe = dest / "input-lifecycle-test"
subprocess.run(["swiftc", "-parse-as-library", "-import-objc-header",
                "ios/NFSU2iOS/Native/NFSU2InputBridge.h",
                "ios/NFSU2iOS/GameInputLifecycle.swift", "tests/input_lifecycle_fixture.swift",
                *objects, "-o", str(exe)], cwd=root, check=True, timeout=120)
subprocess.run([str(exe)], cwd=root, check=True, timeout=30)
