# green-nx

A standalone, open-source **Xbox Cloud Gaming (xCloud) client for the Nintendo
Switch** (homebrew). Sign in with your Microsoft account and stream your Game
Pass library natively — WebRTC connection, hardware H.264 decoding and
GPU rendering are all done on the console, with no companion apps or PCs
involved.

**Fully working:** sign-in, game library with box art and search, and smooth
720p / 1080p / 1080p HQ streaming with audio and full controller input.

## Features

- Microsoft **device-code sign-in** (no password typed on console); tokens
  cached on SD
- **Game library** with box-art covers, search, quality settings
- **Native WebRTC streaming**: ICE (Teredo), DTLS-SRTP, SCTP data channels,
  RTP video/audio, NACK/PLI recovery — implemented on-console
- **Hardware H.264 decoding** (NVDEC via FFmpeg's NVTEGRA hwaccel) with
  **zero-copy GPU rendering** (deko3d) — the decoded surface goes straight
  from the decoder to the screen
- Opus audio, 60 fps video, three quality tiers (720p / 1080p / 1080p HQ)

## Requirements

- A Nintendo Switch running the [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) custom firmware
- An active **Xbox Game Pass** subscription — every tier (Essential, Premium,
  Ultimate) includes Cloud Gaming; Ultimate streams the largest catalog at the
  highest quality
- A 5 GHz Wi-Fi connection is recommended

## Install

1. Copy `green-nx.nro` to `sdmc:/switch/` on your SD card.
2. Launch **in title mode** — hold **R** while starting an installed game.
   (Applet mode does not have enough memory for hardware video decoding.)
3. Sign in once with the device code shown on screen (`microsoft.com/link`).
   Tokens and the game list are cached in `sdmc:/switch/green-nx/`.

### Controls

| Context | Buttons |
| --- | --- |
| Library | Left stick / d-pad move · **A** play · **Y** search · **X** refresh · **ZL** settings · **-** sign out · **+** exit |
| Settings | Stream quality (720p / 1080p / 1080p HQ) · button layout (positional / labels) |
| In stream | Xbox controls mapped from the Switch pad · hold **-** + **+** to quit |

## Build

No local toolchain needed — everything builds inside the
`devkitpro/devkita64` Docker image:

```sh
# 1. Build the WebRTC dependencies once (libpeer, libsrtp, usrsctp, mbedTLS)
bash deps/build-switch.sh

# 2. Build the app
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64 make
```

The result is `green-nx.nro`. A small desktop harness
(`make -f Makefile.pc`) builds the core (auth/catalog/signaling) for
development on a PC.

### Layout

```
src/core/            auth, catalog, HTTP, xCloud session + protocol
src/switch/          UI (SDL2), covers, input
src/switch/stream/   streaming engine: WebRTC glue, RTP jitter buffer,
                     NVDEC decoder, deko3d zero-copy renderer, Opus audio
shaders/             deko3d video shaders (compiled by uam during make)
deps/                build script + patches for the WebRTC stack
```

## Third-party software

green-nx builds on these open-source projects:

| Project | License | Use |
| --- | --- | --- |
| [libpeer](https://github.com/sepfy/libpeer) (patched) | MIT | WebRTC: ICE / DTLS-SRTP / SCTP |
| [libsrtp](https://github.com/cisco/libsrtp) | BSD-3 | SRTP encryption |
| [usrsctp](https://github.com/sctplab/usrsctp) | BSD-3 | SCTP data channels |
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | Apache-2.0 | TLS / DTLS crypto |
| [FFmpeg](https://ffmpeg.org) (devkitPro build with [averne's](https://github.com/averne/FFmpeg) NVTEGRA hwaccel) | LGPL-2.1+ | H.264 hardware decoding |
| [deko3d](https://github.com/devkitPro/deko3d) | zlib | GPU rendering |
| [SDL2](https://libsdl.org), SDL2_ttf, SDL2_image | zlib | UI rendering, input |
| [Opus](https://opus-codec.org) | BSD-3 | Audio decoding |
| [libcurl](https://curl.se) | curl | HTTPS |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | JSON |
| [devkitPro / libnx](https://devkitpro.org) | various | Switch toolchain and OS interface |

The patches applied to libpeer (keyframe requests, NACK, receiver reports,
REMB, data-channel fixes, Switch port) are in `deps/patches/`.

## Disclaimer

green-nx is a **fun, experimental, non-commercial** hobby project, provided
as-is for personal use. It is **not affiliated with, endorsed by, or supported
by Microsoft or Nintendo** in any way. Xbox, Xbox Cloud Gaming and Game Pass
are trademarks of Microsoft. Nintendo Switch is a trademark of Nintendo. You
need your own valid Game Pass subscription; this project only
implements a client for a service you already pay for.

## License

[GPL-3.0](LICENSE)
