#!/usr/bin/env python3
"""Build the LiteQuery shared library and copy it into the Python package.

Run this once before `pip install .` (or before using the package from a source
checkout). It invokes CMake to build the self-contained shared library and
places the resulting liblitequery.{dll,so,dylib} inside the `litequery/`
package directory, where the ctypes loader finds it.

    python build_lib.py
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BUILD_DIR = os.path.join(REPO_ROOT, "build")
PKG_DIR = os.path.join(HERE, "litequery")


def lib_names() -> list[str]:
    if sys.platform == "win32":
        return ["liblitequery.dll", "litequery.dll"]
    if sys.platform == "darwin":
        return ["liblitequery.dylib", "litequery.dylib"]
    return ["liblitequery.so", "litequery.so"]


def find_built_lib() -> str:
    for root in (BUILD_DIR, os.path.join(BUILD_DIR, "Release")):
        for name in lib_names():
            p = os.path.join(root, name)
            if os.path.exists(p):
                return p
    raise SystemExit(
        "Could not find the built shared library under "
        f"{BUILD_DIR}. Did the CMake build succeed?"
    )


def main() -> None:
    print(f"Configuring CMake in {BUILD_DIR} ...")
    subprocess.check_call(
        ["cmake", "-S", REPO_ROOT, "-B", BUILD_DIR,
         "-DCMAKE_BUILD_TYPE=Release", "-DLITEQUERY_BUILD_SHARED=ON"]
    )
    print("Building litequery_shared ...")
    subprocess.check_call(
        ["cmake", "--build", BUILD_DIR, "--target", "litequery_shared",
         "--config", "Release"]
    )

    src = find_built_lib()
    dst = os.path.join(PKG_DIR, os.path.basename(src))
    shutil.copy2(src, dst)
    print(f"Copied {os.path.basename(src)} -> {dst}")
    print("Done. You can now `pip install .` or import litequery from here.")


if __name__ == "__main__":
    main()
