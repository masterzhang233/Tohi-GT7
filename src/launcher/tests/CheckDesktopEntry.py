#!/usr/bin/env python3
"""Offline desktop-entry checks. Fixture executables are never started."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


RUNTIME = ("kyty_dlss_backend.dll", "nvngx_dlss.dll")
REQUIRED = ("launcher.exe", "kyty_emulator.exe", *RUNTIME)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--powershell", required=True, help="Explicit PowerShell executable")
    args = parser.parse_args()
    host = shutil.which(args.powershell) or args.powershell
    source = Path(__file__).resolve().parents[3]
    script = source / "scripts/Start-GT7-Desktop.ps1"
    checks = 0
    with tempfile.TemporaryDirectory(prefix="GT7 desktop fixture ") as temp:
        install = Path(temp) / "release with spaces"
        install.mkdir()
        for name in REQUIRED:
            (install / name).write_bytes(b"not an executable; CheckOnly fixture")

        def run():
            return subprocess.run(
                [host, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script),
                 "-InstallDirectory", str(install), "-CheckOnly"],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30,
            )

        result = run()
        assert result.returncode == 0, result.stderr
        report = json.loads(result.stdout.lstrip("\ufeff"))
        assert report["started"] is False and report["save_files_modified"] is False
        assert report["required_files_present"] is True
        assert Path(report["executable"]) == install / "launcher.exe"
        assert all(text in report["graphics_preset"] for text in (
            "2560x1440", "3840x2160", "frame generation disabled", "no extra FPS cap", "RTX 40/50"))
        assert "NVIDIA fixed 2X" not in report["graphics_preset"]
        checks += 1
        for name in REQUIRED:
            path = install / name
            path.unlink()
            result = run()
            assert result.returncode != 0 and name in result.stderr, (name, result)
            checks += 1
            path.mkdir()
            result = run()
            assert result.returncode != 0 and name in result.stderr, (name, result)
            checks += 1
            path.rmdir()
            path.write_bytes(b"not an executable; CheckOnly fixture")
    print(json.dumps({"status": "PASS", "checks": checks, "powershell": host,
                      "game_started": False, "gpu_used": False}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
