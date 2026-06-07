# Deck Screen Share

Deck Screen Share records a Steam Deck display and system audio over Wi-Fi while
showing a live preview on a Windows PC. Both devices must be connected to the
same local network.

## Features

- A headless C++ SteamOS agent packaged as a Flatpak.
- The SteamOS application can be added as a Non-Steam Game and kept running
  while another game is open.
- A C# Avalonia receiver with a custom liquid-glass Windows interface.
- H.264, H.265/HEVC, and AV1 video encoding.
- Configurable video bitrate, audio bitrate, FPS, and stream buffer.
- MKV, MP4, MOV, WebM, and MPEG-TS recording containers.
- Automatic low-latency UDP live preview with audio, volume, and mute controls.
- Automatic reconnection when the SteamOS agent starts after the Windows app.
- Automatic persistence of all Windows receiver settings between launches.
- Independent UDP video, audio, and recording streams for low-latency local Wi-Fi use.

## Architecture

The SteamOS Flatpak runs as a headless service, starts FFmpeg on receiver
request, captures the display and system audio, and sends independent UDP
streams. The Windows application decodes preview frames directly into memory,
plays preview audio, and records from dedicated video and audio ports without
interrupting the preview.

MKV and WebM preserve the original video and Opus audio without transcoding.
MP4, MOV, and MPEG-TS preserve the original video and transcode audio to AAC for
container compatibility.

The SteamOS application is implemented in C++17 using GIO and JSON-GLib.
Python is not used anywhere in the project.

## Build And Install The SteamOS Flatpak

### Install From A GitHub Release

Download `DeckScreenShare-SteamOS.flatpak` from the latest
[GitHub Release](https://github.com/alexeygasenko/DeckScreenShare/releases),
then install it in SteamOS Desktop Mode:

```bash
flatpak install --user ./DeckScreenShare-SteamOS.flatpak
```

Run it directly for a quick check:

```bash
flatpak run io.github.deckscreenshare.Agent
```

### Build From Source

Install Flatpak and Flatpak Builder in SteamOS Desktop Mode, then install the
required GNOME SDK:

```bash
flatpak install flathub org.gnome.Platform//50 org.gnome.Sdk//50
```

Build and install the application from the repository root:

```bash
flatpak-builder --user --install --force-clean build-flatpak \
  flatpak/io.github.deckscreenshare.Agent.yml
```

Open Steam in Desktop Mode:

1. Select **Games > Add a Non-Steam Game**.
2. Add **Deck Screen Share**.
3. Return to Gaming Mode and launch Deck Screen Share.
4. Keep it running and launch the game you want to record.

The Flatpak listens for receiver commands on TCP port `8765`. UDP ports `9000`
and `9002` carry preview video and audio. UDP ports `9001` and `9003` carry
recording video and audio.

The SteamOS agent is always headless so its lifetime is not tied to a Gamescope
window. Its persistent diagnostic log is stored at:

```text
~/.var/app/io.github.deckscreenshare.Agent/data/DeckScreenShare/agent.log
```

Stop the agent from the Steam library when it is no longer needed.

## SteamOS Capture Notes

`pipewire` is the default capture mode for Gaming Mode. It receives the
recording stream published by Gamescope without requiring privileged DRM/KMS
framebuffer access. The Flatpak uses PipeWire's manager socket so that the
Gamescope recording node is visible inside the sandbox.

The SteamOS agent validates the requested DRM and VAAPI device paths and
automatically falls back to the first available `/dev/dri/card*` and
`/dev/dri/renderD*` devices. Both paths can still be changed in the Windows
receiver.

For Desktop Mode testing, select `x11grab` in the Windows receiver.

Available encoders depend on the FFmpeg build and Steam Deck hardware. Hardware
AV1 encoding may be unavailable, and software AV1 encoding may be too expensive
while playing a game.

## Build The Windows Receiver

Download `DeckScreenShare-Windows-x64.zip` from the latest
[GitHub Release](https://github.com/alexeygasenko/DeckScreenShare/releases) and
extract it, or build it from source.

Requirements:

- .NET 8 SDK
- FFmpeg for Windows

Build:

```powershell
dotnet build DeckScreenShare.sln -c Release
```

Place `ffmpeg.exe` beside the built `DeckScreenShare.exe`, or add FFmpeg to
`PATH`. Start the application and it immediately begins waiting for the SteamOS
agent. When the agent becomes available, preview video and audio connect
automatically. Use the volume and mute controls below the preview. The recording
button toggles recording on and off.

Recordings are saved to `Videos\DeckScreenShare` by default. On first launch,
allow the application and FFmpeg through Windows Firewall for private networks.

If the SteamOS agent is unavailable, the preview remains black and reports that
there is no connection. The receiver retries automatically.

## Automated Builds

GitHub Actions builds both applications on every push and pull request. Tags
matching `v*`, such as `v0.1.0`, automatically create a GitHub Release with:

- `DeckScreenShare-SteamOS.flatpak`
- `DeckScreenShare-Windows-x64.zip`

## Container Compatibility

| Container | H.264 | H.265 | AV1 | Audio |
|---|---:|---:|---:|---|
| MKV | yes | yes | yes | Opus, copied |
| MP4 | yes | yes | yes | AAC |
| MOV | yes | yes | yes | AAC |
| WebM | no | no | yes | Opus, copied |
| MPEG-TS | yes | yes | no | AAC |

## Audio

The default source is `@DEFAULT_MONITOR@`, which captures the current system
audio output. For unusual audio configurations, select another source reported
by:

```bash
pactl list short sources
```

## Security

The SteamOS HTTP control API does not implement authentication. Use it only on a
trusted local network. Do not expose control port `8765` or UDP ports `9000`
through `9003` to the internet.
