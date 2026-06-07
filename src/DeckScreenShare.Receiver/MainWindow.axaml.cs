using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;

namespace DeckScreenShare.Receiver;

public sealed partial class MainWindow : Window
{
    private const int StreamPort = 9000;
    private readonly AgentClient _agent = new();
    private readonly MediaReceiver _media = new();
    private readonly SettingsStore _settingsStore = new();
    private readonly DispatcherTimer _connectionTimer;
    private readonly DispatcherTimer _saveTimer;
    private WriteableBitmap? _previewBitmap;
    private AppSettings _settings = new();
    private bool _connected;
    private bool _connecting;
    private bool _closing;
    private bool _hasFrame;
    private bool _recording;
    private DateTime _lastFrameAt;
    private string _activeConfig = "";

    public string[] Codecs { get; } = ["h264", "h265", "av1"];
    public string[] Backends { get; } = ["vaapi", "software"];
    public string[] CaptureModes { get; } = ["pipewire", "x11grab", "kmsgrab"];
    public string[] Containers { get; } = ["mkv", "mp4", "mov", "webm", "ts"];

    public MainWindow()
    {
        InitializeComponent();
        DataContext = this;
        var version = Assembly.GetExecutingAssembly().GetName().Version;
        VersionText.Text = version is null ? "" : $"v{version.Major}.{version.Minor}.{version.Build}";
        ApplySettings(_settingsStore.Load());

        _media.FrameReady += OnFrameReady;
        _media.Log += message => Dispatcher.UIThread.Post(() =>
        {
            if (!_connected)
                StatusText.Text = message;
        });
        _media.RecordingExited += () => Dispatcher.UIThread.Post(() =>
        {
            if (!_recording)
                return;
            _recording = false;
            RecordButton.Content = "Start recording";
            RecordButton.Classes.Remove("recording");
            StatusText.Text = "Recording stopped unexpectedly. Check the selected codec and output folder.";
        });

        _connectionTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
        _connectionTimer.Tick += async (_, _) => await EnsureConnectedAsync();
        _saveTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(600) };
        _saveTimer.Tick += (_, _) =>
        {
            _saveTimer.Stop();
            SaveSettings();
        };

        AddHandler(TextBox.TextChangedEvent, (_, _) => ScheduleSave());
        AddHandler(SelectingItemsControl.SelectionChangedEvent, (_, _) => ScheduleSave());
        Opened += async (_, _) =>
        {
            StartPreviewReceiver();
            _connectionTimer.Start();
            await EnsureConnectedAsync();
        };
        Closing += Window_Closing;
    }

    private void StartPreviewReceiver()
    {
        try
        {
            var (width, height) = ParseSize(_settings.Size);
            _previewBitmap = new WriteableBitmap(
                new PixelSize(width, height), new Vector(96, 96),
                PixelFormat.Bgra8888, AlphaFormat.Unpremul);
            PreviewImage.Source = _previewBitmap;
            _media.StartPreview(new PreviewSettings(FindFfmpeg(), StreamPort, width, height));
            _media.SetVolume(_settings.PreviewVolume, _settings.PreviewMuted);
        }
        catch (Exception error)
        {
            _media.StopPreview();
            StatusText.Text = $"Preview receiver: {error.Message}";
        }
    }

    private async Task EnsureConnectedAsync()
    {
        if (_connecting || _closing)
            return;
        _connecting = true;
        try
        {
            _settings = ReadSettings();
            if (!_media.IsPreviewRunning)
                StartPreviewReceiver();
            var status = await _agent.GetStatusAsync(_settings.DeckHost, _settings.DeckPort);
            var config = ConfigKey(_settings);
            if (!status.Running || config != _activeConfig)
            {
                await _agent.StartAsync(
                    _settings.DeckHost, _settings.DeckPort, BuildStreamSettings(_settings));
                _activeConfig = config;
            }
            _connected = true;
            StatusText.Text = _hasFrame ? "Live preview" : "Connected. Waiting for video...";
            ConnectionHeading.Text = "Connected";
            ConnectionDetail.Text = "Waiting for the next video frame";
        }
        catch
        {
            SetDisconnected();
        }
        finally
        {
            _connecting = false;
        }

        if (_connected && _hasFrame && DateTime.UtcNow - _lastFrameAt > TimeSpan.FromSeconds(4))
        {
            _connected = false;
            _hasFrame = false;
            SetDisconnected();
        }
    }

    private void OnFrameReady(byte[] frame)
    {
        Dispatcher.UIThread.Post(() =>
        {
            if (_previewBitmap is null)
                return;
            using var framebuffer = _previewBitmap.Lock();
            Marshal.Copy(frame, 0, framebuffer.Address, Math.Min(frame.Length, framebuffer.RowBytes * framebuffer.Size.Height));
            PreviewImage.InvalidateVisual();
            _hasFrame = true;
            _connected = true;
            _lastFrameAt = DateTime.UtcNow;
            PreviewImage.IsVisible = true;
            DisconnectedPanel.IsVisible = false;
            StatusText.Text = _recording ? "Recording in progress" : "Live preview";
        }, DispatcherPriority.Render);
    }

    private async void Record_Click(object? sender, RoutedEventArgs e)
    {
        try
        {
            if (_recording)
            {
                RecordButton.IsEnabled = false;
                StatusText.Text = "Finalizing recording...";
                await Task.Run(_media.StopRecording);
                _recording = false;
                RecordButton.Content = "Start recording";
                RecordButton.Classes.Remove("recording");
                StatusText.Text = _hasFrame ? "Live preview" : "Waiting for video";
                return;
            }

            _settings = ReadSettings();
            _settings.OutputFolder = Required(_settings.OutputFolder, "Recording folder");
            MediaReceiver.ValidateContainer(_settings.Codec, _settings.Container);
            Directory.CreateDirectory(_settings.OutputFolder);
            var output = Path.Combine(
                _settings.OutputFolder,
                $"deck_{DateTime.Now:yyyy-MM-dd_HH-mm-ss}.{_settings.Container}");
            _recording = true;
            _media.StartRecording(new RecordingSettings(
                FindFfmpeg(), StreamPort, _settings.LatencyMs, _settings.Codec,
                _settings.Container, _settings.AudioBitrateKbps, output));
            await Task.Delay(150);
            if (!_media.IsRecording)
                throw new InvalidOperationException("The recording receiver could not start.");
            if (_settings.Codec == "av1")
            {
                await _agent.StartAsync(
                    _settings.DeckHost, _settings.DeckPort, BuildStreamSettings(_settings));
                _activeConfig = ConfigKey(_settings);
            }
            RecordButton.Content = "Stop recording";
            RecordButton.Classes.Add("recording");
            StatusText.Text = $"Recording: {Path.GetFileName(output)}";
        }
        catch (Exception error)
        {
            _media.StopRecording();
            _recording = false;
            RecordButton.Content = "Start recording";
            RecordButton.Classes.Remove("recording");
            StatusText.Text = error.Message;
        }
        finally
        {
            RecordButton.IsEnabled = true;
        }
    }

    private void Mute_Click(object? sender, RoutedEventArgs e)
    {
        _settings.PreviewMuted = !_settings.PreviewMuted;
        MuteButton.Content = _settings.PreviewMuted ? "Unmute" : "Mute";
        _media.SetVolume(_settings.PreviewVolume, _settings.PreviewMuted);
        SaveSettings();
    }

    private void VolumeSlider_ValueChanged(object? sender, Avalonia.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        _settings.PreviewVolume = e.NewValue;
        _media.SetVolume(_settings.PreviewVolume, _settings.PreviewMuted);
        ScheduleSave();
    }

    private async void Browse_Click(object? sender, RoutedEventArgs e)
    {
        var folders = await StorageProvider.OpenFolderPickerAsync(new()
        {
            Title = "Recording folder",
            AllowMultiple = false
        });
        if (folders.Count > 0)
        {
            OutputFolderBox.Text = folders[0].Path.LocalPath;
            SaveSettings();
        }
    }

    private void TitleBar_PointerPressed(object? sender, PointerPressedEventArgs e)
    {
        if (e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)
            BeginMoveDrag(e);
    }

    private void Minimize_Click(object? sender, RoutedEventArgs e) =>
        WindowState = WindowState.Minimized;

    private void Close_Click(object? sender, RoutedEventArgs e) => Close();

    private async void Window_Closing(object? sender, WindowClosingEventArgs e)
    {
        if (_closing)
            return;
        e.Cancel = true;
        _closing = true;
        _connectionTimer.Stop();
        SaveSettings();
        await Task.Run(_media.StopRecording);
        _media.StopPreview();
        await _agent.StopAsync();
        Close();
    }

    private void SetDisconnected()
    {
        _connected = false;
        _hasFrame = false;
        PreviewImage.IsVisible = false;
        DisconnectedPanel.IsVisible = true;
        ConnectionHeading.Text = "No connection";
        ConnectionDetail.Text = "Waiting for the SteamOS agent";
        StatusText.Text = "Waiting for the SteamOS agent";
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
        OutputFolder = OutputFolderBox.Text?.Trim() ?? "",
        PreviewVolume = VolumeSlider.Value,
        PreviewMuted = _settings.PreviewMuted
    };

    private static StreamSettings BuildStreamSettings(AppSettings settings) => new(
        settings.ReceiverHost, StreamPort, settings.Codec, settings.Backend,
        settings.CaptureMode, "vaapi", settings.Fps, settings.VideoBitrateKbps,
        settings.AudioBitrateKbps, settings.LatencyMs, settings.Size, settings.Display,
        settings.AudioSource, settings.DrmDevice, settings.VaapiDevice);

    private static string ConfigKey(AppSettings settings) => string.Join('|',
        settings.ReceiverHost, settings.Codec, settings.Backend, settings.VideoBitrateKbps,
        settings.AudioBitrateKbps, settings.Fps, settings.LatencyMs, settings.CaptureMode,
        settings.Size, settings.Display, settings.AudioSource, settings.DrmDevice,
        settings.VaapiDevice);

    private void ApplySettings(AppSettings settings)
    {
        _settings = settings;
        DeckHostBox.Text = settings.DeckHost;
        DeckPortBox.Text = settings.DeckPort.ToString();
        ReceiverHostBox.Text = string.IsNullOrWhiteSpace(settings.ReceiverHost) ? GetLocalIp() : settings.ReceiverHost;
        CodecBox.SelectedItem = settings.Codec;
        BackendBox.SelectedItem = settings.Backend;
        VideoBitrateBox.Text = settings.VideoBitrateKbps.ToString();
        AudioBitrateBox.Text = settings.AudioBitrateKbps.ToString();
        FpsBox.Text = settings.Fps.ToString();
        LatencyBox.Text = settings.LatencyMs.ToString();
        CaptureModeBox.SelectedItem = settings.CaptureMode;
        ContainerBox.SelectedItem = settings.Container;
        SizeBox.Text = settings.Size;
        DisplayBox.Text = settings.Display;
        AudioSourceBox.Text = settings.AudioSource;
        DrmDeviceBox.Text = settings.DrmDevice;
        VaapiDeviceBox.Text = settings.VaapiDevice;
        OutputFolderBox.Text = string.IsNullOrWhiteSpace(settings.OutputFolder)
            ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos), "DeckScreenShare")
            : settings.OutputFolder;
        VolumeSlider.Value = settings.PreviewVolume;
        MuteButton.Content = settings.PreviewMuted ? "Unmute" : "Mute";
    }

    private void ScheduleSave()
    {
        _saveTimer?.Stop();
        _saveTimer?.Start();
    }

    private void SaveSettings()
    {
        try
        {
            _settings = ReadSettings();
            _settingsStore.Save(_settings);
        }
        catch { }
    }

    private static string Selected(ComboBox box) =>
        box.SelectedItem?.ToString() ?? throw new InvalidOperationException("Select a value.");

    private static string Required(string? text, string name) =>
        string.IsNullOrWhiteSpace(text)
            ? throw new InvalidOperationException($"{name} is required.")
            : text.Trim();

    private static int PositiveInt(string? text, string name) =>
        int.TryParse(text, out var value) && value > 0
            ? value
            : throw new InvalidOperationException($"{name} must be a positive number.");

    private static (int Width, int Height) ParseSize(string size)
    {
        var parts = size.Split('x');
        return parts.Length == 2 && int.TryParse(parts[0], out var width) &&
               int.TryParse(parts[1], out var height) && width > 0 && height > 0
            ? (width, height)
            : (1280, 800);
    }

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
        catch { return "127.0.0.1"; }
    }
}
