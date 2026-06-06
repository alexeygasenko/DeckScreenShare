using System.Text.Json.Serialization;

namespace DeckScreenShare.Receiver;

public sealed record StreamSettings(
    [property: JsonPropertyName("receiver_host")] string ReceiverHost,
    [property: JsonPropertyName("srt_port")] int SrtPort,
    [property: JsonPropertyName("codec")] string Codec,
    [property: JsonPropertyName("backend")] string Backend,
    [property: JsonPropertyName("capture_mode")] string CaptureMode,
    [property: JsonPropertyName("fps")] int Fps,
    [property: JsonPropertyName("video_bitrate_kbps")] int VideoBitrateKbps,
    [property: JsonPropertyName("audio_bitrate_kbps")] int AudioBitrateKbps,
    [property: JsonPropertyName("latency_ms")] int LatencyMs,
    [property: JsonPropertyName("size")] string Size,
    [property: JsonPropertyName("display")] string Display,
    [property: JsonPropertyName("audio_source")] string AudioSource);

public sealed record RecordingSettings(
    string FfmpegPath,
    int SrtPort,
    int LatencyMs,
    string Codec,
    string Container,
    int AudioBitrateKbps,
    string OutputPath,
    string PreviewPath);

