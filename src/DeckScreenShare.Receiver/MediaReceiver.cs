using System.Diagnostics;
using NAudio.Wave;

namespace DeckScreenShare.Receiver;

public sealed class MediaReceiver : IDisposable
{
    private readonly object _sync = new();
    private Process? _video;
    private Process? _audio;
    private Process? _recording;
    private WaveOutEvent? _audioOutput;
    private BufferedWaveProvider? _audioBuffer;
    private CancellationTokenSource? _previewCancellation;

    public event Action<byte[]>? FrameReady;
    public event Action<string>? Log;
    public event Action? RecordingExited;

    public bool IsRecording => Running(_recording);
    public bool IsPreviewRunning => Running(_video);

    public void StartPreview(PreviewSettings settings)
    {
        StopPreview();
        _previewCancellation = new CancellationTokenSource();
        var cancellation = _previewCancellation.Token;
        var input = UdpInput(settings.SrtPort);

        _video = Start(settings.FfmpegPath,
        [
            "-hide_banner", "-loglevel", "warning", "-fflags", "nobuffer",
            "-flags", "low_delay", "-analyzeduration", "0", "-probesize", "64k",
            "-i", input, "-map", "0:v:0", "-an",
            "-vf", $"scale={settings.Width}:{settings.Height}",
            "-pix_fmt", "bgra", "-f", "rawvideo", "pipe:1"
        ], redirectOutput: true);

        _audio = Start(settings.FfmpegPath,
        [
            "-hide_banner", "-loglevel", "warning", "-fflags", "nobuffer",
            "-flags", "low_delay", "-analyzeduration", "0", "-probesize", "64k",
            "-i", UdpInput(settings.SrtPort + 2), "-map", "0:a:0", "-vn",
            "-f", "s16le", "-acodec", "pcm_s16le", "-ar", "48000", "-ac", "2", "pipe:1"
        ], redirectOutput: true);

        _audioBuffer = new BufferedWaveProvider(new WaveFormat(48000, 16, 2))
        {
            BufferDuration = TimeSpan.FromMilliseconds(500),
            DiscardOnBufferOverflow = true
        };
        try
        {
            _audioOutput = new WaveOutEvent { DesiredLatency = 100 };
            _audioOutput.Init(_audioBuffer);
            _audioOutput.Play();
        }
        catch (Exception error)
        {
            Log?.Invoke($"Preview audio output: {error.Message}");
            _audioOutput?.Dispose();
            _audioOutput = null;
        }

        _ = ReadVideoAsync(_video.StandardOutput.BaseStream, settings.Width * settings.Height * 4, cancellation);
        _ = ReadAudioAsync(_audio.StandardOutput.BaseStream, cancellation);
    }

    public void SetVolume(double volume, bool muted)
    {
        if (_audioOutput is not null)
            _audioOutput.Volume = muted ? 0f : (float)Math.Clamp(volume, 0, 1);
    }

    public void StartRecording(RecordingSettings settings)
    {
        StopRecording();
        ValidateContainer(settings.Codec, settings.Container);
        var args = new List<string>
        {
            "-hide_banner", "-loglevel", "warning", "-y",
            "-i", UdpInput(settings.SrtPort + 1),
            "-i", UdpInput(settings.SrtPort + 3),
            "-map", "0:v:0", "-map", "1:a:0?"
        };
        AddRecordingArguments(args, settings);
        var process = Start(settings.FfmpegPath, args, redirectOutput: false);
        _recording = process;
        process.Exited += (_, _) =>
        {
            if (!ReferenceEquals(_recording, process))
                return;
            _recording = null;
            RecordingExited?.Invoke();
        };
        if (process.HasExited)
        {
            _recording = null;
            RecordingExited?.Invoke();
        }
    }

    public void StopRecording()
    {
        StopProcess(ref _recording, graceful: true);
    }

    public void StopPreview()
    {
        _previewCancellation?.Cancel();
        _previewCancellation?.Dispose();
        _previewCancellation = null;
        StopProcess(ref _video, graceful: false);
        StopProcess(ref _audio, graceful: false);
        _audioOutput?.Stop();
        _audioOutput?.Dispose();
        _audioOutput = null;
        _audioBuffer = null;
    }

    private async Task ReadVideoAsync(Stream stream, int frameSize, CancellationToken cancellation)
    {
        try
        {
            while (!cancellation.IsCancellationRequested)
            {
                var frame = new byte[frameSize];
                if (!await ReadExactAsync(stream, frame, cancellation))
                    return;
                FrameReady?.Invoke(frame);
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception error) { Log?.Invoke($"Preview video: {error.Message}"); }
    }

    private async Task ReadAudioAsync(Stream stream, CancellationToken cancellation)
    {
        var buffer = new byte[16384];
        try
        {
            while (!cancellation.IsCancellationRequested)
            {
                var read = await stream.ReadAsync(buffer, cancellation);
                if (read == 0)
                    return;
                lock (_sync)
                    _audioBuffer?.AddSamples(buffer, 0, read);
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception error) { Log?.Invoke($"Preview audio: {error.Message}"); }
    }

    private static async Task<bool> ReadExactAsync(
        Stream stream, byte[] buffer, CancellationToken cancellation)
    {
        var offset = 0;
        while (offset < buffer.Length)
        {
            var read = await stream.ReadAsync(buffer.AsMemory(offset), cancellation);
            if (read == 0)
                return false;
            offset += read;
        }
        return true;
    }

    private Process Start(string path, IEnumerable<string> args, bool redirectOutput)
    {
        var info = new ProcessStartInfo(path)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = redirectOutput,
            RedirectStandardError = true
        };
        foreach (var arg in args)
            info.ArgumentList.Add(arg);
        var process = new Process { StartInfo = info, EnableRaisingEvents = true };
        process.ErrorDataReceived += (_, eventArgs) =>
        {
            if (!string.IsNullOrWhiteSpace(eventArgs.Data))
                Log?.Invoke(eventArgs.Data);
        };
        process.Start();
        process.BeginErrorReadLine();
        return process;
    }

    private static string UdpInput(int port) =>
        $"udp://0.0.0.0:{port}?fifo_size=1000000&overrun_nonfatal=1";

    private static bool Running(Process? process)
    {
        try { return process is { HasExited: false }; }
        catch { return false; }
    }

    private static void StopProcess(ref Process? process, bool graceful)
    {
        var current = process;
        process = null;
        if (current is null)
            return;
        try
        {
            if (!current.HasExited && graceful)
            {
                current.StandardInput.WriteLine("q");
                if (!current.WaitForExit(2500))
                    current.Kill(true);
            }
            else if (!current.HasExited)
            {
                current.Kill(true);
            }
        }
        catch { }
        finally { current.Dispose(); }
    }

    private static void AddRecordingArguments(List<string> args, RecordingSettings settings)
    {
        if (settings.Container is "mkv" or "webm")
            args.AddRange(["-c:v", "copy", "-c:a", "copy"]);
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

    public void Dispose()
    {
        StopRecording();
        StopPreview();
    }
}
