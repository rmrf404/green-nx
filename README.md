# green-nx

A standalone, open-source Xbox Cloud Gaming (xCloud) client for Nintendo
Switch homebrew. Green NX signs in to Xbox, starts cloud games, receives the
WebRTC stream, decodes H.264 in hardware, and renders it directly on the
console. No companion PC or phone is required.

> [!NOTE]
> Green NX is an experimental community project. It is not affiliated with,
> endorsed by, or supported by Microsoft or Nintendo.

## Features

- Microsoft device-code sign-in with cached tokens
- Game Pass cloud library, cover art, search, region and language settings
- Native WebRTC transport with ICE, DTLS-SRTP, SCTP, NACK/PLI recovery and REMB
- NVTEGRA/NVDEC H.264 hardware decoding and deko3d zero-copy presentation
- Opus audio and full controller input with configurable button mapping/rumble
- 720p, 1080p and 1080p high-bitrate quality tiers
- Low-latency and smooth video-pacing modes
- GPU luma sharpening, contrast enhancement and labeled comparison tests
- Per-pacing-mode diagnostic stream logs

## Install

Requirements:

- A Nintendo Switch running
  [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)
- An Xbox Game Pass Ultimate subscription
- 5 GHz Wi-Fi or docked Ethernet for the best results

Copy `green-nx.nro` to `sdmc:/switch/`, then launch it in title mode by holding
**R** while opening an installed game. Applet mode does not provide enough
memory for hardware video decoding.

Sign in using the device code displayed by the app. Tokens, settings, cached
catalog data and stream logs are stored in `sdmc:/switch/green-nx/`.

## Settings

| Setting | Options | Guidance |
| --- | --- | --- |
| Stream quality | 720p, 1080p, 1080p high bitrate | Higher tiers require a stable, fast connection. |
| Video pacing | Low latency, Smooth | Low latency presents the newest frame immediately. Smooth holds one decoded source frame to absorb arrival jitter, improving cadence at the cost of roughly one source frame of latency. |
| Luma sharpening | Off, Low, Medium, High, Extreme, Strobe test | **Medium is recommended.** Sharpening affects brightness detail without sharpening chroma. Disable it if compression edges or halos become distracting. |
| Contrast | Off, Low, Medium, High, Strobe test | **Medium is recommended.** The endpoint-protected curve improves perceived depth while retaining black and white endpoints. |
| Button layout | Positional, Match labels | Select physical Xbox-style positions or matching printed Switch labels. |
| Vibration | Off, Low, Medium, High | Controls rumble strength. |
| Region bypass | Off plus available regions | Intended for unsupported xCloud locations; use at your own risk. |
| Game language | Supported xCloud locales | Applied when the next game launches. |

### Strobe tests

Strobe is a visual comparison tool, not a normal playback mode. It changes the
selected effect every three seconds and shows the active level in the top-left
corner of the stream:

- Sharpening cycles **Off → Low → Medium → High → Extreme**
- Contrast cycles **Off → Low → Medium → High**

Watch the same stationary detail and moving edges through a complete cycle,
then select the fixed level you prefer. If both strobe tests are enabled, their
labels use separate lines so they do not overlap.

## Controls

| Context | Buttons |
| --- | --- |
| Library | Left stick/d-pad move · **A** play · **Y** search · **X** refresh · **ZL** settings · **-** sign out · **+** exit |
| Settings | Up/down select · Left/right change · **B** or **ZL** back |
| In stream | Xbox controls mapped from the Switch pad · hold **-** + **+** to quit |

## Stream logs

Each pacing mode writes a separate log so its timing can be analyzed without
mixing behaviors:

- `sdmc:/switch/green-nx/stream-log-low-latency.txt`
- `sdmc:/switch/green-nx/stream-log-smooth.txt`

The preceding session is retained with the matching `-prev` suffix. Timing and
cadence records include receive, decode and presentation behavior. Logs can
contain session diagnostics, so review them before sharing publicly.

## Changes in this development version

- Added a smooth pacing mode that uses a one-frame reserve to regularize
  presentation cadence; low-latency mode preserves the original behavior.
- Added independent logs for low-latency and smooth sessions.
- Added decode, presentation and cadence diagnostics.
- Fixed audio initialization when starting another stream or changing pacing
  mode in the same app session.
- Added configurable luma sharpening: Off, Low, Medium, High, Extreme and a
  labeled three-second strobe comparison.
- Added configurable endpoint-protected contrast enhancement: Off, Low,
  Medium, High and a labeled three-second strobe comparison.
- Kept sharpening and contrast strobe labels on separate overlay lines and
  corrected their screen orientation.
- Documented the recommended image settings and the latency/smoothness
  tradeoff in the on-device Settings screen.

See [CHANGELOG.md](CHANGELOG.md) for the concise release history.

## Build

Build with the `devkitpro/devkita64` Docker image:

```sh
# Build the WebRTC dependencies once.
bash deps/build-switch.sh

# Build Green NX.
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64 make
```

The output is `green-nx.nro`. The desktop harness can be built with
`make -f Makefile.pc` for core auth, catalog and signaling development.

### Source layout

```text
src/core/            Authentication, catalog, HTTP and xCloud protocol
src/switch/          Switch UI, covers and input
src/switch/stream/   WebRTC glue, jitter buffers, decoding, rendering and audio
shaders/             deko3d video shaders, image enhancement and overlays
deps/                WebRTC dependency build scripts and patches
```

## Third-party software

| Project | License | Use |
| --- | --- | --- |
| [libpeer](https://github.com/sepfy/libpeer) (patched) | MIT | WebRTC |
| [libsrtp](https://github.com/cisco/libsrtp) | BSD-3-Clause | SRTP |
| [usrsctp](https://github.com/sctplab/usrsctp) | BSD-3-Clause | SCTP |
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | Apache-2.0 | TLS/DTLS |
| [FFmpeg](https://ffmpeg.org) with [NVTEGRA](https://github.com/averne/FFmpeg) | LGPL-2.1+ | H.264 decoding |
| [deko3d](https://github.com/devkitPro/deko3d) | zlib | GPU rendering |
| [SDL2](https://libsdl.org), SDL2_ttf, SDL2_image | zlib | UI and input |
| [Opus](https://opus-codec.org) | BSD-3-Clause | Audio |
| [libcurl](https://curl.se) | curl | HTTPS |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | JSON |
| [devkitPro/libnx](https://devkitpro.org) | Various | Toolchain and Switch APIs |

The libpeer patches for keyframe requests, NACK, receiver reports, REMB, data
channels and the Switch port are under `deps/patches/`.

## License

Green NX is provided as-is under [GPL-3.0](LICENSE). Xbox, Xbox Cloud Gaming
and Game Pass are Microsoft trademarks. Nintendo Switch is a Nintendo
trademark.
