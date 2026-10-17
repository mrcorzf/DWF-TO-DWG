#!/usr/bin/env python3
"""Package Windows programs with corresponding sources and license notices."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import zipfile

from build import PINS, ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in PINS:
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    dist = ROOT / "dist/windows"
    for name in ("dwf3d.exe", "dwgwriter.exe"):
        if not (dist / name).is_file():
            parser.error(f"Compile primeiro: {dist / name}")
    sources = dist / "sources"
    sources.mkdir(exist_ok=True)
    for name, (_, commit) in PINS.items():
        path = getattr(args, name).resolve()
        actual = subprocess.check_output(["git", "-C", path, "rev-parse", "HEAD"], text=True).strip()
        if actual != commit:
            parser.error(f"Commit incorreto: {name}")
        dirty = subprocess.check_output(["git", "-C", path, "diff", "--name-only", "HEAD"], text=True).strip()
        if dirty:
            parser.error(f"Fontes alteradas: {name}")
        subprocess.run(["git", "-C", str(path), "archive", "--format=tar.gz", "--prefix=" + name + "/", "-o", str(sources / (name + ".tar.gz")), commit], check=True)
    with tarfile.open(sources / "project.tar.gz", "w:gz") as archive:
        for name in ("src", "tools", "tests", "README.md", "LICENSE", "requirements-dev.txt", ".gitignore"):
            path = ROOT / name
            paths = sorted(path.rglob("*")) if path.is_dir() else [path]
            for entry in paths:
                if entry.is_file() and "__pycache__" not in entry.parts:
                    archive.add(entry, arcname="DWF-TO-DWG/" + str(entry.relative_to(ROOT)))
    (sources / "LEIA-ME.txt").write_text(
        "Fontes correspondentes dos dois executaveis e de suas dependencias.\n"
        "Extraia project.tar.gz. tools/build.py reproduz a compilacao em Linux\n"
        "e busca por HTTPS os mesmos commits listados em VERSIONS.json.\n"
        "Os arquivos toolkit.tar.gz, libredwg.tar.gz e zlib.tar.gz contem\n"
        "copias completas desses fontes, sem credenciais nem metadados Git.\n"
        "Veja README.md para comandos e licencas.\n", encoding="utf-8")
    def included(p):
        return p.is_file() and p.suffix.lower() not in {".pdb", ".lib"}
    files = sorted(p for p in dist.rglob("*") if included(p) and p.name != "SHA256SUMS.txt")
    (dist / "SHA256SUMS.txt").write_text("".join(hashlib.sha256(p.read_bytes()).hexdigest() + "  " + p.relative_to(dist).as_posix() + "\n" for p in files))
    output = ROOT / "dist/DWF-3D-para-DWG-Windows-x64.zip"
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in sorted(dist.rglob("*")):
            if included(path):
                archive.write(path, "DWF-3D/" + path.relative_to(dist).as_posix())
    print(json.dumps({"path": str(output), "bytes": output.stat().st_size, "sha256": hashlib.sha256(output.read_bytes()).hexdigest()}, indent=2))


if __name__ == "__main__":
    main()
