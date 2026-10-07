#!/usr/bin/env python3
"""Configure and build PTPN.

If vcpkg is missing, clones and bootstraps it under ~/vcpkg by default,
then runs CMake (Ninja) with a host-matching vcpkg triplet.

Examples:
  ./build.py
  ./build.py --test
  ./build.py --clean --triplet arm64-osx
  ./build.py --vcpkg-root /opt/vcpkg --build-dir build-asan
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DEFAULT_BUILD_DIR = ROOT / "build"
BUILD_DIR_MARKER = ROOT / ".ptpn-build-dir"

DEFAULT_VCPKG_ROOT = Path.home() / "vcpkg"
VCPKG_REPO = "https://github.com/microsoft/vcpkg.git"


def log(msg: str) -> None:
    print(f"[*] {msg}", flush=True)


def run(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    display = " ".join(cmd)
    log(f"run: {display}")
    proc = subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        env=env,
        text=True,
        check=False,
    )
    if check and proc.returncode != 0:
        raise subprocess.CalledProcessError(proc.returncode, cmd)
    return proc


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if not path:
        raise RuntimeError(
            f"required tool not found: {name}\n"
            f"  macOS: brew install {name}\n"
            f"  Debian/Ubuntu: sudo apt install {name}"
        )
    return path


def detect_triplet(explicit: str | None) -> str:
    if explicit:
        return explicit

    system = platform.system()
    machine = platform.machine().lower()
    arm = machine in {"arm64", "aarch64"}

    if system == "Darwin":
        return "arm64-osx" if arm else "x64-osx"
    if system == "Linux":
        return "arm64-linux" if arm else "x64-linux"
    if system == "Windows":
        return "arm64-windows" if arm else "x64-windows"

    raise RuntimeError(
        f"unsupported host platform: {system} {machine}; pass --triplet explicitly"
    )


def vcpkg_executable(root: Path) -> Path:
    name = "vcpkg.exe" if os.name == "nt" else "vcpkg"
    return root / name


def vcpkg_is_ready(root: Path) -> bool:
    return (
        vcpkg_executable(root).is_file()
        and (root / "scripts" / "buildsystems" / "vcpkg.cmake").is_file()
    )


def ensure_vcpkg(root: Path) -> Path:
    root = root.expanduser().resolve()

    if vcpkg_is_ready(root):
        log(f"vcpkg ready at {root}")
        return root

    require_tool("git")

    if not (root / ".git").is_dir():
        log(f"cloning vcpkg into {root}")
        root.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", VCPKG_REPO, str(root)], cwd=root.parent)

    bootstrap = root / (
        "bootstrap-vcpkg.bat" if os.name == "nt" else "bootstrap-vcpkg.sh"
    )
    if not bootstrap.is_file():
        raise RuntimeError(f"vcpkg bootstrap script missing: {bootstrap}")

    log(f"bootstrapping vcpkg in {root}")
    if os.name == "nt":
        run([str(bootstrap), "-disableMetrics"], cwd=root)
    else:
        run(["bash", str(bootstrap), "-disableMetrics"], cwd=root)

    if not vcpkg_is_ready(root):
        raise RuntimeError(f"vcpkg bootstrap failed under {root}")

    log(f"vcpkg installed at {root}")
    return root


def cmake_env(vcpkg_root: Path, triplet: str) -> dict[str, str]:
    env = os.environ.copy()
    env["VCPKG_ROOT"] = str(vcpkg_root)
    env["VCPKG_DEFAULT_TRIPLET"] = triplet
    return env


def configure(
    *,
    build_dir: Path,
    vcpkg_root: Path,
    triplet: str,
    build_type: str,
    fresh: bool,
) -> None:
    require_tool("cmake")
    require_tool("ninja")

    build_dir.mkdir(parents=True, exist_ok=True)
    env = cmake_env(vcpkg_root, triplet)

    cmd = [
        "cmake",
        "-S",
        str(ROOT),
        "-B",
        str(build_dir),
        "-G",
        "Ninja",
        f"-DCMAKE_TOOLCHAIN_FILE={vcpkg_root / 'scripts' / 'buildsystems' / 'vcpkg.cmake'}",
        f"-DVCPKG_TARGET_TRIPLET={triplet}",
        f"-DCMAKE_BUILD_TYPE={build_type}",
    ]
    if fresh:
        cmd.append("--fresh")

    run(cmd, env=env, cwd=ROOT)


def build_project(build_dir: Path, jobs: int | None) -> None:
    cmd = ["cmake", "--build", str(build_dir)]
    if jobs is not None:
        cmd.extend(["-j", str(jobs)])
    run(cmd, cwd=ROOT)


def run_tests(build_dir: Path) -> None:
    run(["ctest", "--test-dir", str(build_dir), "--output-on-failure"], cwd=ROOT)


def write_build_dir_marker(build_dir: Path) -> None:
    build_dir = build_dir.expanduser().resolve()
    try:
        stored = str(build_dir.relative_to(ROOT))
    except ValueError:
        stored = str(build_dir)
    BUILD_DIR_MARKER.write_text(stored + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--vcpkg-root",
        type=Path,
        default=DEFAULT_VCPKG_ROOT,
        help=f"vcpkg install directory (default: {DEFAULT_VCPKG_ROOT})",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=DEFAULT_BUILD_DIR,
        help=f"CMake build directory (default: {DEFAULT_BUILD_DIR})",
    )
    parser.add_argument(
        "--triplet",
        help="vcpkg target triplet (default: auto-detect from host CPU/OS)",
    )
    parser.add_argument(
        "--build-type",
        default="Release",
        choices=("Debug", "Release", "RelWithDebInfo", "MinSizeRel"),
        help="CMake build type (default: Release)",
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=None,
        help="parallel build jobs (default: cmake/ninja default)",
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="remove build directory before configuring",
    )
    parser.add_argument(
        "--fresh",
        action="store_true",
        help="pass --fresh to cmake configure (ignore cache)",
    )
    parser.add_argument(
        "--skip-vcpkg",
        action="store_true",
        help="do not install/bootstrap vcpkg (use system Boost if available)",
    )
    parser.add_argument(
        "--configure-only",
        action="store_true",
        help="run cmake configure only, skip build",
    )
    parser.add_argument(
        "--test",
        action="store_true",
        help="run ctest after a successful build",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    build_dir = args.build_dir.expanduser().resolve()
    triplet = detect_triplet(args.triplet)

    log(f"host: {platform.system()} {platform.machine()}")
    log(f"vcpkg triplet: {triplet}")

    try:
        if args.clean and build_dir.exists():
            log(f"removing {build_dir}")
            shutil.rmtree(build_dir)

        vcpkg_root: Path | None = None
        if not args.skip_vcpkg:
            vcpkg_root = ensure_vcpkg(args.vcpkg_root.expanduser())
        else:
            log("skipping vcpkg setup (--skip-vcpkg)")

        if args.skip_vcpkg:
            require_tool("cmake")
            require_tool("ninja")
            build_dir.mkdir(parents=True, exist_ok=True)
            cmd = [
                "cmake",
                "-S",
                str(ROOT),
                "-B",
                str(build_dir),
                "-G",
                "Ninja",
                f"-DCMAKE_BUILD_TYPE={args.build_type}",
            ]
            if args.fresh:
                cmd.append("--fresh")
            run(cmd, cwd=ROOT)
        else:
            assert vcpkg_root is not None
            configure(
                build_dir=build_dir,
                vcpkg_root=vcpkg_root,
                triplet=triplet,
                build_type=args.build_type,
                fresh=args.fresh,
            )

        if args.configure_only:
            write_build_dir_marker(build_dir)
            log(f"recorded build directory in {BUILD_DIR_MARKER}")
            log("configure-only; done")
            return 0

        build_project(build_dir, args.jobs)

        write_build_dir_marker(build_dir)
        log(f"recorded build directory in {BUILD_DIR_MARKER}")

        ptpn_bin = build_dir / ("ptpn.exe" if os.name == "nt" else "ptpn")
        if ptpn_bin.is_file():
            log(f"built: {ptpn_bin}")
        else:
            log(f"warning: expected binary not found: {ptpn_bin}")

        if args.test:
            run_tests(build_dir)
            log("tests passed")

    except subprocess.CalledProcessError as exc:
        print(f"error: command failed with exit code {exc.returncode}", file=sys.stderr)
        return exc.returncode or 1
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
