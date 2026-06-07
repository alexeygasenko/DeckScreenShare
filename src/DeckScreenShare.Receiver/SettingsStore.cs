using System.IO;
using System.Text.Json;

namespace DeckScreenShare.Receiver;

public sealed class SettingsStore
{
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    private readonly string _path;

    public SettingsStore()
    {
        var directory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "DeckScreenShare");
        Directory.CreateDirectory(directory);
        _path = Path.Combine(directory, "settings.json");
        DiagnosticsPath = Path.Combine(directory, "diagnostics.txt");
    }

    public string DiagnosticsPath { get; }

    public AppSettings Load()
    {
        try
        {
            return File.Exists(_path)
                ? JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(_path), JsonOptions) ?? new AppSettings()
                : new AppSettings();
        }
        catch (JsonException)
        {
            return new AppSettings();
        }
        catch (IOException)
        {
            return new AppSettings();
        }
    }

    public void Save(AppSettings settings)
    {
        var temporaryPath = _path + ".tmp";
        File.WriteAllText(temporaryPath, JsonSerializer.Serialize(settings, JsonOptions));
        File.Move(temporaryPath, _path, true);
    }

    public void SaveDiagnostics(AgentDiagnostics diagnostics)
    {
        File.WriteAllText(
            DiagnosticsPath,
            $"Capture stderr:\n{diagnostics.CaptureStderr}\n\n" +
            $"FFmpeg stderr:\n{diagnostics.FfmpegStderr}\n\n" +
            $"PipeWire ports and links:\n{diagnostics.PipewirePorts}\n\n" +
            $"PipeWire nodes:\n{diagnostics.PipewireNodes}\n");
    }
}
