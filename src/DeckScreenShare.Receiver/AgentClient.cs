using System.Net.Http;
using System.Net.Http.Json;

namespace DeckScreenShare.Receiver;

public sealed class AgentClient : IDisposable
{
    private readonly HttpClient _http = new() { Timeout = TimeSpan.FromSeconds(8) };
    private string? _baseUrl;

    public async Task StartAsync(string host, int port, StreamSettings settings)
    {
        _baseUrl = $"http://{host}:{port}";
        using var response = await _http.PostAsJsonAsync($"{_baseUrl}/api/start", settings);
        response.EnsureSuccessStatusCode();
    }

    public async Task StopAsync()
    {
        if (_baseUrl is null)
            return;
        try
        {
            using var response = await _http.PostAsJsonAsync($"{_baseUrl}/api/stop", new { });
        }
        catch
        {
            // Local recording still has to be finalized when the Deck is unreachable.
        }
    }

    public void Dispose() => _http.Dispose();
}
