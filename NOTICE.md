# Media Cast — Project Notice

Media Cast is an independent open-source project.

Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

Licensed under the **GNU General Public License version 3 or later**
(SPDX identifier `GPL-3.0-or-later`). The full license text is in `LICENSE` at the
repository root, and every source file carries the corresponding SPDX header.

## Third-party components

This package bundles third-party software. The authoritative list — component, version,
license, copyright holder, and where to get the corresponding source — is
**`licenses/THIRD-PARTY-NOTICES.md`**, together with the full license texts in `licenses/`.

In short:

| Component | License |
|---|---|
| libmpv / FFmpeg (`libmpv-2.dll`) | GPLv2 or later (this build enables GPL components) |
| Qt 6 (dynamically linked, unmodified) | LGPLv3 |
| FluentUI (QML component library) | MIT |

## Documents in this repository

**At the repository root** — for anyone visiting the project:

- `LICENSE` — the project's own license (GPL-3.0-or-later)
- `NOTICE.md` — this file
- `CONTRIBUTING.md` — how contributions are licensed (no CLA, no copyright assignment)
- `SECURITY.md` — how to report a vulnerability
- `licenses/` — third-party notices and full license texts, shipped with every package

**In `docs/legal/`** — notices aimed at users of the program:

- `PRIVACY.md` — what the program touches, what it leaves on disk, which ports it opens
- `LEGAL-NOTICES.md` — general legal notice: warranty, protocol compatibility vs. certification, network behavior, user content
- `TRADEMARKS.md` — third-party names and marks
- `CODECS.md` — codec patent notice
- `README-LEGAL-SHORT.md` — a one-screen summary of the above

## No certification is claimed

Interoperating with DLNA/UPnP, Apple AirPlay, Google Cast, Miracast, or vendor-specific
casting protocols does **not** imply endorsement, sponsorship, certification, or
affiliation with the owners of those protocols. No third-party certification is claimed
unless explicitly stated.
