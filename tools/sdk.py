"""Build and verify a self-contained Windows TabEngine SDK.

Skia source and all network fetches are owned by this repository. Consumers
receive a pinned DLL, import library, headers, and CMake package instead of
running GN or selecting their own Skia build.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import locale
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[1]
SKIA = ROOT / "third_party" / "skia"
REVISION = (ROOT / "third_party" / "SKIA_REVISION.txt").read_text(encoding="utf-8").strip()
SKIA_URL = "https://github.com/google/skia.git"
EXTERNALS = (
    ("zlib", "646b7f569718921d7d4b5b8e22572ff6c76f2596", ("https://github.com/skia4delphi/zlib.git", "https://chromium.googlesource.com/chromium/src/third_party/zlib")),
    ("libpng", "d5515b5b8be3901aac04e5bd8bd5c89f287bcd33", ("https://github.com/pnggroup/libpng.git", "https://skia.googlesource.com/third_party/libpng.git")),
    ("freetype", "264b5fbf5b912b39f98d038bf75d39be0a73f21b", ("https://github.com/freetype/freetype.git", "https://chromium.googlesource.com/chromium/src/third_party/freetype2.git")),
    ("d3d12allocator", "169895d529dfce00390a20e69c2f516066fe7a3b", ("https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator.git", "https://skia.googlesource.com/external/github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator.git")),
    ("spirv-cross", "b8fcf307f1f347089e3c46eb4451d27f32ebc8d3", ("https://github.com/KhronosGroup/SPIRV-Cross.git", "https://chromium.googlesource.com/external/github.com/KhronosGroup/SPIRV-Cross")),
)
LICENSES = (
    ("LICENSE", "skia/LICENSE.txt"),
    ("modules/skcms/LICENSE", "skcms/LICENSE.txt"),
    ("third_party/externals/d3d12allocator/LICENSE.txt", "d3d12allocator/LICENSE.txt"),
    ("third_party/externals/d3d12allocator/NOTICES.txt", "d3d12allocator/NOTICES.txt"),
    ("third_party/externals/freetype/LICENSE.TXT", "freetype/LICENSE.TXT"),
    ("third_party/externals/libpng/LICENSE", "libpng/LICENSE.txt"),
    ("third_party/externals/spirv-cross/LICENSE", "spirv-cross/LICENSE.txt"),
    ("third_party/externals/spirv-cross/LICENSES/LicenseRef-KhronosFreeUse.txt", "spirv-cross/LicenseRef-KhronosFreeUse.txt"),
    ("third_party/externals/zlib/LICENSE", "zlib/LICENSE.txt"),
)


def run(*args: str, cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    print("+", subprocess.list2cmdline(args), flush=True)
    subprocess.run(args, cwd=cwd, env=env, check=True)


def run_logged(log: Path, *args: str, cwd: Path | None = None,
               env: dict[str, str] | None = None) -> None:
    log.parent.mkdir(parents=True, exist_ok=True)
    print("+", subprocess.list2cmdline(args), f"(log: {log})", flush=True)
    with log.open("w", encoding="utf-8", errors="replace") as output:
        result = subprocess.run(args, cwd=cwd, env=env, stdout=output,
                                stderr=subprocess.STDOUT, check=False)
    lines = log.read_text(encoding=locale.getpreferredencoding(False), errors="replace").splitlines()
    if result.returncode:
        print("\n".join(lines[-60:]).encode("ascii", errors="replace").decode("ascii"), file=sys.stderr)
        raise SystemExit(f"Command failed with exit code {result.returncode}; see {log}")
    print("\n".join(lines[-8:]).encode("ascii", errors="replace").decode("ascii"))


def capture(*args: str, cwd: Path | None = None) -> str:
    return subprocess.check_output(args, cwd=cwd, text=True, encoding="utf-8").strip()


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def checkout(directory: Path, revision: str, urls: tuple[str, ...]) -> None:
    if not (directory / ".git").is_dir():
        directory.mkdir(parents=True, exist_ok=True)
        run("git", "init", "-q", str(directory))
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=directory,
                          capture_output=True, text=True, encoding="utf-8", check=False)
    current = head.stdout.strip() if head.returncode == 0 else ""
    if current == revision:
        if capture("git", "status", "--porcelain", "--untracked-files=no", cwd=directory):
            raise SystemExit(f"Pinned dependency has modified tracked files: {directory}")
        return
    if current and capture("git", "status", "--porcelain", cwd=directory):
        raise SystemExit(f"Refusing to replace modified dependency checkout: {directory}")
    for url in urls:
        result = subprocess.run(["git", "-c", "http.sslBackend=openssl", "fetch", "--depth", "1", url, revision], cwd=directory, check=False)
        if result.returncode == 0:
            run("git", "checkout", "--detach", "FETCH_HEAD", cwd=directory)
            if capture("git", "rev-parse", "HEAD", cwd=directory) != revision:
                raise SystemExit(f"Revision mismatch in {directory}")
            return
    raise SystemExit(f"Could not fetch {revision} into {directory}")


def fetch() -> None:
    checkout(SKIA, REVISION, (SKIA_URL,))
    if not (SKIA / "BUILD.gn").is_file():
        raise SystemExit("Skia checkout is incomplete")
    deps = (SKIA / "DEPS").read_text(encoding="utf-8")
    for name, revision, urls in EXTERNALS:
        match = re.search(r'"third_party/externals/' + re.escape(name) +
                          r'"\s*:\s*"[^"\n]+@([0-9a-f]{40})"', deps)
        if not match or match.group(1) != revision:
            raise SystemExit(f"Pinned {name} revision does not match Skia DEPS")
        checkout(SKIA / "third_party" / "externals" / name, revision, urls)
    if not (SKIA / "bin" / "gn.exe").is_file():
        run(sys.executable, "bin/fetch-gn", cwd=SKIA)


def msbuild_path() -> Path:
    found = shutil.which("MSBuild.exe")
    if found:
        return Path(found)
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.is_file():
        result = capture(str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.Component.MSBuild", "-find", r"MSBuild\Current\Bin\MSBuild.exe")
        if result:
            return Path(result.splitlines()[0])
    raise SystemExit("MSBuild was not found; install Visual Studio with C++ build tools")


def librarian_path() -> Path:
    found = shutil.which("lib.exe")
    if found:
        return Path(found)
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.is_file():
        result = capture(str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-find", r"VC\Tools\MSVC\*\bin\Hostx64\x64\lib.exe")
        if result:
            return Path(result.splitlines()[0])
    raise SystemExit("MSVC lib.exe was not found")


def skia_output(config: str) -> Path:
    return SKIA / "out" / f"tabengine-{config.lower()}-shared"


def shared_package(config: str) -> Path:
    return ROOT / "build" / "skia-shared" / config


def reset_generated_dir(path: Path, expected_parent: Path) -> None:
    resolved = path.resolve()
    if resolved.parent != expected_parent.resolve():
        raise SystemExit(f"Refusing to clean unexpected generated directory: {resolved}")
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)


def locate_artifact(output: Path, names: tuple[str, ...]) -> Path:
    for name in names:
        direct = output / name
        if direct.is_file():
            return direct
    for name in names:
        matches = sorted(output.glob(f"**/{name}"), key=lambda p: len(p.parts))
        if matches:
            return matches[0]
    raise SystemExit(f"Missing {' or '.join(names)} under {output}")


def build_skia(config: str) -> Path:
    if os.name != "nt":
        raise SystemExit("The current SDK builder supports Windows only")
    fetch()
    output = skia_output(config)
    output.mkdir(parents=True, exist_ok=True)
    args = (ROOT / "cmake" / "skia_shared_args.gn").read_text(encoding="utf-8")
    if config == "Release":
        args = args.replace("is_official_build = false", "is_official_build = true", 1)
    args = (f'is_debug = {str(config == "Debug").lower()}\n'
            'is_trivial_abi = false\n'
            f'extra_cflags = ["{("/MDd" if config == "Debug" else "/MD")}"]\n'
            + args)
    (output / "args.gn").write_text(args, encoding="utf-8", newline="\n")
    dotfile = SKIA / ".gn"
    original = dotfile.read_bytes()
    marker = 'script_executable = "python3"'
    dotfile_text = original.decode("utf-8")
    with tempfile.TemporaryDirectory(prefix="tabengine-python3-") as temp:
        shim = Path(temp)
        (shim / "python3.cmd").write_text(
            f'@echo off\r\n"{Path(sys.executable).resolve()}" %*\r\n',
            encoding="utf-8", newline="")
        env = os.environ.copy()
        env["PATH"] = str(shim) + os.pathsep + env.get("PATH", "")
        if marker in dotfile_text:
            dotfile.write_text(dotfile_text.replace(marker, f'script_executable = "{Path(sys.executable).resolve().as_posix()}"', 1), encoding="utf-8", newline="\n")
        try:
            run(str(SKIA / "bin" / "gn.exe"), "gen", str(output), "--ide=vs", cwd=SKIA, env=env)
            project = output / "obj" / "skia.vcxproj"
            if not project.is_file():
                raise SystemExit(f"GN did not generate {project}")
            run_logged(ROOT / "build" / "logs" / f"skia-{config.lower()}.log",
                str(msbuild_path()), str(project), "/m", "/t:Build",
                "/p:Configuration=GN", "/p:Platform=x64",
                f"/p:SolutionDir={output}{os.sep}", cwd=SKIA, env=env)
        finally:
            dotfile.write_bytes(original)
    dll = locate_artifact(output, ("skia.dll",))
    library = locate_artifact(output, ("skia.dll.lib", "skia.lib"))
    package = shared_package(config)
    reset_generated_dir(package, ROOT / "build" / "skia-shared")
    (package / "bin").mkdir(parents=True, exist_ok=True)
    (package / "lib").mkdir(parents=True, exist_ok=True)
    shutil.copy2(dll, package / "bin" / "skia.dll")
    shutil.copy2(library, package / "lib" / "skia.lib")
    for component in ("freetype2", "libpng"):
        shutil.copy2(locate_artifact(output, (f"{component}.lib",)),
                     package / "lib" / f"{component}.lib")
    # GN leaves zlib's SIMD source sets outside zlib.lib because skia.dll links
    # their objects directly. Repack them so SDK consumers can link FreeType
    # and libpng without reaching into the Skia build tree.
    zlib_objects = (
        "zlib_adler32_simd.adler32_simd.obj",
        "zlib_crc32_simd.crc32_simd.obj",
        "zlib_crc32_simd.crc_folding.obj",
        "zlib_inflate_chunk_simd.inffast_chunk.obj",
        "zlib_inflate_chunk_simd.inflate.obj",
    )
    run(str(librarian_path()), "/NOLOGO", f"/OUT:{package / 'lib' / 'zlib.lib'}",
        str(locate_artifact(output, ("zlib.lib",))),
        *(str(locate_artifact(output, (name,))) for name in zlib_objects))
    symbol_file = output / "skia.dll.pdb"
    if symbol_file.is_file():
        (package / "symbols").mkdir(parents=True, exist_ok=True)
        shutil.copy2(symbol_file, package / "symbols" / symbol_file.name)
    shutil.copy2(output / "args.gn", package / "skia_args.gn")
    shutil.copytree(SKIA / "include", package / "include", dirs_exist_ok=True)
    shutil.copytree(SKIA / "modules" / "skcms", package / "modules" / "skcms", dirs_exist_ok=True)
    freetype = SKIA / "third_party" / "externals" / "freetype" / "include"
    shutil.copytree(freetype, package / "third_party" / "freetype" / "include", dirs_exist_ok=True)
    for source, target in LICENSES:
        src = SKIA / source
        if not src.is_file():
            raise SystemExit(f"Missing license: {src}")
        dest = package / "licenses" / target
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)
    (package / "SKIA_BUILD_INFO.txt").write_text(
        f"skia_revision={REVISION}\nconfiguration={config}\n"
        "is_component_build=true\narchitecture=x64\n"
        f"gn_args_sha256={hashlib.sha256(args.encode()).hexdigest()}\n",
        encoding="utf-8")
    print(f"Shared Skia package: {package}")
    return package


def sdk_stage(config: str) -> Path:
    return ROOT / "build" / "sdk-stage" / config


def package_sdk(config: str, skia_root: Path | None = None) -> Path:
    skia_root = (skia_root or shared_package(config)).resolve()
    if not (skia_root / "bin" / "skia.dll").is_file():
        raise SystemExit(f"Shared Skia is missing: run build-skia --config {config}")
    build = ROOT / "build" / "sdk-build" / config
    stage = sdk_stage(config)
    reset_generated_dir(stage, ROOT / "build" / "sdk-stage")
    run("cmake", "-S", str(ROOT), "-B", str(build), "-G", "Visual Studio 18 2026", "-A", "x64",
        "-DBUILD_TESTING=OFF", "-DTABENGINE_BUILD_SKIA=ON", "-DTABENGINE_INSTALL_SDK=ON",
        f"-DTABENGINE_SKIA_ROOT={skia_root}")
    run("cmake", "--build", str(build), "--config", config,
        "--target", "tabengine_core", "tabengine_win32", "tabengine_skia")
    run_logged(ROOT / "build" / "logs" / f"sdk-install-{config.lower()}.log",
               "cmake", "--install", str(build), "--config", config,
               "--prefix", str(stage))
    source_licenses = skia_root / "licenses"
    if not source_licenses.is_dir():
        raise SystemExit(f"Skia license bundle is missing: {source_licenses}")
    shutil.copytree(source_licenses, stage / "licenses", dirs_exist_ok=True)
    for name in ("SKIA_BUILD_INFO.txt", "skia_args.gn"):
        shutil.copy2(skia_root / name, stage / name)
    version_match = re.search(r"project\(TabEngine VERSION ([0-9.]+)", (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))
    version = version_match.group(1) if version_match else "unknown"
    files = sorted(p for p in stage.rglob("*") if p.is_file() and p.name != "manifest.json")
    manifest = {
        "name": "TabEngine SDK",
        "version": version,
        "skia_revision": REVISION,
        "skia_externals": {name: revision for name, revision, _ in EXTERNALS},
        "skia_gn_args_sha256": hashlib.sha256((stage / "skia_args.gn").read_bytes()).hexdigest(),
        "configuration": config,
        "platform": "windows-x64",
        "files": {p.relative_to(stage).as_posix(): hash_file(p) for p in files},
    }
    (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    archive = ROOT / "dist" / f"tabengine-sdk-{version}-windows-x64-{config.lower()}.zip"
    archive.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
        for path in sorted(stage.rglob("*")):
            if path.is_file():
                bundle.write(path, Path(archive.stem) / path.relative_to(stage))
    print(f"TabEngine SDK: {archive}")
    symbol_sources = list((skia_root / "symbols").glob("*.pdb"))
    symbol_sources.extend((build / config).glob("tabengine_*.pdb"))
    symbol_archive = archive.with_name(archive.stem + "-symbols.zip")
    if symbol_sources:
        symbols = ROOT / "build" / "sdk-symbols" / config
        reset_generated_dir(symbols, ROOT / "build" / "sdk-symbols")
        for source in symbol_sources:
            shutil.copy2(source, symbols / source.name)
        symbol_manifest = {
            "sdk_archive": archive.name,
            "sdk_sha256": hash_file(archive),
            "files": {p.name: hash_file(p)
                      for p in sorted(symbols.glob("*.pdb"))},
        }
        (symbols / "manifest.json").write_text(json.dumps(symbol_manifest, indent=2) + "\n", encoding="utf-8")
        with zipfile.ZipFile(symbol_archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
            for path in sorted(symbols.iterdir()):
                bundle.write(path, Path(symbol_archive.stem) / path.name)
        print(f"Debug symbols: {symbol_archive}")
    elif symbol_archive.is_file():
        symbol_archive.unlink()
    return stage


def verify(config: str) -> None:
    version_match = re.search(r"project\(TabEngine VERSION ([0-9.]+)", (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))
    version = version_match.group(1) if version_match else "unknown"
    archive = ROOT / "dist" / f"tabengine-sdk-{version}-windows-x64-{config.lower()}.zip"
    if not archive.is_file():
        raise SystemExit(f"SDK archive is missing: {archive}")
    (ROOT / "build").mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="sdk-verify-", dir=ROOT / "build") as temp:
        with zipfile.ZipFile(archive) as bundle:
            bundle.extractall(temp)
        stage = Path(temp) / archive.stem
        verify_extracted_sdk(config, stage, Path(temp) / "consumer-build")
        symbols_zip = archive.with_name(archive.stem + "-symbols.zip")
        if symbols_zip.is_file():
            with zipfile.ZipFile(symbols_zip) as bundle:
                bundle.extractall(Path(temp) / "symbols")
            symbols = Path(temp) / "symbols" / symbols_zip.stem
            symbol_manifest = json.loads((symbols / "manifest.json").read_text(encoding="utf-8"))
            if symbol_manifest["sdk_sha256"] != hash_file(archive):
                raise SystemExit("Symbol archive does not match the SDK archive")
            for relative, expected in symbol_manifest["files"].items():
                if hash_file(symbols / relative) != expected:
                    raise SystemExit(f"Symbol archive file mismatch: {relative}")
            print("SDK symbols passed")


def verify_extracted_sdk(config: str, stage: Path, build: Path) -> None:
    manifest_path = stage / "manifest.json"
    if not manifest_path.is_file():
        raise SystemExit(f"SDK manifest is missing: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for relative, expected in manifest["files"].items():
        path = stage / relative
        if not path.is_file() or hash_file(path) != expected:
            raise SystemExit(f"SDK file mismatch: {relative}")
    consumer_src = build.parent / "consumer-src"
    shutil.copytree(ROOT / "tests" / "sdk_consumer", consumer_src)
    run("cmake", "-S", str(consumer_src), "-B", str(build),
        "-G", "Visual Studio 18 2026", "-A", "x64", f"-DCMAKE_PREFIX_PATH={stage}")
    run("cmake", "--build", str(build), "--config", config)
    exe = build / config / "tabengine_sdk_consumer.exe"
    env = os.environ.copy()
    env["PATH"] = str(stage / "bin") + os.pathsep + env.get("PATH", "")
    run(str(exe), env=env)
    print("SDK external consumer passed")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("fetch", "build-skia", "package", "verify", "release"))
    parser.add_argument("--config", choices=("Debug", "Release"), default="Debug")
    parser.add_argument("--skia-root", type=Path, help="Prebuilt shared Skia package for package action")
    args = parser.parse_args()
    if args.action == "fetch":
        fetch()
    elif args.action == "build-skia":
        build_skia(args.config)
    elif args.action == "package":
        package_sdk(args.config, args.skia_root)
    elif args.action == "verify":
        verify(args.config)
    else:
        build_skia(args.config)
        package_sdk(args.config)
        verify(args.config)


if __name__ == "__main__":
    main()
