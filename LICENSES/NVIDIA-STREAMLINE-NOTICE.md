# NVIDIA Streamline and native 2X frame generation

Tohi-GT7 1.0.3 includes the unmodified production x64
Streamline 2.14.16 components `sl.interposer.dll`, `sl.common.dll`,
`sl.dlss_g.dll`, `sl.reflex.dll` and `sl.pcl.dll`, together with
`nvngx_dlssg.dll` 310.9.16.0 and `NvLowLatencyVk.dll` from that SDK bundle.
The accompanying DLSS Super Resolution runtime is `nvngx_dlss.dll`
310.9.16.0. These eight NVIDIA DLLs reside in the same runtime directory.
The package's SHA256SUMS.txt identifies the exact distributed files.
Only the signed production files are included; development/debug runtimes,
SDK tools and shader caches are not included.

NVIDIA components retain their respective copyrights and licenses. They are
not relicensed under the project's GPL license. See the accompanying files:

- `streamline/LICENSE.txt`: Streamline source license.
- `streamline/THIRD-PARTY-LICENSES.md`: Streamline SDK third-party notices.
- `streamline/RTX-SDK-LICENSE.txt`: NVIDIA RTX SDK component license.
- `streamline/REFLEX-LICENSE.txt`: NVIDIA Reflex SDK component license.
- `LICENSE.txt` and `NVIDIA-DLSS-NOTICE.md`: the separately integrated NGX SR backend.

The native integration consumes the game's PSSR temporal inputs and reconstructs
with NVIDIA DLSS. It does not distribute Sony neural-network code or game data.
Frame generation requests one extra frame per rendered frame (fixed 2X).
It does not expose higher or dynamic multi-frame-generation multipliers.
This release supports GeForce RTX 40 and RTX 50 families; the tested model was
GeForce RTX 5080, not every model in those families. Runtime hardware/driver
support and valid scene inputs are required; unsupported
frames use ordinary presentation. This is not a guarantee of continuous 2X,
physical display FPS, or NVIDIA sponsorship or endorsement.

Upstream SDK sources and component terms:
https://github.com/NVIDIAGameWorks/Streamline
https://github.com/NVIDIA/DLSS
