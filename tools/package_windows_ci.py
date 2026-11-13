"""Bundle a fresh Windows CMake install and its app-local VC++ runtime."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import shutil
import zipfile


REQUIRED = (
    "黎明界决策系统.exe", "labyrinth_server.exe", "slint_cpp.dll",
    "onnxruntime.dll", "DirectML.dll", "LICENSE", "THIRD_PARTY_NOTICES.txt",
    "ONNXRUNTIME-LICENSE", "config/labyrinth.json", "data/game_catalog.json",
    "models/context.onnx", "models/decoder.onnx", "models/critic.onnx",
    "models/metadata.json", "models/manifest.json", "models/export_verification.json",
    "README.md", "adb_reverse.cmd", "adb.exe", "AdbWinApi.dll", "AdbWinUsbApi.dll",
)
CRT_FILES = ("msvcp140.dll", "msvcp140_1.dll", "vcruntime140.dll", "vcruntime140_1.dll")


def package(install: Path, crt: Path, runtime: Path, config: Path, output: Path) -> Path:
    if output.resolve().is_relative_to(install.resolve()):
        raise ValueError("Archive output must be outside the install directory")
    missing = [name for name in REQUIRED if not (install / name).is_file()]
    missing += [f"VC++/{name}" for name in CRT_FILES if not (crt / name).is_file()]
    if missing:
        raise FileNotFoundError("Incomplete Windows package: " + ", ".join(missing))
    if (install / "config/labyrinth.json").read_bytes() != config.read_bytes():
        raise ValueError("Installed default config differs from the source config")
    for path in crt.glob("*.dll"):
        shutil.copy2(path, install / path.name)
    shutil.copy2(runtime / "DirectML-LICENSE", install / "DirectML-LICENSE")
    hashes = []
    for path in sorted(crt.glob("*.dll")):
        with path.open("rb") as stream:
            checksum = hashlib.file_digest(stream, "sha256").hexdigest()
            hashes.append(f"{checksum}  {path.name}\n")
    (install / "MICROSOFT-RUNTIME-SOURCE.txt").write_text(
        "Microsoft Visual C++ x64 runtime from Visual Studio 2022.\n"
        f"MSVC redistributable version: {crt.parent.parent.name}\n"
        "Source: Microsoft.VC143.CRT from the official Visual Studio installation.\n"
        "https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files\n"
        + "\nSHA256:\n" + "".join(hashes),
        encoding="utf-8",
    )
    output.mkdir(parents=True, exist_ok=True)
    archive = output / "labyrinth-windows-x64.zip"
    temporary = archive.with_suffix(".zip.partial")
    try:
        with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
            for path in sorted(install.rglob("*")):
                if path.is_file():
                    bundle.write(path, (Path("windows-x64") / path.relative_to(install)).as_posix())
        with zipfile.ZipFile(temporary) as bundle:
            if bundle.testzip():
                raise ValueError("ZIP integrity check failed")
        temporary.replace(archive)
    finally:
        temporary.unlink(missing_ok=True)
    with archive.open("rb") as stream:
        checksum = hashlib.file_digest(stream, "sha256").hexdigest()
    (output / "SHA256SUMS.txt").write_text(f"{checksum}  {archive.name}\n", encoding="utf-8")
    return archive


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install-dir", type=Path, required=True)
    parser.add_argument("--vc-runtime-dir", type=Path, required=True)
    parser.add_argument("--runtime-dir", type=Path, required=True)
    parser.add_argument("--config", type=Path, default=Path("config/labyrinth.json"))
    parser.add_argument("--output-dir", type=Path, default=Path("dist"))
    args = parser.parse_args()
    print(package(args.install_dir, args.vc_runtime_dir, args.runtime_dir, args.config, args.output_dir))


if __name__ == "__main__":
    main()
