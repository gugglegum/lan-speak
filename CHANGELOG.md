**English** | [Русский](CHANGELOG_RU.md)

# Changelog

All notable changes to LAN Speak are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

Changes intended for version 1.1.0 will be recorded here as development progresses.

### Added

- Added on-demand discovery of LAN Speak users on the local network. The resizable **Settings > Find people...** dialog broadcasts only when opened or refreshed, shows Windows computer names, and adds selected endpoints to contacts while preventing duplicates.
- Added per-contact online presence using low-frequency peer-to-peer UDP `PING/PONG/GOODBYE` messages. Contact cards show a status dot, while the tooltip reports the exact state and RTT.
- Added estimated incoming and outgoing voice pipeline latency to the contact tooltip. New clients exchange compact, acknowledged audio profiles only when needed; the estimate combines measured capture-to-send time, half RTT, the recipient's configured receive buffer, and WASAPI render latency.
- Global and per-contact PTT hotkeys can now use the right, middle, Mouse 4, or Mouse 5 button. Mouse hotkeys remain active while keyboard modifiers are held.
- Added a per-contact `Talk continuously` latch to the contact context menu. A latched contact remains active after the PTT button is released and is marked with a lock on the contact card.
- Added a per-contact receive buffer setting from 5 to 100 ms for tuning latency and resilience to network jitter.
- Added an audio latency diagnostics dialog showing the active input and output devices, stream format, engine period, endpoint buffer, WASAPI stream latency, stream mode, and current render padding.
- Added network settings for the local UDP port and IPv4 interface. A saved interface is tracked by adapter ID, and an unavailable adapter falls back to all interfaces with a startup warning.

### Changed

- Replaced the group talk mode selector with a `Talk continuously` checkbox. The group talk button now always transmits only while held, while the checkbox latches and releases the same transmission state.
- Default input and output device selection now uses the Windows `console` role, matching the devices selected on the main Sound settings page.
- Development and pre-release builds now derive their displayed and Windows executable versions from an exact SemVer Git tag or a CMake override without modifying tracked source files.

### Fixed

- The global hotkey dialog now reliably records mouse buttons and hotkeys that are already assigned.
- Editing or cancelling a contact no longer clears its latched `Talk continuously` state. Latched group and per-contact transmission is restored after a Core restart caused by settings changes.
- The lock indicator on a contact PTT button no longer squeezes the `PTT` label against the button border.

## [1.0.0] - 2026-07-13

### Added

- First public release of LAN Speak for Windows 10 and Windows 11 x64.
- Shared-mode WASAPI capture and playback, allowing the microphone to remain available to games and other applications.
- Direct peer-to-peer mono PCM16 audio transport over UDP with no central server or codec delay.
- Small jitter buffer, sample-rate conversion, mixing, and support for full-mesh rooms with two or more participants.
- Manually managed contacts with persistent IP address, port, gain, self-ducking, mute, and group-routing settings.
- Global and per-contact Push-to-Talk controls with configurable system-wide hotkeys.
- Toggle and hold-to-talk modes, with outgoing microphone transmission disabled by default.
- Per-contact receiver-side self-ducking with configurable threshold, attenuation, attack, hold, and release.
- Per-contact and local microphone VU meters with a `-60 dBFS` floor and smooth release.
- In-game OSD for incoming and outgoing voice activity, including destination and VU indication.
- Input and output device selection through the application menu.
- System tray integration, close-to-tray behavior, and single-instance application handling.
- English and Russian GUI localization with automatic Windows language detection.
- Persistent settings stored in `settings.txt` next to the executables.
- Optional embedded diagnostic console in the GUI.
- Separate `LanSpeakCore.exe` audio/network process with WASAPI, UDP, tone, capture, playback, and loopback diagnostic modes.
- Text-based control and telemetry IPC between `LanSpeak.exe` and `LanSpeakCore.exe`.
- Static MSVC runtime linking, so release binaries do not require the Visual C++ Redistributable.

[Unreleased]: https://github.com/gugglegum/lan-speak/compare/1.0.0...HEAD
[1.0.0]: https://github.com/gugglegum/lan-speak/releases/tag/1.0.0
