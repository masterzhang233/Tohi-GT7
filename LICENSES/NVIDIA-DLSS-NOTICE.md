# NVIDIA DLSS runtime

The Windows release includes NVIDIA's unmodified production `nvngx_dlss.dll`
(file version 310.9.16.0) from the NVIDIA DLSS SDK, revision
`9981c6350dc58c0ce21f024fd65e31e851c31689`.

NVIDIA DLSS and the NGX SDK components retain their own copyright and license.
They are governed by the accompanying NVIDIA RTX SDKs license in `LICENSE.txt`,
not by the project's GPL license. The independently built
`kyty_dlss_backend.dll` links the NGX SDK and exposes a C ABI to the emulator.

DLSS requires a supported NVIDIA RTX GPU and compatible NVIDIA drivers.
Tohi-GT7 1.0.4 uses native PSSR temporal inputs for 2560x1440 to 3840x2160
DLSS Super Resolution reconstruction, with no frame generation. Its supported
GPU scope is NVIDIA GeForce RTX 40 / 50 series; live validation used an RTX 5080.
The package does not include Streamline, DLSS-G or FidelityFX frame-generation
runtimes. This integration does not make DLSS available on other GPU vendors
and does not execute Sony's PSSR neural model.

The runtime is shipped as provided by NVIDIA; no runtime patch or replacement
DLL is included. The DLSS SDK is available at https://github.com/NVIDIA/DLSS.
