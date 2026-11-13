"""Prepare pinned Windows SDKs for CI; no training checkout is required."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import urllib.request
import zipfile


DOWNLOADS = {
    "slint.exe": (
        "https://github.com/slint-ui/slint/releases/download/v1.16.1/Slint-cpp-1.16.1-win64-MSVC-AMD64.exe",
        "c8c5c1586b8caebc32fcffed832b23b3455b05561e6f885c45099ae300be777a",
    ),
    "onnxruntime.nupkg": (
        "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg",
        "57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681",
    ),
    "directml.nupkg": (
        "https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg",
        "4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9",
    ),
}
MODEL_FILES = ("context.onnx", "decoder.onnx", "critic.onnx", "metadata.json", "export_verification.json")


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify_models(root: Path) -> None:
    bundles = [root / "models"]
    if (root / "model2").exists():
        bundles.append(root / "model2")
    for bundle in bundles:
        manifest = json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))
        for filename in MODEL_FILES:
            if digest(bundle / filename) != manifest["files"][filename]:
                raise ValueError(f"Model checksum mismatch: {bundle.name}/{filename}")
        print(f"Verified {bundle.name}")


def download(destination: Path, name: str) -> Path:
    url, expected = DOWNLOADS[name]
    path = destination / name
    if path.exists():
        if digest(path) != expected:
            raise ValueError(f"Cached download checksum mismatch: {name}")
        return path
    temporary = path.with_suffix(path.suffix + ".partial")
    try:
        with urllib.request.urlopen(url, timeout=120) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        if digest(temporary) != expected:
            raise ValueError(f"Download checksum mismatch: {name}")
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)
    return path


def copy_member(archive: zipfile.ZipFile, member: str, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    with archive.open(member) as source, target.open("wb") as output:
        shutil.copyfileobj(source, output)


def prepare(destination: Path) -> tuple[Path, Path]:
    destination.mkdir(parents=True, exist_ok=True)
    downloads = {name: download(destination, name) for name in DOWNLOADS}
    slint = destination / "slint"
    seven_zip = shutil.which("7z") or str(Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "7-Zip/7z.exe")
    subprocess.run([seven_zip, "x", "-y", "-bso0", "-bsp0", f"-o{slint}", str(downloads["slint.exe"])], check=True)
    runtime = destination / "onnxruntime"
    with zipfile.ZipFile(downloads["onnxruntime.nupkg"]) as archive:
        for name in archive.namelist():
            if name.startswith("build/native/include/") and name.endswith(".h"):
                copy_member(archive, name, runtime / "include" / Path(name).name)
        for name in ("onnxruntime.dll", "onnxruntime.lib", "onnxruntime_providers_shared.dll"):
            copy_member(archive, "runtimes/win-x64/native/" + name, runtime / "lib" / name)
        for name in ("LICENSE", "ThirdPartyNotices.txt"):
            copy_member(archive, name, runtime / name)
    with zipfile.ZipFile(downloads["directml.nupkg"]) as archive:
        copy_member(archive, "bin/x64-win/DirectML.dll", runtime / "lib/DirectML.dll")
        copy_member(archive, "LICENSE.txt", runtime / "DirectML-LICENSE")
    for path in (slint / "bin/slint-compiler.exe", slint / "lib/cmake/Slint/SlintConfig.cmake",
                 runtime / "include/onnxruntime_cxx_api.h", runtime / "lib/DirectML.dll"):
        if not path.is_file():
            raise FileNotFoundError(path)
    return slint, runtime


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()
    verify_models(Path(__file__).resolve().parents[1])
    slint, runtime = prepare(args.destination.resolve())
    if os.environ.get("GITHUB_ENV"):
        with Path(os.environ["GITHUB_ENV"]).open("a", encoding="utf-8") as output:
            output.write(f"SLINT_ROOT={slint.as_posix()}\nONNXRUNTIME_ROOT={runtime.as_posix()}\n")
    print(f"Slint: {slint}\nONNX Runtime: {runtime}")


if __name__ == "__main__":
    main()
