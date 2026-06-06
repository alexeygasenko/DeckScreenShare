using System.Diagnostics;

namespace DeckScreenShare.Receiver;

public sealed class FfmpegReceiver : IDisposable
{
    private Process? _process;
    public bool IsRunning
    {
        get
        {
            try { return _process is { HasExited: false }; }
            catch (InvalidOperationException) { return false; }
        }
    }
    public event Action<string>? Log;

    public void Start(RecordingSettings settings)
    {
        Stop();
        ValidateContainer(settings.Codec, settings.Container);

        var srtUrl = $"srt://0.0.0.0:{settings.SrtPort}?mode=listener&transtype=live&latency={settings.LatencyMs * 1000}";
        var args = new List<string>
        {
            "-hide_banner", "-loglevel", "warning", "-y", "-i", srtUrl,
            "-map", "0:v:0", "-map", "0:a:0?"
        };
        AddRecordingArguments(args, settings);
        args.AddRange([
            "-map", "0:v:0", "-an",
            "-vf", "fps=12,scale=1280:-2",
            "-q:v", "5", "-update", "1", "-f", "image2", settings.PreviewPath
        ]);

        var info = new ProcessStartInfo(settings.FfmpegPath)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardError = true
        };
        foreach (var arg in args)
            info.ArgumentList.Add(arg);

        var process = new Process { StartInfo = info, EnableRaisingEvents = true };
        process.ErrorDataReceived += (_, e) => { if (!string.IsNullOrWhiteSpace(e.Data)) Log?.Invoke(e.Data); };
        process.Start();
        process.BeginErrorReadLine();
        _process = process;
    }

    private static void AddRecordingArguments(List<string> args, RecordingSettings settings)
    {
        if (settings.Container is "mkv" or "webm")
        {
            args.AddRange(["-c:v", "copy", "-c:a", "copy"]);
        }
        else
        {
            args.AddRange(["-c:v", "copy", "-c:a", "aac", "-b:a", $"{settings.AudioBitrateKbps}k"]);
            if (settings.Container is "mp4" or "mov")
            {
                args.AddRange(["-movflags", "+faststart"]);
                if (settings.Codec == "h265")
                    args.AddRange(["-tag:v", "hvc1"]);
            }
            if (settings.Container == "ts")
                args.AddRange(["-f", "mpegts"]);
        }
        args.Add(settings.OutputPath);
    }

    public static void ValidateContainer(string codec, string container)
    {
        if (container == "webm" && codec != "av1")
            throw new InvalidOperationException("This application supports WebM only with AV1.");
        if (container == "ts" && codec == "av1")
            throw new InvalidOperationException("Select MKV, MP4, MOV, or WebM for AV1.");
    }

    public void Stop()
    {
        if (!IsRunning)
        {
            _process?.Dispose();
            _process = null;
            return;
        }

        try
        {
            _process!.StandardInput.WriteLine("q");
            if (!_process.WaitForExit(8000))
                _process.Kill(true);
        }
        catch
        {
            if (IsRunning)
                _process!.Kill(true);
        }
        finally
        {
            _process?.Dispose();
            _process = null;
        }
    }

    public void Dispose() => Stop();
}
