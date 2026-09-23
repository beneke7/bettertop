# Third-party components

## btop++

BetterTop's host collectors, terminal toolkit, process view and themes derive
from [btop++](https://github.com/aristocratos/btop), revision
`612e18f5bd598fe987b30d041b39e5d7b3e0794f`. Its source and project files retain
their original Apache License 2.0 notices. The repository's `LICENSE` covers
those files.

## nvtop

The NVIDIA collector is built from the vendored extraction and timing sources
in `src/bettertop_gpu/vendor/nvtop/`, copied from
[nvtop](https://github.com/Syllo/nvtop), revision
`2300fb5d0ab2ff5a03fd6325b1cb01b0b757926e`. The original GPL-3.0-or-later
license is at `src/bettertop_gpu/vendor/nvtop/LICENSE`; original source
copyright and SPDX headers remain in place.

Local changes gate PCIe/NVLink diagnostics behind an explicit helper option,
correct the NVML memory-v2 structure version, cap and report device/process
truncation, add stable device identity, and serialize valid NVIDIA fields
through a versioned local protocol. The vendored source is distributed with
those changes under its original terms. The release archive includes both the
Apache-licensed BetterTop files and the GPL-licensed nvtop helper sources and
notices.
