# Deck Screen Share

Deck Screen Share records a Steam Deck display and system audio over Wi-Fi while
showing a live preview on a Windows PC. Both devices must be connected to the
same local network.

## Features

- A C++/GTK SteamOS application packaged as a Flatpak.
- The SteamOS application can be added as a Non-Steam Game and kept running
  while another game is open.
- A native C# WPF receiver for Windows.
- H.264, H.265/HEVC, and AV1 video encoding.
- Configurable video bitrate, audio bitrate, FPS, and SRT latency.
- MKV, MP4, MOV, WebM, and MPEG-TS recording containers.
- Built-in live preview.
- SRT transport designed to tolerate packet loss on a local Wi-Fi network.

## Architecture

The SteamOS Flatpak starts FFmpeg, captures the display and system audio, and
sends a Matroska stream over SRT. The Windows application receives that stream,
saves it to the selected container, and decodes reduced JPEG frames for its live
preview.

MKV and WebM preserve the original video and Opus audio without transcoding.
MP4, MOV, and MPEG-TS preserve the original video and transcode audio to AAC for
container compatibility.

The SteamOS application is implemented in C++17 using GTK 3, GIO, and
JSON-GLib. Python is not used anywhere in the project.

## Build And Install The SteamOS Flatpak

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

Run it directly for a quick check:

```bash
flatpak run io.github.deckscreenshare.Agent
```

Open Steam in Desktop Mode:

1. Select **Games > Add a Non-Steam Game**.
2. Add **Deck Screen Share**.
3. Return to Gaming Mode and launch Deck Screen Share.
4. Keep it running and launch the game you want to record.

The Flatpak listens for receiver commands on TCP port `8765` and sends SRT
video to the Windows receiver on UDP port `9000`.

## SteamOS Capture Notes

`kmsgrab` is the default capture mode for Gaming Mode. It needs direct access to
DRM devices, so the Flatpak requests broad device access with `--device=all`.
SteamOS or Gamescope updates may still restrict DRM capture. Test capture on the
target SteamOS version before relying on it.

For Desktop Mode testing, select `x11grab` in the Windows receiver.

Available encoders depend on the FFmpeg build and Steam Deck hardware. Hardware
AV1 encoding may be unavailable, and software AV1 encoding may be too expensive
while playing a game.

## Build The Windows Receiver

Requirements:

- .NET 8 SDK
- FFmpeg for Windows with SRT support

Build:

```powershell
dotnet build DeckScreenShare.sln -c Release
```

Place `ffmpeg.exe` beside the built `DeckScreenShare.exe`, or add FFmpeg to
`PATH`. Start the application, enter the Steam Deck IP and the Windows PC local
IP, select the recording settings, and click **Start recording**.

Recordings are saved to `Videos\DeckScreenShare` by default. On first launch,
allow the application and FFmpeg through Windows Firewall for private networks.

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
trusted local network. Do not expose ports `8765` or `9000` to the internet.
