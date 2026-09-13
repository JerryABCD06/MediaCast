# Media Cast — Codec and Patent Notice

> This document is informational and is not legal advice.

The playback backend (libmpv / FFmpeg) can handle far more formats than the list below.
What matters for this notice is that those formats fall into **two very different groups**,
and lumping them together would be misleading.

## Generally considered royalty-free

Their promoters have made public, explicit commitments:

| Format | Basis |
|---|---|
| VP8, VP9 | Google's royalty-free commitments |
| AV1 | AOMedia's royalty-free policy |
| Opus, FLAC, Vorbis | royalty-free by design |

## Covered by patent pools

A separate license may be required depending on **use, jurisdiction, and distribution model**:

| Format | Pools / holders |
|---|---|
| H.264 / AVC | Via LA (formerly MPEG LA) AVC pool |
| H.265 / HEVC | Access Advance, Via LA, Velos Media, plus independent holders |
| AAC | Via LA AAC pool |
| MP3 | patents expired |

## Two things this notice wants to be explicit about

1. **This is a patent question, not a copyright one.** It is not affected by which
   library, language, or implementation is used — the patents cover the method, not the
   code. Writing your own decoder changes nothing.
2. **The obligation typically falls on whoever distributes the capability.** Media Cast
   ships a playback backend that includes software decoders, so whoever redistributes
   this package is distributing codec capability. **The project's license grants no
   patent rights, and the project holds no codec patent license.**

The Media Cast project does not grant patent licenses for technologies it does not own.

The fact that an open-source playback component can decode or encode a format does not, by itself, establish that every possible commercial, organizational, or redistribution use is patent-cleared.

Users and distributors are responsible for determining whether additional licenses are required for their particular use and jurisdiction.

This notice does not prohibit or encourage any particular codec. It exists to make the distinction between software copyright licensing and technology patent licensing explicit.
