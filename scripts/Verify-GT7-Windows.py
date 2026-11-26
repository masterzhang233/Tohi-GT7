"""Verify the portable Windows release ZIP without launching either executable."""

import argparse
import hashlib
import json
import pathlib
import re
import zipfile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=pathlib.Path)
    args = parser.parse_args()
    with zipfile.ZipFile(args.archive) as release:
        names = release.namelist()
        if len(names) != len(set(names)):
            raise ValueError("duplicate ZIP members")
        if release.testzip() is not None:
            raise ValueError("ZIP CRC verification failed")
        roots = {name.split("/")[0] for name in names}
        if len(roots) != 1:
            raise ValueError("release must have one top-level directory")
        root = roots.pop() + "/"
        if not re.fullmatch(r"Tohi-GT7-\d+\.\d+\.\d+-Windows-x64/", root):
            raise ValueError("unexpected release folder name")
        for name in names:
            path = pathlib.PurePosixPath(name)
            if path.is_absolute() or ".." in path.parts or "\\" in name or ":" in name:
                raise ValueError(f"unsafe ZIP member: {name}")
        sums_name = root + "SHA256SUMS.txt"
        records = release.read(sums_name).decode("utf-8").splitlines()
        expected = {}
        for record in records:
            digest, relative = record.split("  ", 1)
            if relative in expected or relative.startswith("/") or ".." in relative.split("/"):
                raise ValueError("invalid checksum manifest entry")
            expected[relative] = digest
        actual = {name[len(root):] for name in names if name.startswith(root)
                  and not name.endswith("/") and name != sums_name}
        if set(expected) != actual:
            raise ValueError("checksum manifest does not cover exactly the bundled files")
        for relative, digest in expected.items():
            if hashlib.sha256(release.read(root + relative)).hexdigest() != digest:
                raise ValueError(f"SHA-256 mismatch: {relative}")
        required = {"launcher.exe", "kyty_emulator.exe", "Qt6Core.dll", "Qt6Gui.dll",
                    "Qt6Network.dll", "Qt6Svg.dll", "Qt6Widgets.dll", "msvcp140.dll",
                    "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140_1.dll", "msvcp140_2.dll",
                    "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll", "icuuc.dll",
                    "libwinpthread-1.dll", "platforms/qwindows.dll",
                    "kyty_dlss_backend.dll", "nvngx_dlss.dll",
                    "Start-GT7-Desktop.ps1", "Start-GT7-Desktop.cmd",
                    "patches/GT7/patches.json", "LICENSE", "licenses/Kyty-MIT.txt",
                    "licenses/qt/Qt-NOTICE.md", "licenses/qt/Qt-LGPL-3.0-only.txt",
                    "licenses/qt/Qt-GPL-3.0-only.txt", "licenses/openvr/LICENSE",
                    "licenses/winpthread/COPYING", "licenses/opus/COPYING",
                    "licenses/ffmpeg/copyright", "licenses/ffmpeg/SOURCE.txt",
                    "licenses/nvidia/LICENSE.txt", "licenses/nvidia/NVIDIA-DLSS-NOTICE.md",
                    "docs/GT7.md", "docs/NATIVE-PSSR-DLSS.md",
                    "patches/GT7/GT7-PSSR-1440-to-4K.json",
                    "fonts/NotoSansCJKsc-Regular.otf", "licenses/fonts/noto-cjk/OFL.txt",
                    "licenses/fonts/noto-cjk/NOTICE.md", "licenses/fonts/noto-cjk/provenance.json",
                    "translations/qt_zh_CN.qm"}
        version = re.fullmatch(r"Tohi-GT7-(\d+\.\d+\.\d+)-Windows-x64/", root)[1]
        if version != "1.0.4":
            raise ValueError("this SR-only package contract targets Tohi-GT7 1.0.4")
        required.add(f"docs/RELEASE-{version}.zh-CN.md")
        if not required.issubset(actual):
            raise ValueError(f"required runtime or notice files are missing: {sorted(required - actual)}")
        vendor_hashes = {
            "kyty_dlss_backend.dll": "60cfdbbc405a641570e30c330638e5e662a271f977638bdd2df36f1d80f06ecf",
            "nvngx_dlss.dll": "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e",
        }
        forbidden_fg_names = {"nvngx_dlssg.dll", "nvlowlatencyvk.dll", "amd_fidelityfx_vk.dll"}
        for name in actual:
            path = pathlib.PurePosixPath(name)
            if (path.name.lower() in forbidden_fg_names or
                    (path.name.lower().startswith("sl.") and path.suffix.lower() == ".dll") or
                    name.lower().startswith(("licenses/nvidia/streamline/", "licenses/fidelityfx/")) or
                    path.name.lower() == "nvidia-streamline-notice.md"):
                raise ValueError(f"frame-generation component is forbidden in SR-only release: {name}")
        pinned_assets = {
            "licenses/nvidia/LICENSE.txt": "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0",
            "fonts/NotoSansCJKsc-Regular.otf": "2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b",
            "licenses/fonts/noto-cjk/OFL.txt": "6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2",
            "patches/GT7/GT7-PSSR-1440-to-4K.json": "5b05b13d0c9732fb84781b3e20b3cac2865a090ad73d49b0b1c0eb4602b94a1d",
        }
        for name, digest in {**vendor_hashes, **pinned_assets}.items():
            if expected[name].lower() != digest:
                raise ValueError(f"unvalidated release component: {name}")
        patch_manifest = json.loads(release.read(root + "patches/GT7/patches.json"))
        native_patch = [item for item in patch_manifest["patches"] if item.get("file") == "GT7-PSSR-1440-to-4K.json"]
        if len(native_patch) != 1 or native_patch[0].get("sha256") != pinned_assets["patches/GT7/GT7-PSSR-1440-to-4K.json"]:
            raise ValueError("native 1440p-to-4K profile registration mismatch")
        for item in patch_manifest["patches"]:
            name = item["file"]
            if not name or re.search(r"[/\\:]", name) or name in {".", ".."}:
                raise ValueError("unsafe GT7 patch filename")
            if expected.get("patches/GT7/" + name) != item["sha256"]:
                raise ValueError(f"GT7 patch manifest hash mismatch: {name}")
        # Assert the same exact file set at ZIP verification as during packaging.
        allowed_root_dll = re.compile(
            r"(?:Qt6[A-Za-z0-9_]+|icu[a-z0-9_]*|concrt140|msvcp140(?:_[a-z0-9_]+)?|"
            r"vcruntime140(?:_[a-z0-9_]+)?|vccorlib140|libwinpthread-1|kyty_dlss_backend|"
            r"nvngx_dlss)\.dll", re.I)
        for name in actual:
            if "/" not in name and name.lower().endswith(".dll") and not allowed_root_dll.fullmatch(name):
                raise ValueError(f"unexpected root runtime DLL: {name}")
        forbidden_suffixes = {".pdb", ".map", ".log", ".dmp", ".gpudmp", ".bin", ".raw", ".rgba16f", ".ini"}
        forbidden_parts = {"_savedata", "_downloaddata", "_trophies", "cache", "caches",
                           "shader-cache", "shader_cache", "_pipelinecache", "capture",
                           "captures", "log", "logs", "outputs", "scripts"}
        for name in actual:
            path = pathlib.PurePosixPath(name)
            lowered = {part.lower() for part in path.parts}
            if path.name.lower() in {"eboot.bin", "gt7save.bin"} or lowered & forbidden_parts:
                raise ValueError(f"game, save or cache content found in release: {name}")
            if path.suffix.lower() in forbidden_suffixes:
                raise ValueError(f"development symbol, dump or log found in release: {name}")

        # Public metadata and docs must not disclose this builder's absolute workspace path.
        text_extensions = {".md", ".txt", ".json", ".ps1", ".cmd", ".xml", ".ini"}
        windows_absolute_path = re.compile(rb"[A-Za-z]:[\\/](?:Users|Documents and Settings)[\\/]", re.I)
        for name in actual:
            path = pathlib.PurePosixPath(name)
            if path.suffix.lower() in text_extensions:
                if windows_absolute_path.search(release.read(root + name)):
                    raise ValueError(f"local absolute path found in release text: {name}")
        print(f"PASS: {len(actual)} files, ZIP CRC and SHA-256, pinned native 1440p-to-4K PSSR backend and NVIDIA SR runtime; "
              "no FG component or game/save/cache/log/symbol/legacy-script content")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
