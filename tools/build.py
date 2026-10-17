#!/usr/bin/env python3
"""Build two independent programs; supports Linux and Windows x64 from Linux."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PINS = {
    "toolkit": ("https://github.com/kveretennicov/dwf-toolkit.git", "fd1e8097b158ab3afef335484d7a111ab8da3d74"),
    "libredwg": ("https://github.com/LibreDWG/libredwg.git", "f860b0fb76d5f8effb5a70769a3389cb1e0308c8"),
    "zlib": ("https://github.com/madler/zlib.git", "51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf"),
}


def run(command, *, log=None, cwd=None, env=None):
    if log:
        with Path(log).open("w") as f:
            result = subprocess.run([str(x) for x in command], cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(f"Falha: {shlex.join(map(str, command))}\n{Path(log).read_text()[-6000:]}")
    else:
        subprocess.run([str(x) for x in command], cwd=cwd, env=env, check=True)


def dependency(name, supplied):
    url, commit = PINS[name]
    path = supplied.resolve() if supplied else ROOT / "build" / "deps" / name
    if not path.exists():
        path.mkdir(parents=True)
        run(["git", "init", path])
        run(["git", "-C", path, "remote", "add", "origin", url])
        run(["git", "-C", path, "fetch", "--depth", "1", "origin", commit])
        run(["git", "-C", path, "checkout", "--detach", "FETCH_HEAD"])
    actual = subprocess.check_output(["git", "-C", path, "rev-parse", "HEAD"], text=True).strip()
    if actual != commit:
        raise RuntimeError(f"Versao inesperada de {name}: {actual}; esperado {commit}")
    dirty = subprocess.check_output(["git", "-C", path, "diff", "--name-only", "HEAD"], text=True).strip()
    if dirty:
        raise RuntimeError(f"Dependencia {name} com arquivos alterados: {dirty}")
    return path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--target", choices=["linux", "windows"], default="linux")
    p.add_argument("--zig", type=Path, help="Zig 0.13.0; necessario para Windows")
    p.add_argument("--cmake", default="cmake")
    p.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    for name in PINS:
        p.add_argument("--" + name, type=Path)
    p.add_argument("--libredwg-build", type=Path, help="Reutilizar diretorio CMake da mesma plataforma")
    args = p.parse_args()
    if args.jobs < 1:
        p.error("--jobs deve ser positivo")
    windows = args.target == "windows"
    if windows and not args.zig:
        p.error("--zig e obrigatorio para Windows")
    work = ROOT / "build" / args.target
    dist = ROOT / "dist" / args.target
    work.mkdir(parents=True, exist_ok=True)
    dist.mkdir(parents=True, exist_ok=True)
    deps = {name: dependency(name, getattr(args, name)) for name in PINS}
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT / "build" / "zig-cache"), ZIG_LOCAL_CACHE_DIR=str(work / "zig-cache"))
    if windows:
        zig = args.zig.resolve()
        version = subprocess.check_output([zig, "version"], text=True).strip()
        if version != "0.13.0":
            raise RuntimeError(f"Use Zig 0.13.0, encontrado {version}")
        cc = [str(zig), "cc", "-target", "x86_64-windows-gnu"]
        cxx = [str(zig), "c++", "-target", "x86_64-windows-gnu"]
        for name, command in [("cc", cc), ("ar", [str(zig), "ar"]), ("ranlib", [str(zig), "ranlib"])]:
            wrapper = work / name
            wrapper.write_text("#!/bin/sh\nexec " + shlex.join(command) + ' "$@"\n')
            wrapper.chmod(0o755)
        toolchain = work / "windows.cmake"
        toolchain.write_text(
            'set(CMAKE_SYSTEM_NAME Windows)\nset(CMAKE_SYSTEM_PROCESSOR AMD64)\n'
            f'set(CMAKE_C_COMPILER "{work / "cc"}")\nset(CMAKE_AR "{work / "ar"}")\n'
            f'set(CMAKE_RANLIB "{work / "ranlib"}")\n'
        )
    else:
        cc, cxx = ["gcc"], ["g++"]

    libbuild = args.libredwg_build.resolve() if args.libredwg_build else work / "libredwg"
    cmake_args = [args.cmake, "-S", deps["libredwg"], "-B", libbuild, "-G", "Unix Makefiles",
                  "-DLIBREDWG_LIBONLY=ON", "-DLIBREDWG_DISABLE_JSON=ON", "-DBUILD_SHARED_LIBS=OFF",
                  "-DENABLE_LTO=OFF", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_FLAGS_RELEASE=-O1"]
    if windows:
        # Old GNU-specific hardening flags are unused by the Windows Clang target.
        cmake_args += [f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", "-DDISABLE_WERROR=ON", "-DCMAKE_C_FLAGS=-Wno-unused-command-line-argument"]
    run(cmake_args, log=work / "libredwg-config.log", env=env)
    run([args.cmake, "--build", libbuild, "--target", "redwg", "-j", args.jobs], log=work / "libredwg-build.log", env=env)

    root = deps["toolkit"] / "develop/global/src"
    base = root / "dwf/w3dtk"
    compat = work / "compat/bits"
    compat.mkdir(parents=True, exist_ok=True)
    # The toolkit's ANSI headers refer to a private libstdc++ header. Supply
    # its public equivalent for libc++; upstream files remain untouched.
    (compat / "stl_algobase.h").write_text("#include <algorithm>\n")
    common = (["-I" + str(compat.parent)] if windows else []) + ["-O1", "-w", "-DDWFTK_STATIC", "-DDWFCORE_STATIC", "-DDWFTK_READ_ONLY",
              "-DHAVE_PROTOTYPES", "-DHAVE_STDLIB_H", "-DHAVE_STDDEF_H",
              "-I" + str(root), "-I" + str(base / "jpeg"), "-I" + str(deps["zlib"])]
    cppflags = ["-std=gnu++11"] + (["-DBSD"] if windows else ["-fpermissive"])
    sources = [(f, True) for f in sorted(base.glob("*.cpp")) if f.name not in {"W3DCamera.cpp", "W3DStreamWriter.cpp"}]
    sources += [(f, True) for f in sorted((base / "utility").glob("*.cpp"))]
    sources += [(f, f.suffix == ".cpp") for f in sorted((base / "hoops").glob("*")) if f.suffix in {".cpp", ".c"}]
    sources += [(root / "dwfcore/Exception.cpp", True)]
    jpegnames = (base / "jpeg/Makefile.am").read_text().split("libdwfw3dtkjpeg_la_SOURCES =")[1].split()
    sources += [(base / "jpeg" / n, False) for n in jpegnames]
    znames = "adler32 crc32 deflate infback inffast inflate inftrees trees zutil compress uncompr".split()
    sources += [(deps["zlib"] / (n + ".c"), False) for n in znames]

    def compile_one(item):
        path, cpp = item
        output = work / (path.stem + ".o")
        command = (cxx + cppflags if cpp else cc) + common + ["-c", path, "-o", output]
        run(command, log=work / (path.stem + ".log"), env=env)
        return output

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        objects = list(pool.map(compile_one, sources))
    frontend = compile_one((ROOT / "src/dwf3d.cpp", True))
    extension = ".exe" if windows else ""
    resource = []
    if windows:
        resource = [work / "windows.res"]
        run([zig, "rc", "/fo", resource[0], "windows.rc"], cwd=ROOT / "src", log=work / "resource.log", env=env)
    run(cxx + [*objects, frontend, *resource, "-o", dist / ("dwf3d" + extension)] +
        (["-municode", "-lcomdlg32", "-luser32", "-static"] if windows else []), log=work / "frontend-link.log", env=env)
    run(cc + ["-std=gnu99", "-O1", "-I" + str(libbuild / "src"), "-I" + str(deps["libredwg"] / "include"),
              ROOT / "src/dwgwriter.c", libbuild / "libredwg.a", *resource, "-lm", "-o", dist / ("dwgwriter" + extension)] +
        (["-static"] if windows else []), log=work / "writer-link.log", env=env)
    if not windows:
        geometry = compile_one((ROOT / "tests/geometry.cpp", True))
        run(cxx + [*objects, geometry, "-o", work / "geometry-test"], log=work / "test-link.log", env=env)
        run([work / "geometry-test"], env=env)
    (dist / "VERSIONS.json").write_text(json.dumps({name: pin[1] for name, pin in PINS.items()}, indent=2) + "\n")
    shutil.copy2(ROOT / "README.md", dist / "LEIA-ME.md")
    licenses = dist / "licenses"
    licenses.mkdir(exist_ok=True)
    shutil.copy2(ROOT / "LICENSE", licenses / "frontend-MIT.txt")
    shutil.copy2(deps["libredwg"] / "COPYING", licenses / "dwgwriter-GPL-3.txt")
    shutil.copy2(deps["toolkit"] / "docs/DWF Toolkit/Specification/sharedHtml/LicenseAgreement.txt", licenses / "Autodesk-DWF-license.txt")
    shutil.copy2(deps["zlib"] / "LICENSE", licenses / "zlib.txt")
    shutil.copy2(base / "jpeg/README", licenses / "jpeg-README.txt")
    print(f"Compilado: {dist}")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
