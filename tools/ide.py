# SPDX-License-Identifier: GPL-3.0-or-later
"""build/ide/compile_commands.json for VS Code on this machine, with the IDF headers copied out of the container."""

import io
import json
import os
import posixpath
import shlex
import subprocess
import tarfile
from pathlib import Path

TOP = Path(__file__).resolve().parent.parent
OUT = TOP / "build/ide"
APP = TOP / "build/app/compile_commands.json"
TEST = TOP / "build/test/host-address-undefined/compile_commands.json"
GCC_ONLY = (
    "-fstrict-volatile-bitfields",
    "-fno-tree-switch-conversion",
    "-fzero-init-padding-bits",
    "-fno-malloc-dce",
    "-Wno-old-style-declaration",
    "-mtune",
    "-march",
    "-specs",
    "-fdiagnostics-color",
    "-fmacro-prefix-map",
)


def container(script: str) -> bytes:
    return subprocess.run(
        ["docker", "compose", "run", "--rm", "-T", "--entrypoint", "bash", "idf", "-c", script],
        cwd=TOP,
        check=True,
        capture_output=True,
    ).stdout


def host_path(path: str, root: str) -> str:
    """The container's root (where compose mounts the repository) becomes TOP; its /opt/esp tree is copied under OUT."""
    if path == root or path.startswith(root + "/"):
        return str(TOP) + path.removeprefix(root)
    return str(OUT) + path if path.startswith("/opt/esp/") else path


def main() -> None:
    with APP.open() as f:
        entries = json.load(f)
    build = entries[0]["directory"]
    root = posixpath.dirname(posixpath.dirname(build))
    gcc = shlex.split(entries[0]["command"])[0]
    # The compiler finds picolibc and its own headers through the specs file; ask it where they are
    probe = container(f"{gcc} @{build}/toolchain/cflags -E -x c -v /dev/null -o /dev/null 2>&1").decode()
    builtin = [
        posixpath.normpath(d)
        for d in probe.split("#include <...> search starts here:")[1].split("End of search list.")[0].split()
    ]
    dirs = set(builtin) | {a[2:] for e in entries for a in shlex.split(e["command"]) if a.startswith("-I/opt/esp")}
    OUT.mkdir(parents=True, exist_ok=True)
    headers = container(
        f"cd / && find {' '.join(sorted(dirs))} -name '*.h' | sort -u | tar --hard-dereference -cf - -T -"
    )
    with tarfile.open(fileobj=io.BytesIO(headers)) as tar:
        tar.extractall(OUT, filter="data")

    isystem = [x for d in builtin for x in ("-isystem", host_path(d, root))]
    directory = host_path(build, root)
    for e in entries:
        args = [
            "-I" + host_path(a[2:], root) if a.startswith("-I") else host_path(a, root)
            for a in shlex.split(e["command"])[1:]
            if not a.startswith(("@", *GCC_ONLY))
        ]
        e["command"] = shlex.join(
            [
                "clang",
                "--target=riscv32-esp-elf",
                "-march=rv32imafc_zicsr_zifencei",
                "-mabi=ilp32f",
                "-nostdinc",
                *isystem,
                *args,
            ]
        )
        e["directory"], e["file"] = directory, host_path(e["file"], root)
        e.pop("output", None)
    if TEST.exists():
        firmware = {e["file"] for e in entries}
        with TEST.open() as f:
            entries += [e for e in json.load(f) if os.path.normpath(e["file"]) not in firmware]
    with (OUT / "compile_commands.json").open("w") as f:
        json.dump(entries, f, indent=1)


if __name__ == "__main__":
    main()
