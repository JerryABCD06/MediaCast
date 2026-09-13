# Media Cast — Security Policy

## Reporting a Vulnerability

Please report security vulnerabilities **privately** — not in a public issue — to:

**Wang Yunzheng <wyz-mcast@outlook.com>**

(or through the repository's private security-advisory mechanism, if one is enabled)

Do not publicly disclose a vulnerability containing an exploitable proof of concept before the project has had a reasonable opportunity to investigate it.

## Scope

Security reports may include issues involving:

- unauthorized control of the receiver;
- unsafe network protocol handling;
- remote code execution;
- arbitrary file access;
- malicious media or URL handling;
- unsafe deserialization;
- privilege escalation;
- sensitive information disclosure;
- denial of service;
- unsafe automatic updates.

## Local Network Assumption

Media Cast is primarily intended for trusted local networks. This is not a substitute for secure protocol design.

Network-facing components should treat received data as untrusted.

## Supported Versions

| Version | Supported |
|---|---|
| Latest release | Yes |
| Older releases | Best effort — please try the latest first |
| Development builds | Best effort |

## Security Updates

Security fixes should be documented in release notes when disclosure is appropriate.

Users should update to the latest supported version when security fixes are released.
