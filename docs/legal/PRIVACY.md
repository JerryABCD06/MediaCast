# Media Cast — Privacy Notice

**Applies to:** the Media Cast Receiver application itself (`MCast.exe`). It does not
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

No. It does not initiate outbound connections on its own. It listens on local ports and
accepts connections from the local network.

(One exception worth naming: if a media URL points at the internet, the playback backend
will fetch that stream — because you asked it to play that URL. That is playback of
content you selected, not the application reporting anything about you.)

## What it leaves on your machine

| What | Where | Contains |
|---|---|---|
| Settings | `MediaCast.json`, next to the executable | language, theme, accept/broadcast switches |
| Log | `MCast.log`, next to the executable | **the computer name, local IP addresses, and the media URLs that were played** (which may be local file paths) |
| Device identity | `%LOCALAPPDATA%\MCast\renderer.ini` | the device UDID. Delete it and senders will treat this as a new device |

The log is a local file that this program never sends anywhere. Review it before sharing
it publicly — it contains the names of media you played.

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
