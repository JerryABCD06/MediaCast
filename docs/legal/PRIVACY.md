# Media Cast — Privacy Notice

**Status:** Draft

## 1. Scope

This notice describes the privacy behavior of the Media Cast application itself.

It does not describe the privacy practices of sender applications, operating systems, third-party services, or websites that may interact with Media Cast.

## 2. Local Network Data

Media Cast may process information necessary to provide receiver functionality, including:

- local IP addresses and network interfaces;
- device name;
- receiver capabilities;
- protocol discovery messages;
- playback commands;
- media URLs or identifiers supplied by a sender;
- playback state and metadata;
- local configuration and diagnostic information.

## 3. Internet Communication

Media Cast is intended primarily for local-network casting.

Unless a specific feature explicitly requires network access, Media Cast should not assume that remote servers are necessary for ordinary local playback.

If future versions introduce telemetry, update checking, crash reporting, cloud services, or other remote communication, those features must be documented here before release.

## 4. Logs

Diagnostic logs may contain technical information such as IP addresses, device names, media URLs, filenames, error messages, and protocol messages.

Users should review logs before publicly sharing them.

The project should avoid collecting unnecessary personal information.

## 5. Local Storage

Media Cast may store configuration, receiver identity, logs, cache data, or other local application state.

Future releases should document the exact storage locations and retention behavior.

## 6. Third-Party Applications

A sender application may independently collect information about casting activity.

Media Cast does not control the privacy practices of third-party senders or services.

Users should consult the privacy documentation of those applications.

## 7. Changes

This notice should be updated before any new telemetry, analytics, cloud, advertising, account, or remote-data feature is introduced.
