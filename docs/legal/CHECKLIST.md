# Media Cast — Open-Source Release Checklist

This checklist is for project maintenance, not legal advice.

## Before first public source release

- [ ] Select an open-source license.
- [ ] Add the complete license text as `LICENSE`.
- [ ] Decide whether source files should carry SPDX identifiers.
- [ ] Confirm copyright ownership of project-created code and assets.
- [ ] Add `LEGAL-NOTICES.md`.
- [ ] Add `TRADEMARKS.md`.
- [ ] Add `PRIVACY.md`.
- [ ] Add `SECURITY.md`.
- [ ] Add `CODE_OF_CONDUCT.md`.
- [ ] Add `CONTRIBUTING.md`.
- [ ] Add `SUPPORT.md`.
- [ ] Decide whether to add a project warranty/disclaimer section to the chosen license package.
- [ ] Document whether the application has telemetry, update checking, crash reporting, or cloud communication.
- [ ] Document supported protocols and clearly distinguish compatibility from certification.
- [ ] Do not claim DLNA Certified, Apple-certified, Google-certified, or similar status unless actually obtained.

## Before the first binary release

- [ ] Determine exactly what libraries and runtime components are bundled.
- [ ] Add a separate third-party software notice package later.
- [ ] Record codec support.
- [ ] Review codec/patent notice.
- [ ] Check executable and installer metadata.
- [ ] Check trademark use in application UI and documentation.
- [ ] Document network ports and discovery behavior.
- [ ] Review Windows Firewall behavior.
- [ ] Verify the privacy notice matches actual network traffic.
- [ ] Verify logs do not unnecessarily expose sensitive information.
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
