# Media Cast — Open-Source Release Checklist

This checklist is for project maintenance, not legal advice. Checked items reflect files
present in the repository as of 2026-11-12; release evidence still needs to be attached.

## Before first public source release

- [x] Select an open-source license: GPL-3.0-or-later.
- [x] Add the complete license text as `LICENSE`.
- [x] Decide whether source files should carry SPDX identifiers: yes, GPL-3.0-or-later headers.
- [x] Confirm copyright ownership of project-created code and assets.
- [x] Add `LEGAL-NOTICES.md`.
- [x] Add `TRADEMARKS.md`.
- [x] Add `PRIVACY.md`.
- [x] Add `SECURITY.md`.
- [x] Add `CODE_OF_CONDUCT.md`.
- [x] Add `CONTRIBUTING.md`.
- [x] Add `SUPPORT.md`.
- [ ] Add a project-specific warranty/disclaimer section where legally appropriate.
- [x] Document telemetry, update checking, crash reporting and cloud communication status.
- [x] Document supported protocols and distinguish compatibility from certification.
- [x] Do not claim DLNA Certified, Apple-certified, Google-certified, or similar status.

## Before the first binary release

- [x] Determine the intended bundled libraries/runtime components.
- [x] Add a separate third-party software notice package.
- [x] Record codec support.
- [x] Review codec/patent notice.
- [ ] Check executable and installer metadata.
- [ ] Check trademark use in application UI and documentation.
- [x] Document network ports and discovery behavior.
- [ ] Review Windows Firewall behavior.
- [ ] Re-verify at release time that the privacy notice matches actual network traffic.
- [ ] Re-verify at release time that both process logs and temporary media paths match the privacy notice.
- [ ] Record release-specific changes in `RELEASE-NOTICES.md`.

## Before certification claims

- [ ] Verify the exact certification program.
- [ ] Obtain written confirmation of certification status.
- [ ] Use only the branding and wording permitted by the certification owner.

## Before commercial distribution

- [ ] Review all bundled component licenses.
- [ ] Review codec/patent requirements for the intended jurisdictions.
- [ ] Review trademarks.
- [ ] Review installer/update infrastructure.
- [ ] Consider professional legal review for the actual distribution model.
