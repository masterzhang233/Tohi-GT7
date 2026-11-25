# NVIDIA DLSS runtime

The Windows release includes NVIDIA's unmodified production `nvngx_dlss.dll`
(file version 310.9.16.0) from the NVIDIA DLSS SDK, revision
`9981c6350dc58c0ce21f024fd65e31e851c31689`.

NVIDIA DLSS and the NGX SDK components retain their own copyright and license.
They are governed by the accompanying NVIDIA RTX SDKs license in `LICENSE.txt`,
not by the project's GPL license. The independently built
`kyty_dlss_backend.dll` links the NGX SDK and exposes a C ABI to the emulator.

DLSS requires a supported NVIDIA RTX GPU and compatible NVIDIA drivers.
Tohi-GT7 1.0.3 uses native PSSR temporal inputs for fixed 1920x1080 to
3840x2160 DLSS reconstruction. The NVIDIA frame-generation option requests
fixed 2X and has additional hardware/driver and scene-input requirements.
See NVIDIA-STREAMLINE-NOTICE.md for its separately licensed components.
Neither path makes DLSS available on other GPU vendors.

The runtime is shipped as provided by NVIDIA; no runtime patch or replacement
DLL is included. The DLSS SDK is available at https://github.com/NVIDIA/DLSS.
