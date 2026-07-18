**English** | [Русский](README_RU.md) | [Changelog](CHANGELOG.md)

# LanSpeak

LanSpeak is a ultra-low latency, peer-to-peer voice chat for Windows. It is designed primarily for players who are physically in the same room and connected to the same wired LAN. In this situation, a person's voice is heard first through the air and then repeated through the headphones after the delay introduced by a typical Internet calling service, creating a "double voice" effect. LanSpeak reduces that delay to a minimum.

LanSpeak uses shared-mode WASAPI and raw PCM over UDP. There is no central server: every participant sends audio directly to the selected contacts.

> [!WARNING]
> LanSpeak currently has no encryption or peer authentication. Use it only on a trusted local network.

## Features

- Shared-mode WASAPI capture and playback, allowing the microphone to remain available to a game.
- Raw mono PCM16 over UDP with a small jitter buffer and no codec delay.
- Full-mesh rooms for two or more participants without a server.
- Configurable local UDP port and IPv4 interface binding.
- Per-contact online presence with low-frequency peer-to-peer UDP probes and RTT diagnostics.
- Global and per-contact push-to-talk, including system-wide keyboard and mouse hotkeys.
- Per-contact gain, mute, and inclusion in or exclusion from group PTT.
- Per-contact receiver-side self-ducking with configurable attack, hold, and release.
- Contact and microphone VU meters with a `-60 dBFS` floor and smooth release.
- In-game OSD for incoming and outgoing voice activity.
- System tray integration and single-instance behavior.
- English and Russian GUI localization with automatic Windows language detection.
- A separate console core with device, capture, playback, UDP, and loopback diagnostics.

## Quick Start

1. Keep `LanSpeak.exe` and `LanSpeakCore.exe` in the same directory.
2. Run `LanSpeak.exe`. The console core starts automatically in the background.
3. If Windows Firewall asks for access for `LanSpeakCore.exe`, allow it on **Private networks**.
4. Select the input and output endpoints from the **Settings** menu.
5. Add every other participant as a contact using their LAN IP address and UDP port. The default port is `49740`.
6. Every participant must add every other participant to form the full mesh.
7. Use the group talk button or configure global/per-contact keyboard or mouse PTT hotkeys under **Settings > Global Hotkeys**.

The application starts in listening mode with outgoing microphone transmission disabled. Settings and contacts are stored in `settings.txt` next to the executables.
The default network binding accepts traffic on all interfaces (`0.0.0.0`). It can be restricted under **Settings > Network settings** when a computer has multiple network adapters.

## Building

### Requirements

- Windows 10 or Windows 11, x64.
- Visual Studio 2022 with the **Desktop development with C++** workload.
- CMake 4.2 or newer.
- A C++20 compiler. MSVC is the supported toolchain.

No third-party libraries are required.

From an **x64 Native Tools Command Prompt for VS 2022**:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The resulting executables are normally placed in `build/Release/`:

- `LanSpeak.exe` - Windows GUI application and the file users launch.
- `LanSpeakCore.exe` - console audio/network core and diagnostic tool.

MSVC builds use the static runtime (`/MT` for Release and `/MTd` for Debug), so the Visual C++ Redistributable is not required for the resulting executables.

### Build Versions

An untagged development build uses `1.1.0-alpha-dev`. When the current commit has an exact SemVer tag such as `1.1.0-alpha.1`, CMake uses that tag for the About dialog and the Windows version information embedded in both executables.

The version can also be injected without changing tracked files:

```powershell
cmake -S . -B build -DLANSPEAK_VERSION_OVERRIDE=1.1.0-alpha.1
```

To build, test, and create a local Windows archive without publishing a tag or release:

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_package.ps1 -Version 1.1.0-alpha.1
```

The archive is written to `dist/` and contains both executables, the license, both README files, and both changelog files. It never includes `settings.txt`.

To publish an alpha snapshot, tag the tested commit and build that exact commit:

```powershell
git tag -a 1.1.0-alpha.1 -m "LAN Speak 1.1.0 alpha 1"
git push origin 1.1.0-alpha.1
```

GitHub releases for `alpha`, `beta`, and `rc` tags should be marked as pre-releases.

### CLion

Open the repository root as a CMake project and select a **Visual Studio** toolchain. MinGW is not a supported toolchain for this Windows/WASAPI implementation. The main CMake targets are `LanSpeak`, `LanSpeakCore`, and `LanSpeakTests`.

## Core Diagnostics

The core can be run directly from a terminal:

```powershell
LanSpeakCore.exe --help
LanSpeakCore.exe --list-devices
LanSpeakCore.exe --capture-test 10
LanSpeakCore.exe --tone-test 5
LanSpeakCore.exe --udp-audio-loopback 10 49740
```

`--tone-test` produces an audible test tone. Run `LanSpeakCore.exe --help` for all available modes and parameters.

## Architecture

- `LanSpeak.exe` owns contacts, settings, hotkeys, tray integration, OSD, and the Windows UI.
- `LanSpeakCore.exe` owns shared WASAPI streams, UDP transport, jitter buffering, mixing, ducking, and audio telemetry.
- The processes communicate through text control and telemetry pipes. The GUI also captures the core's diagnostic output.
- UDP protocol v1 uses a fixed 72-byte header followed by mono PCM16 audio. Existing v1 cores remain wire-compatible.
- At `48 kHz`, one active PCM stream uses about `0.8 Mbit/s` of payload bandwidth per recipient.

The practical latency target is approximately `20-50 ms`, but actual results depend on audio hardware, WASAPI periods, drivers, scheduling, and the network.

## Repository Layout

```text
src/common/  Shared utilities
src/core/    WASAPI, UDP, PCM, jitter buffer, routing, and diagnostics
src/gui/     Win32 GUI, settings, IPC, contacts, OSD, tray, and hotkeys
tests/       CTest support tests
assets/      Application icons and previews
tools/       Development scripts
```

## Current Limitations

- Windows-only; WASAPI is required.
- Intended for trusted LANs: audio and control metadata are neither encrypted nor authenticated.
- No NAT traversal, relay server, discovery service, or Internet congestion control.
- Raw PCM favors latency and simplicity over bandwidth efficiency.
- Self-ducking suppresses the local user's voice relayed back by a nearby peer; it is not a full acoustic echo canceller.
- This is still an experimental project. Test audio devices and firewall rules before relying on it during a game.

## Contributing

Build both the application and the tests, then run CTest before submitting a change. Changes to UDP protocol v1, CLI commands, IPC text formats, or `settings.txt` should preserve backward compatibility unless a migration is included.

## License

LanSpeak is available under the [MIT License](LICENSE) (`SPDX-License-Identifier: MIT`).
