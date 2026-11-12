# Media Cast — Privacy Notice

**Applies to:** the Media Cast Receiver processes (`MCast.Shell.exe`,
`MCast.Protocol.exe`, and `MCast.Ui.exe`). It does not
describe what sender applications, the operating system, or third-party services do.

## In one sentence

**No telemetry. Nothing is uploaded anywhere. It needs no internet access.** It works
inside the local network and only exchanges the protocol traffic required to receive a cast.

## What it handles

- The machine's network interfaces and their addresses — used to decide where to announce itself.
- **The computer name** — used as the device name, and **broadcast to the local network**,
  so other devices can see this computer in their cast lists.
- The playback commands, media URLs, and any metadata the sender attaches (title, artist, …).
- Playback state and position, read back from the local player.

## Does it talk to the internet?

No telemetry or update-check connection is initiated by the application. It listens on
local ports and accepts connections from the local network.

Two content-related exceptions are worth naming:

- if a media or playlist URL points at the internet, the playback backend/planner will
  fetch it because the sender asked this receiver to play that URL;
- a remote `albumArtURI` may be fetched to display the sender-provided cover.

These are playback of content selected by the sender, not telemetry or reporting about
the user.

## What it leaves on your machine

| What | Where | Contains |
|---|---|---|
| Settings | `MediaCast.json`, next to the executable | language, theme, Mica, accept/broadcast settings, device name/UDN, restored picture controls |
| Shell migration state | `MediaCast.Shell.json`, next to the executable | legacy receiving-state migration marker/state |
| Protocol log | `MCast.Protocol.log`, next to the executable | computer name, local IP addresses, protocol requests and media/playlist URLs |
| UI log | `MCast.Ui.log`, next to the executable | QML/mpv state, metadata fields, local/temporary artwork paths and media URLs |
| Temporary artwork | `%TEMP%\MediaCast\artwork` | extracted/cached cover images |
| Temporary image sequences | `%TEMP%\MediaCast\image-sequences` | generated playlists for image-sequence playback |
| Legacy device identity | `%LOCALAPPDATA%\MCast\renderer.ini` | old device UUID imported once if `MediaCast.json` has no valid UDN; not the current authority |

Logs are local files that the program never uploads. Review them before sharing publicly:
they contain local addresses and the names/URLs of media played. Temporary media files
are cleaned during normal operation, but an abnormal process exit can leave residue that
may be deleted manually.

## Ports it listens on

- TCP **8200** — device description and SOAP control
- UDP **1900** — SSDP discovery (announcing itself, answering searches)

## Discoverability

Being discoverable *is* the function of a receiver — but it also means this computer is
visible to other devices on the same network. On networks you do not trust, turn off
"accept new casts" from the tray menu.

## Changes

If a future version adds telemetry, crash reporting, update checking, accounts, or any
cloud feature, **this notice must be updated first** — and that change must be called out
in the release notes.
