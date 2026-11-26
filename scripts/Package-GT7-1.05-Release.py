"""Turn a verified 1.0.5 staging directory into a portable release and verify it.

Run Package-GT7-1.05-Candidate.ps1 first against the exact clean release build.
This step preserves third-party notices and records the frozen helper's distinct
source provenance; it does not assert that those licenses have one common scope.
No game, personal configuration, cache, or log is selected for publication.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import zipfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def safe_path(name):
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or "\\" in name or ":" in name:
        raise ValueError(f"Unsafe member: {name}")
    if any(part.lower() in {"_savedata", "_downloaddata", "_trophies", "cache", "caches", "logs", "captures"}
           for part in path.parts) or path.suffix.lower() in {".pdb", ".map", ".log", ".dmp", ".bin"}:
        raise ValueError(f"Private or development file: {name}")
    return path


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--fg-source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    def git(*argv):
        return subprocess.check_output(["git", "-C", str(source), *argv], text=True).strip()
    if git("rev-parse", "HEAD") != args.source_commit or git("status", "--porcelain"):
        raise ValueError("Packaging requires the exact clean release commit")
    if git("rev-parse", "v1.0.5^{commit}") != args.source_commit:
        raise ValueError("Release tag does not identify this source")
    if args.output.exists():
        raise ValueError("Output directory must not already exist")
    staging = json.loads((args.candidate / "local-candidate-manifest.json").read_text(encoding="utf-8-sig"))
    if staging["version"] != "1.0.5" or staging["framegen_executable_sha256"] != "9b9442d8791f6c36385ffd7282e64ace40cbb65f0cb77bfffc29e01aa76ee92e":
        raise ValueError("Unexpected staging policy or frame-generation build")
    payload = {}
    for record in staging["payload_files"]:
        name = record["path"]
        safe_path(name)
        path = args.candidate / name
        if path.is_symlink() or name in payload:
            raise ValueError("Linked or duplicate payload")
        data = path.read_bytes()
        if len(data) != record["bytes"] or digest(data) != record["sha256"]:
            raise ValueError(f"Staging content changed: {name}")
        payload[name] = data
    if len(payload) != 99:
        raise ValueError("Unexpected frozen staging file set")
    if payload["docs/RELEASE-1.0.5.zh-CN.md"] != (source / "docs/RELEASE-1.0.5.zh-CN.md").read_bytes():
        raise ValueError("Release notes do not match committed source")
    fg_source_data = args.fg_source.read_bytes()
    with zipfile.ZipFile(args.fg_source) as fg_source:
        if fg_source.testzip() is not None:
            raise ValueError("FG corresponding-source ZIP is corrupt")
        names = fg_source.namelist()
        if len(names) != len(set(names)) or not any(n.endswith("src/TohiFrameGen/main.cpp") for n in names):
            raise ValueError("Full modified FG source is missing")
        for name in names:
            path = PurePosixPath(name)
            if path.is_absolute() or ".." in path.parts or "\\" in name or ":" in name:
                raise ValueError("Unsafe corresponding-source path")
    # Retain the original candidate manifest as build provenance, without treating
    # its local-stage policy flags as a new licensing or performance certification.
    payload["framegen/FG-COMPONENT-NOTICE.txt"] = ("""Tohi-GT7 1.0.5 - external capture frame generation

This separate Magpie-derived GPLv3 helper captures the completed game window
through Windows Graphics Capture and uses DLSS Frame Generation at 2X. It does
not receive the game's native depth, motion vectors, or separated HUD resources.
It does not run another upscaler. Fast-moving window edges may exhibit ghosting.

Complete modified helper source and build instructions are available beside the
binary download as TohiFrameGen-1.0.5-source.zip at:
https://github.com/masterzhang233/Tohi-GT7/releases/tag/v1.0.5
The source/ subdirectory also preserves the original change manifest and rebuild
materials for the frozen helper. fg-runtime-manifest.json is the historical local
build-stage provenance, not a certification of legal or performance validation.

Magpie: https://github.com/SAOG0721/Magpie/tree/v0.6.14-experimental
Community FG bundle: https://github.com/sdli1995/dlssg_for_sm86/tree/6f047bdc885e861575e8b049413ca31dc171a924
GPL, MIT, dependency, and NVIDIA notices retain their separate scopes. NVIDIA
runtime and derived kernel assets are excluded from the proxy's MIT license.
Retaining these notices is not a claim of NVIDIA endorsement or an additional
license grant; consult the original terms and accompanying source materials.
""").encode("utf-8")
    private_path = re.compile(rb"[A-Za-z]:[\\/](?:Users|Documents and Settings)[\\/]", re.I)
    for name, data in payload.items():
        if PurePosixPath(name).suffix.lower() in {".txt", ".md", ".json", ".ps1", ".cmd", ".ini"} and private_path.search(data):
            raise ValueError(f"Builder's private path in public text: {name}")
    records = [{"path": n, "bytes": len(d), "sha256": digest(d)} for n, d in sorted(payload.items())]
    metadata = {
        "schema": 1, "product": "Tohi-GT7", "version": "1.0.5", "tag": "v1.0.5",
        "source_commit": args.source_commit,
        "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "user_data_included": False, "game_included": False,
        "nvidia_frame_generation_compiled": False, "fsr_frame_generation_compiled": False,
        "external_frame_generation": {"capture": "WGC", "multiplier": 2, "requested_preset": "auto",
            "actual_preset": "unknown", "helper_sha256": staging["framegen_executable_sha256"],
            "source_asset": "TohiFrameGen-1.0.5-source.zip", "source_sha256": digest(fg_source_data)},
        "reconstruction_input": [2560, 1440], "reconstruction_output": [3840, 2160],
        "extra_fps_cap": 0, "supported_gpu_families": ["GeForce RTX 40", "GeForce RTX 50"],
        "known_issues": ["External FG ghosting at fast-moving car-window edges; disabling FG2X removes it in the observed A/B test."],
        "files": records,
    }
    payload["release-build.json"] = json_bytes(metadata)
    payload["SHA256SUMS.txt"] = ("".join(f"{digest(data)}  {name}\n" for name, data in sorted(payload.items()))).encode("utf-8")
    args.output.mkdir(parents=True)
    archive = args.output / "Tohi-GT7-1.0.5-Windows-x64.zip"
    prefix = "Tohi-GT7-1.0.5-Windows-x64/"
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as release:
        for name, data in sorted(payload.items()):
            release.writestr(prefix + name, data)
    with zipfile.ZipFile(archive) as release:
        if release.testzip() is not None or set(release.namelist()) != {prefix + n for n in payload}:
            raise ValueError("ZIP integrity or file-set validation failed")
        for name, data in payload.items():
            if digest(release.read(prefix + name)) != digest(data):
                raise ValueError(f"Archive round-trip failed: {name}")
    archive_hash = digest(archive.read_bytes())
    (args.output / (archive.name + ".sha256")).write_text(f"{archive_hash}  {archive.name}\n", encoding="utf-8")
    (args.output / "TohiFrameGen-1.0.5-source.zip").write_bytes(fg_source_data)
    (args.output / "TohiFrameGen-1.0.5-source.zip.sha256").write_text(
        f"{digest(fg_source_data)}  TohiFrameGen-1.0.5-source.zip\n", encoding="utf-8")
    metadata.update(archive=archive.name, bytes=archive.stat().st_size, sha256=archive_hash,
                    payload_file_count=len(payload), verification="ZIP CRC, exact file set, every file SHA-256 and corresponding-source archive checked")
    (args.output / "release-manifest.json").write_bytes(json_bytes(metadata))
    print(json.dumps({"archive": archive.name, "bytes": archive.stat().st_size, "sha256": archive_hash,
                      "file_count": len(payload), "source_commit": args.source_commit}))


if __name__ == "__main__":
    main()
