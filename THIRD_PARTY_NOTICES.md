# Third-party notices

Apex Combat statically incorporates or links the following third-party software. Each item below is used as a library dependency.

## CommonLibSSE-NG

Source: https://github.com/alandtse/CommonLibSSE-NG (pinned commit, recorded in `CMakeLists.txt` and `build-info.json`).
License: GPL-3.0-or-later with the Modding Exception and GPL-3.0 Linking Exception (with Corresponding Source); see `licenses/CommonLib-COPYING.txt` and `licenses/CommonLib-EXCEPTIONS.md`. The original MIT attribution is retained in `licenses/CommonLib-LICENSE-MIT.txt`; it is not the license of the current project as a whole. The bundled hde64 code uses BSD 2-Clause terms in `licenses/CommonLib-LICENSE-hde64.txt`. OpenVR headers use BSD 3-Clause terms in `licenses/OpenVR.txt`.

## Precision API headers

The `third_party/precision` directory contains the Precision modding API header and license documents:

- `licenses/Precision-COPYING`
- `licenses/Precision-EXCEPTIONS`
- `licenses/Precision-NOTICE.md`

Precision itself is a runtime dependency of the perfect-block feature and is not redistributed in this archive.

## TrueHUD API header

The `third_party/truehud/TrueHUDAPI.h` file is the TrueHUD modding API header, distributed by its author for exactly this purpose ("For modders: Copy this file into your own project if you wish to use this API"). TrueHUD itself is an optional runtime dependency of the poise HUD feature and is not redistributed in this archive.

## vcpkg dependencies

Built from the versions pinned in `vcpkg.json`; license texts are included per dependency:

- `licenses/fmt.txt` — {fmt}, MIT.
- `licenses/spdlog.txt` — spdlog, MIT.
- `licenses/directxmath.txt` — DirectXMath, MIT.
- `licenses/directxtk.txt` — DirectXTK for DirectX 11, MIT.
- `licenses/rapidcsv.txt` — rapidcsv, BSD 3-Clause.

## Apex Combat

Copyright the Apex Combat contributors; released under the repository LICENSE.
