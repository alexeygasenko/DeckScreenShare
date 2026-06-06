using System.Net.Http;
using System.Text;
using System.Text.Json;

namespace DeckScreenShare.Receiver;

public sealed class AgentClient : IDisposable
{
    private readonly HttpClient _http = new() { Timeout = TimeSpan.FromSeconds(8) };
    private string? _baseUrl;

    public async Task CheckAsync(string host, int port)
    {
        var baseUrl = $"http://{host}:{port}";
        using var response = await _http.GetAsync($"{baseUrl}/api/status");
        await EnsureSuccessAsync(response);
        _baseUrl = baseUrl;
    }

    public async Task StartAsync(string host, int port, StreamSettings settings)
    {
        var baseUrl = $"http://{host}:{port}";
        using var response = await PostJsonAsync($"{baseUrl}/api/start", settings);
        await EnsureSuccessAsync(response);
        _baseUrl = baseUrl;
    }

    public async Task StopAsync()
    {
        if (_baseUrl is null)
            return;
        try
        {
            using var response = await PostJsonAsync($"{_baseUrl}/api/stop", new { });
        }
        catch
        {
            // Local recording still has to be finalized when the Deck is unreachable.
        }
        finally
        {
            _baseUrl = null;
        }
    }

    private async Task<HttpResponseMessage> PostJsonAsync<T>(string url, T value)
    {
        var payload = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(value));
        var content = new ByteArrayContent(payload);
        content.Headers.ContentType = new("application/json") { CharSet = "utf-8" };
        return await _http.PostAsync(url, content);
    }

    private static async Task EnsureSuccessAsync(HttpResponseMessage response)
    {
        if (response.IsSuccessStatusCode)
            return;

        var details = (await response.Content.ReadAsStringAsync()).Trim();
        try
        {
            using var document = JsonDocument.Parse(details);
            if (document.RootElement.TryGetProperty("error", out var error))
                details = error.GetString() ?? details;
        }
        catch (JsonException)
        {
            // Preserve non-JSON responses from proxies and network middleware.
        }
        throw new HttpRequestException(
            string.IsNullOrWhiteSpace(details)
                ? $"SteamOS agent returned {(int)response.StatusCode} {response.ReasonPhrase}."
                : $"SteamOS agent returned {(int)response.StatusCode}: {details}");
    }

    public void Dispose() => _http.Dispose();
}
