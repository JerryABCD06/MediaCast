# Contributing to Media Cast

Thank you for contributing.

## Before Contributing

Please check existing issues and discussions before opening a duplicate report.

For protocol changes, include enough technical information for the behavior to be reproduced.

For compatibility issues, useful information may include:

- sender application and version;
- Media Cast version;
- Windows version;
- protocol involved;
- network topology;
- relevant logs;
- packet captures where appropriate and safe to share.

Remove personal information, credentials, tokens, private URLs, and other sensitive data before publishing logs or packet captures.

## Code Contributions

Keep changes focused and avoid unrelated refactoring.

Protocol implementations should remain separated from the core playback layer where practical.

Generated build artifacts should not normally be committed.

## Copyright and Licensing

The project is licensed under **GPL-3.0-or-later** (see `LICENSE` at the repository root).

By submitting a contribution — a patch, a pull request, or anything else — you agree
to license that contribution under GPL-3.0-or-later, and you confirm that you have the
right to do so. Contributions come in under the same license the project goes out
under, so **no separate contributor license agreement is required, and no copyright
assignment takes place**: you keep the copyright in what you wrote, and the project can
still distribute it as part of the whole.

Contributors should only submit code, documentation, assets, or other material that they are permitted to contribute.

Do not submit confidential third-party source code, leaked credentials, private certificates, proprietary assets, or material copied from software that you are not authorized to reuse.

## Pull Requests

A useful pull request should explain:

1. what changed;
2. why it changed;
3. how it was tested;
4. whether compatibility behavior changed.

For protocol work, interoperability testing against at least one real sender or receiver is strongly encouraged.
