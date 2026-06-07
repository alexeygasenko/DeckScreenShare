using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Microsoft.Win32;

namespace DeckScreenShare.Receiver;

public partial class MainWindow : Window
{
    private const int SrtPort = 9000;
    private readonly AgentClient _agent = new();
    private readonly FfmpegReceiver _receiver = new();
    private readonly SettingsStore _settingsStore = new();
    private readonly DispatcherTimer _previewTimer;
    private readonly DispatcherTimer _settingsSaveTimer;
    private OperationMode _mode;
    private bool _busy;
    private bool _allowClose;
    private string _lastLog = "";

    public MainWindow()
    {
        InitializeComponent();
        var version = Assembly.GetExecutingAssembly().GetName().Version;
        VersionText.Text = version is null
            ? ""
            : $"Windows receiver {version.Major}.{version.Minor}.{version.Build}";
        ApplySettings(_settingsStore.Load());
        _receiver.Log += message => Dispatcher.Invoke(() =>
        {
            _lastLog = message;
            if (_mode != OperationMode.Idle)
                StatusText.Text = message;
        });
        _receiver.Exited += () => Dispatcher.Invoke(() =>
        {
            if (_mode != OperationMode.Idle)
                StatusText.Text = string.IsNullOrWhiteSpace(_lastLog)
                    ? "The local FFmpeg receiver stopped unexpectedly."
                    : _lastLog;
        });
        _previewTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(120) };
        _previewTimer.Tick += (_, _) => RefreshPreview();
        _settingsSaveTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(600) };
        _settingsSaveTimer.Tick += (_, _) =>
        {
            _settingsSaveTimer.Stop();
            SaveSettings(showError: false);
        };
        AddHandler(TextBox.TextChangedEvent, new TextChangedEventHandler((_, _) => ScheduleSettingsSave()));
        AddHandler(ComboBox.SelectionChangedEvent, new SelectionChangedEventHandler((_, _) => ScheduleSettingsSave()));
        UpdateButtons();
    }

    private async void Test_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            SetBusy(true, "Checking connection...");
            var settings = ReadSettings();
            SaveSettings(settings);
            await _agent.CheckAsync(settings.DeckHost, settings.DeckPort);

            PreparePreview();
            _receiver.StartPreview(new PreviewSettings(
                FindFfmpeg(), SrtPort, settings.LatencyMs, _settingsStore.PreviewPath));
            await Task.Delay(400);
            await _agent.StartAsync(settings.DeckHost, settings.DeckPort, BuildStreamSettings(settings));

            _mode = OperationMode.Preview;
            _previewTimer.Start();
            await WaitForFirstFrameAsync(settings);
            StatusText.Text = "Connection successful. Live preview is active.";
        }
        catch (Exception ex)
        {
            await CleanupFailedStartAsync();
            ShowError(ex, "Connection test failed");
        }
        finally
        {
            SetBusy(false);
        }
    }

    private async void Start_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            SetBusy(true, "Starting recording...");
            var settings = ReadSettings();
            settings.OutputFolder = Required(settings.OutputFolder, "Recording folder");
            FfmpegReceiver.ValidateContainer(settings.Codec, settings.Container);
            SaveSettings(settings);
            await _agent.CheckAsync(settings.DeckHost, settings.DeckPort);

            Directory.CreateDirectory(settings.OutputFolder);
            PreparePreview();
            var output = Path.Combine(
                settings.OutputFolder,
                $"deck_{DateTime.Now:yyyy-MM-dd_HH-mm-ss}.{settings.Container}");
            _receiver.Start(new RecordingSettings(
                FindFfmpeg(), SrtPort, settings.LatencyMs, settings.Codec,
                settings.Container, settings.AudioBitrateKbps, output, _settingsStore.PreviewPath));
            await Task.Delay(400);
            await _agent.StartAsync(settings.DeckHost, settings.DeckPort, BuildStreamSettings(settings));

            _mode = OperationMode.Recording;
            _previewTimer.Start();
            await WaitForFirstFrameAsync(settings);
            StatusText.Text = $"Recording: {output}";
        }
        catch (Exception ex)
        {
            await CleanupFailedStartAsync();
            ShowError(ex, "Could not start recording");
        }
        finally
        {
            SetBusy(false);
        }
    }

    private async void Stop_Click(object sender, RoutedEventArgs e) => await StopAsync();

    private async Task StopAsync()
    {
        var stoppedMode = _mode;
        SetBusy(true, "Stopping...");
        _previewTimer.Stop();
        await Task.WhenAll(_agent.StopAsync(), _receiver.StopAsync());
        _mode = OperationMode.Idle;
        PreviewHint.Visibility = Visibility.Visible;
        StatusText.Text = stoppedMode == OperationMode.Recording
            ? "Recording stopped and saved."
            : "Preview stopped.";
        SetBusy(false);
    }

    private async Task CleanupFailedStartAsync()
    {
        _previewTimer.Stop();
        await Task.WhenAll(_agent.StopAsync(), _receiver.StopAsync());
        _mode = OperationMode.Idle;
        PreviewHint.Visibility = Visibility.Visible;
    }

    private async Task WaitForFirstFrameAsync(AppSettings settings)
    {
        var deadline = DateTime.UtcNow.AddSeconds(12);
        while (DateTime.UtcNow < deadline)
        {
            if (File.Exists(_settingsStore.PreviewPath) &&
                new FileInfo(_settingsStore.PreviewPath).Length > 0 &&
                RefreshPreview())
            {
                return;
            }

            if (!_receiver.IsRunning)
                throw new InvalidOperationException(
                    string.IsNullOrWhiteSpace(_lastLog)
                        ? "The local FFmpeg receiver stopped before the first frame arrived."
                        : _lastLog);

            var agentStatus = await _agent.GetStatusAsync(settings.DeckHost, settings.DeckPort);
            if (!agentStatus.Running)
            {
                var details = string.IsNullOrWhiteSpace(agentStatus.LastError)
                    ? $"SteamOS FFmpeg exited with code {agentStatus.ExitCode?.ToString() ?? "unknown"}."
                    : agentStatus.LastError.Trim();
                throw new InvalidOperationException(details);
            }

            StatusText.Text = "Connected to the agent. Waiting for the first video frame...";
            await Task.Delay(350);
        }

        var diagnostics = await _agent.GetDiagnosticsAsync(settings.DeckHost, settings.DeckPort);
        _settingsStore.SaveDiagnostics(diagnostics);
        throw new TimeoutException(
            "The SteamOS agent is running, but no video frame reached this PC. " +
            $"Full diagnostics were saved to:\n{_settingsStore.DiagnosticsPath}");
    }

    private AppSettings ReadSettings() => new()
    {
        DeckHost = Required(DeckHostBox.Text, "Steam Deck IP address"),
        DeckPort = PositiveInt(DeckPortBox.Text, "Agent port"),
        ReceiverHost = Required(ReceiverHostBox.Text, "This PC IP address"),
        Codec = Selected(CodecBox),
        Backend = Selected(BackendBox),
        VideoBitrateKbps = PositiveInt(VideoBitrateBox.Text, "Video bitrate"),
        AudioBitrateKbps = PositiveInt(AudioBitrateBox.Text, "Audio bitrate"),
        Fps = PositiveInt(FpsBox.Text, "FPS"),
        LatencyMs = PositiveInt(LatencyBox.Text, "Latency"),
        CaptureMode = Selected(CaptureModeBox),
        Container = Selected(ContainerBox),
        Size = Required(SizeBox.Text, "Display size"),
        Display = Required(DisplayBox.Text, "Display"),
        AudioSource = Required(AudioSourceBox.Text, "Audio source"),
        DrmDevice = Required(DrmDeviceBox.Text, "DRM device"),
        VaapiDevice = Required(VaapiDeviceBox.Text, "VAAPI device"),
        OutputFolder = OutputFolderBox.Text.Trim()
    };

    private static StreamSettings BuildStreamSettings(AppSettings settings) => new(
        settings.ReceiverHost, SrtPort, settings.Codec,
        settings.CaptureMode == "pipewire" ? "vaapi" : settings.Backend,
        settings.CaptureMode, "vaapi",
        settings.Fps, settings.VideoBitrateKbps, settings.AudioBitrateKbps, settings.LatencyMs,
        settings.Size, settings.Display, settings.AudioSource, settings.DrmDevice,
        settings.VaapiDevice);

    private void ApplySettings(AppSettings settings)
    {
        DeckHostBox.Text = settings.DeckHost;
        DeckPortBox.Text = settings.DeckPort.ToString();
        ReceiverHostBox.Text = string.IsNullOrWhiteSpace(settings.ReceiverHost)
            ? GetLocalIp()
            : settings.ReceiverHost;
        Select(CodecBox, settings.Codec);
        if (settings.CaptureMode == "kmsgrab")
            settings.CaptureMode = "pipewire";
        if (settings.CaptureMode == "pipewire")
            settings.Backend = "vaapi";
        Select(BackendBox, settings.CaptureMode == "kmsgrab" && settings.Backend == "software"
            ? "vaapi"
            : settings.Backend);
        VideoBitrateBox.Text = settings.VideoBitrateKbps.ToString();
        AudioBitrateBox.Text = settings.AudioBitrateKbps.ToString();
        FpsBox.Text = settings.Fps.ToString();
        LatencyBox.Text = settings.LatencyMs.ToString();
        Select(CaptureModeBox, settings.CaptureMode);
        Select(ContainerBox, settings.Container);
        SizeBox.Text = settings.Size;
        DisplayBox.Text = settings.Display;
        AudioSourceBox.Text = settings.AudioSource;
        DrmDeviceBox.Text = settings.DrmDevice;
        VaapiDeviceBox.Text = settings.VaapiDevice;
        OutputFolderBox.Text = string.IsNullOrWhiteSpace(settings.OutputFolder)
            ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos), "DeckScreenShare")
            : settings.OutputFolder;
    }

    private void ScheduleSettingsSave()
    {
        _settingsSaveTimer.Stop();
        _settingsSaveTimer.Start();
    }

    private void SaveSettings(AppSettings? settings = null, bool showError = true)
    {
        try
        {
            _settingsStore.Save(settings ?? ReadSettings());
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidOperationException)
        {
            if (showError)
                StatusText.Text = $"Could not save settings: {ex.Message}";
        }
    }

    private void PreparePreview()
    {
        TryDelete(_settingsStore.PreviewPath);
        PreviewImage.Source = null;
        PreviewHint.Visibility = Visibility.Visible;
        _lastLog = "";
    }

    private bool RefreshPreview()
    {
        if (!File.Exists(_settingsStore.PreviewPath))
            return false;
        try
        {
            using var stream = new FileStream(
                _settingsStore.PreviewPath, FileMode.Open, FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete);
            var image = new BitmapImage();
            image.BeginInit();
            image.CacheOption = BitmapCacheOption.OnLoad;
            image.StreamSource = stream;
            image.EndInit();
            image.Freeze();
            PreviewImage.Source = image;
            PreviewHint.Visibility = Visibility.Collapsed;
            return true;
        }
        catch (IOException) { return false; }
        catch (NotSupportedException) { return false; }
        catch (FileFormatException) { return false; }
    }

    private void Browse_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "Recording folder", Multiselect = false };
        if (dialog.ShowDialog() == true)
        {
            OutputFolderBox.Text = dialog.FolderName;
            SaveSettings();
        }
    }

    private void ShowError(Exception exception, string title)
    {
        MessageBox.Show(exception.Message, title, MessageBoxButton.OK, MessageBoxImage.Error);
        StatusText.Text = string.IsNullOrWhiteSpace(_lastLog) ? exception.Message : _lastLog;
    }

    private void SetBusy(bool busy, string? status = null)
    {
        _busy = busy;
        if (status is not null)
            StatusText.Text = status;
        UpdateButtons();
    }

    private void UpdateButtons()
    {
        TestButton.IsEnabled = !_busy && _mode == OperationMode.Idle;
        StartButton.IsEnabled = !_busy && _mode == OperationMode.Idle;
        StopButton.IsEnabled = !_busy && _mode != OperationMode.Idle;
    }

    private static string Selected(ComboBox box) =>
        ((ComboBoxItem)box.SelectedItem).Content.ToString()!;

    private static void Select(ComboBox box, string value)
    {
        foreach (ComboBoxItem item in box.Items)
        {
            if (string.Equals(item.Content.ToString(), value, StringComparison.OrdinalIgnoreCase))
            {
                box.SelectedItem = item;
                return;
            }
        }
        box.SelectedIndex = 0;
    }

    private static string Required(string text, string name) =>
        string.IsNullOrWhiteSpace(text)
            ? throw new InvalidOperationException($"{name} is required.")
            : text.Trim();

    private static int PositiveInt(string text, string name) =>
        int.TryParse(text, out var value) && value > 0
            ? value
            : throw new InvalidOperationException($"{name} must be a positive number.");

    private static string FindFfmpeg()
    {
        var bundled = Path.Combine(AppContext.BaseDirectory, "ffmpeg.exe");
        return File.Exists(bundled) ? bundled : "ffmpeg.exe";
    }

    private static string GetLocalIp()
    {
        using var socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        try
        {
            socket.Connect("10.255.255.255", 1);
            return ((IPEndPoint)socket.LocalEndPoint!).Address.ToString();
        }
        catch
        {
            return "127.0.0.1";
        }
    }

    private static void TryDelete(string path)
    {
        try { File.Delete(path); } catch (IOException) { }
    }

    private async void Window_Closing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        SaveSettings();
        if (_allowClose || _mode == OperationMode.Idle)
            return;

        e.Cancel = true;
        await StopAsync();
        _allowClose = true;
        Close();
    }

    private enum OperationMode
    {
        Idle,
        Preview,
        Recording
    }
}
