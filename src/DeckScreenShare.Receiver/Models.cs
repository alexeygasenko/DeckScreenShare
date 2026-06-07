using System.Text.Json.Serialization;

namespace DeckScreenShare.Receiver;

public sealed record StreamSettings(
    [property: JsonPropertyName("receiver_host")] string ReceiverHost,
    [property: JsonPropertyName("srt_port")] int SrtPort,
    [property: JsonPropertyName("codec")] string Codec,
    [property: JsonPropertyName("backend")] string Backend,
    [property: JsonPropertyName("capture_mode")] string CaptureMode,
    [property: JsonPropertyName("pipewire_pipeline")] string PipewirePipeline,
    [property: JsonPropertyName("fps")] int Fps,
    [property: JsonPropertyName("video_bitrate_kbps")] int VideoBitrateKbps,
    [property: JsonPropertyName("audio_bitrate_kbps")] int AudioBitrateKbps,
    [property: JsonPropertyName("latency_ms")] int LatencyMs,
    [property: JsonPropertyName("size")] string Size,
    [property: JsonPropertyName("display")] string Display,
    [property: JsonPropertyName("audio_source")] string AudioSource,
    [property: JsonPropertyName("drm_device")] string DrmDevice,
    [property: JsonPropertyName("vaapi_device")] string VaapiDevice);

public sealed record RecordingSettings(
    string FfmpegPath,
    int SrtPort,
    int LatencyMs,
    string Codec,
    string Container,
    int AudioBitrateKbps,
    string OutputPath);

public sealed record PreviewSettings(
    string FfmpegPath,
    int SrtPort,
    int Width,
    int Height);

public sealed record AgentStatus(
    [property: JsonPropertyName("running")] bool Running,
    [property: JsonPropertyName("last_error")] string? LastError,
    [property: JsonPropertyName("exit_code")] int? ExitCode,
    [property: JsonPropertyName("protocol_version")] int ProtocolVersion,
    [property: JsonPropertyName("app_version")] string? AppVersion);

public sealed record AgentDiagnostics(
    [property: JsonPropertyName("pipewire_nodes")] string? PipewireNodes,
    [property: JsonPropertyName("pipewire_ports")] string? PipewirePorts,
    [property: JsonPropertyName("capture_stderr")] string? CaptureStderr,
    [property: JsonPropertyName("ffmpeg_stderr")] string? FfmpegStderr);

public sealed class AppSettings
{
    public string DeckHost { get; set; } = "192.168.1.50";
    public int DeckPort { get; set; } = 8765;
    public string ReceiverHost { get; set; } = "";
    public string Codec { get; set; } = "h264";
    public string Backend { get; set; } = "vaapi";
    public int VideoBitrateKbps { get; set; } = 8000;
    public int AudioBitrateKbps { get; set; } = 160;
    public int Fps { get; set; } = 60;
    public int LatencyMs { get; set; } = 120;
    public string CaptureMode { get; set; } = "pipewire";
    public string Container { get; set; } = "mkv";
    public string Size { get; set; } = "1280x800";
    public string Display { get; set; } = ":0.0";
    public string AudioSource { get; set; } = "@DEFAULT_MONITOR@";
    public string DrmDevice { get; set; } = "/dev/dri/card0";
    public string VaapiDevice { get; set; } = "/dev/dri/renderD128";
    public string OutputFolder { get; set; } = "";
    public double PreviewVolume { get; set; } = 0.8;
    public bool PreviewMuted { get; set; }
}
