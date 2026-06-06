using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Windows;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Microsoft.Win32;

namespace DeckScreenShare.Receiver;

public partial class MainWindow : Window
{
    private const int SrtPort = 9000;
    private readonly AgentClient _agent = new();
    private readonly FfmpegReceiver _receiver = new();
    private readonly DispatcherTimer _previewTimer;
    private string _previewPath = "";
    private string _lastLog = "";

    public MainWindow()
    {
        InitializeComponent();
        ReceiverHostBox.Text = GetLocalIp();
        OutputFolderBox.Text = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.MyVideos), "DeckScreenShare");
        _receiver.Log += message => _lastLog = message;
        _previewTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(120) };
        _previewTimer.Tick += (_, _) => RefreshPreview();
    }

    private static string Selected(System.Windows.Controls.ComboBox box) =>
        ((System.Windows.Controls.ComboBoxItem)box.SelectedItem).Content.ToString()!;

    private async void Start_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            SetBusy(true);
            var codec = Selected(CodecBox);
            var container = Selected(ContainerBox);
            FfmpegReceiver.ValidateContainer(codec, container);
            var folder = OutputFolderBox.Text.Trim();
            Directory.CreateDirectory(folder);
            _previewPath = Path.Combine(folder, ".deck-preview.jpg");
            TryDelete(_previewPath);
            var output = Path.Combine(folder, $"deck_{DateTime.Now:yyyy-MM-dd_HH-mm-ss}.{container}");
            var audioBitrate = PositiveInt(AudioBitrateBox.Text, "Audio bitrate");
            var stream = new StreamSettings(
                ReceiverHostBox.Text.Trim(), SrtPort, codec, Selected(BackendBox), Selected(CaptureModeBox),
                PositiveInt(FpsBox.Text, "FPS"), PositiveInt(VideoBitrateBox.Text, "Video bitrate"),
                audioBitrate, PositiveInt(LatencyBox.Text, "Latency"), SizeBox.Text.Trim(),
                DisplayBox.Text.Trim(), AudioSourceBox.Text.Trim());
            var recording = new RecordingSettings(
                FindFfmpeg(), SrtPort, stream.LatencyMs, codec, container, audioBitrate, output, _previewPath);

            _receiver.Start(recording);
            await Task.Delay(400);
            await _agent.StartAsync(DeckHostBox.Text.Trim(), PositiveInt(DeckPortBox.Text, "Agent port"), stream);
            _previewTimer.Start();
            StartButton.IsEnabled = false;
            StopButton.IsEnabled = true;
            StatusText.Text = $"Recording: {output}";
        }
        catch (Exception ex)
        {
            await _agent.StopAsync();
            _receiver.Stop();
            MessageBox.Show(ex.Message, "Could not start recording", MessageBoxButton.OK, MessageBoxImage.Error);
            StatusText.Text = string.IsNullOrWhiteSpace(_lastLog) ? ex.Message : _lastLog;
        }
        finally
        {
            if (!StopButton.IsEnabled)
                SetBusy(false);
        }
    }

    private async void Stop_Click(object sender, RoutedEventArgs e) => await StopAsync();

    private async Task StopAsync()
    {
        SetBusy(true);
        _previewTimer.Stop();
        await _agent.StopAsync();
        _receiver.Stop();
        StartButton.IsEnabled = true;
        StopButton.IsEnabled = false;
        PreviewHint.Visibility = Visibility.Visible;
        StatusText.Text = "Recording stopped and saved.";
        SetBusy(false);
    }

    private void RefreshPreview()
    {
        if (!File.Exists(_previewPath))
            return;
        try
        {
            using var stream = new FileStream(_previewPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            var image = new BitmapImage();
            image.BeginInit();
            image.CacheOption = BitmapCacheOption.OnLoad;
            image.StreamSource = stream;
            image.EndInit();
            image.Freeze();
            PreviewImage.Source = image;
            PreviewHint.Visibility = Visibility.Collapsed;
        }
        catch (IOException) { }
        catch (NotSupportedException) { }
    }

    private void Browse_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "Recording folder", Multiselect = false };
        if (dialog.ShowDialog() == true)
            OutputFolderBox.Text = dialog.FolderName;
    }

    private static int PositiveInt(string text, string name) =>
        int.TryParse(text, out var value) && value > 0 ? value : throw new InvalidOperationException($"{name} must be a positive number.");

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

    private void SetBusy(bool busy)
    {
        if (busy)
            StatusText.Text = "Working...";
        StartButton.IsEnabled = !busy && !StopButton.IsEnabled;
        StopButton.IsEnabled = !busy && _receiver.IsRunning;
    }

    private static void TryDelete(string path)
    {
        try { File.Delete(path); } catch (IOException) { }
    }

    private async void Window_Closing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        if (!_receiver.IsRunning)
            return;
        e.Cancel = true;
        await StopAsync();
        Close();
    }
}
