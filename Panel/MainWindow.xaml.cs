using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Net.Http.Json;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace SafeOps.Panel;

public sealed class BotInfo : INotifyPropertyChanged
{
    private string _status = "offline";

    [JsonPropertyName("id")]
    public string Id { get; set; } = string.Empty;

    [JsonPropertyName("hostname")]
    public string Hostname { get; set; } = string.Empty;

    [JsonPropertyName("lastSeen")]
    public DateTime LastSeen { get; set; }

    public string LastSeenLocal => LastSeen == default
        ? "never"
        : LastSeen.ToLocalTime().ToString("yyyy-MM-dd HH:mm:ss");

    public string Status
    {
        get => _status;
        set
        {
            if (_status == value)
            {
                return;
            }

            _status = value;
            OnPropertyChanged();
        }
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}

public sealed class ResponseEntry
{
    [JsonPropertyName("time")]
    public DateTime Time { get; set; } = DateTime.Now;

    [JsonPropertyName("source")]
    public string Source { get; set; } = "panel";

    [JsonPropertyName("message")]
    public string Message { get; set; } = string.Empty;

    public string TimeLocal => Time.ToLocalTime().ToString("yyyy-MM-dd HH:mm:ss");
}

public sealed class PanelSettings
{
    public string ServerAddress { get; set; } = "http://localhost:12346";
    public bool AutoRefresh { get; set; } = true;
    public int RefreshIntervalSeconds { get; set; } = 2;
}

public partial class MainWindow : Window, INotifyPropertyChanged
{
    private static readonly string[] SafeDiagnosticActions = ["ping", "health", "version", "metrics"];
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true
    };

    private readonly ObservableCollection<BotInfo> _clients = [];
    private readonly ObservableCollection<ResponseEntry> _responses = [];
    private readonly string _settingsPath;
    private readonly Timer _refreshTimer;
    private HttpClient _httpClient = new();
    private PanelSettings _settings = new();
    private bool _isRefreshing;
    private string _statusText = "Ожидание";
    private string _footerText = "Готово";
    private string _lastRefreshText = "еще не обновлялось";
    private Brush _connectionBrush = Brushes.Gray;

    public MainWindow()
    {
        InitializeComponent();
        DataContext = this;

        _settingsPath = Path.Combine(AppContext.BaseDirectory, "panel_settings.json");
        ClientsGrid.ItemsSource = _clients;
        LogsGrid.ItemsSource = _responses;
        _refreshTimer = new Timer(async _ => await RefreshDataAsync(), null, Timeout.Infinite, Timeout.Infinite);
    }

    public string StatusText
    {
        get => _statusText;
        private set => SetField(ref _statusText, value);
    }

    public string FooterText
    {
        get => _footerText;
        private set => SetField(ref _footerText, value);
    }

    public string LastRefreshText
    {
        get => _lastRefreshText;
        private set => SetField(ref _lastRefreshText, value);
    }

    public Brush ConnectionBrush
    {
        get => _connectionBrush;
        private set => SetField(ref _connectionBrush, value);
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private async void Window_Loaded(object sender, RoutedEventArgs e)
    {
        await LoadSettingsAsync();
        ApplySettingsToControls();

        if (!TryRecreateHttpClient())
        {
            return;
        }

        ScheduleRefresh();
        await RefreshDataAsync();
    }

    private async Task LoadSettingsAsync()
    {
        try
        {
            if (!File.Exists(_settingsPath))
            {
                return;
            }

            await using var stream = File.OpenRead(_settingsPath);
            _settings = await JsonSerializer.DeserializeAsync<PanelSettings>(stream, JsonOptions) ?? new PanelSettings();
            _settings.ServerAddress = NormalizeServerAddress(_settings.ServerAddress);
            _settings.RefreshIntervalSeconds = Math.Clamp(_settings.RefreshIntervalSeconds, 1, 3600);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        {
            AddLocalLog("settings", $"Не удалось прочитать настройки: {ex.Message}");
        }
    }

    private async Task SaveSettingsAsync()
    {
        _settings = ReadSettingsFromControls();
        await using var stream = File.Create(_settingsPath);
        await JsonSerializer.SerializeAsync(stream, _settings, JsonOptions);
        AddLocalLog("settings", "Настройки сохранены");
    }

    private PanelSettings ReadSettingsFromControls()
    {
        if (!int.TryParse(RefreshIntervalTextBox?.Text?.Trim(), out var interval))
        {
            interval = 2;
        }

        return new PanelSettings
        {
            ServerAddress = NormalizeServerAddress(ServerAddressTextBox?.Text ?? _settings.ServerAddress),
            AutoRefresh = AutoRefreshCheckBox?.IsChecked ?? _settings.AutoRefresh,
            RefreshIntervalSeconds = Math.Clamp(interval, 1, 3600)
        };
    }

    private static string NormalizeServerAddress(string? address)
    {
        address = string.IsNullOrWhiteSpace(address)
            ? "http://localhost:12346"
            : address.Trim();

        return address.TrimEnd('/');
    }

    private void ApplySettingsToControls()
    {
        ServerAddressTextBox.Text = _settings.ServerAddress;
        AutoRefreshCheckBox.IsChecked = _settings.AutoRefresh;
        RefreshIntervalTextBox.Text = _settings.RefreshIntervalSeconds.ToString();
    }

    private bool TryRecreateHttpClient()
    {
        var baseAddress = NormalizeServerAddress(_settings.ServerAddress);
        if (!Uri.TryCreate(baseAddress + "/", UriKind.Absolute, out var uri) ||
            (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps))
        {
            StatusText = "Ошибка";
            FooterText = "Адрес API должен быть абсолютным HTTP/HTTPS URL.";
            ConnectionBrush = Brushes.OrangeRed;
            AddLocalLog("settings", FooterText);
            return false;
        }

        _httpClient.Dispose();
        _httpClient = new HttpClient
        {
            BaseAddress = uri,
            Timeout = TimeSpan.FromSeconds(8)
        };

        return true;
    }

    private void ScheduleRefresh()
    {
        var dueTime = _settings.AutoRefresh
            ? TimeSpan.FromSeconds(_settings.RefreshIntervalSeconds)
            : Timeout.InfiniteTimeSpan;
        var period = _settings.AutoRefresh
            ? TimeSpan.FromSeconds(_settings.RefreshIntervalSeconds)
            : Timeout.InfiniteTimeSpan;

        _refreshTimer.Change(dueTime, period);
    }

    private async Task RefreshDataAsync()
    {
        if (_isRefreshing)
        {
            return;
        }

        _isRefreshing = true;
        await Dispatcher.InvokeAsync(() =>
        {
            StatusText = "Обновление";
            FooterText = "Запрашиваю список узлов и ответы безопасного API...";
            ConnectionBrush = Brushes.Goldenrod;
        });

        try
        {
            await RefreshClientsAsync();
            await RefreshResponsesAsync();
            await Dispatcher.InvokeAsync(() =>
            {
                LastRefreshText = $"обновлено {DateTime.Now:HH:mm:ss}";
                StatusText = "Онлайн";
                FooterText = $"Узлов: {_clients.Count}. Последнее обновление прошло без ошибок.";
                ConnectionBrush = Brushes.LimeGreen;
            });
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or JsonException or InvalidOperationException)
        {
            await Dispatcher.InvokeAsync(() =>
            {
                StatusText = "Нет связи";
                FooterText = ex is TaskCanceledException
                    ? "Сервер не ответил за отведенное время."
                    : $"Ошибка подключения: {ex.Message}";
                ConnectionBrush = Brushes.OrangeRed;
                AddLocalLog("network", FooterText);
            });
        }
        finally
        {
            _isRefreshing = false;
        }
    }

    private async Task RefreshClientsAsync()
    {
        using var response = await _httpClient.GetAsync("clients");
        response.EnsureSuccessStatusCode();

        var clients = await response.Content.ReadFromJsonAsync<BotInfo[]>(JsonOptions) ?? [];
        await Dispatcher.InvokeAsync(() =>
        {
            _clients.Clear();
            foreach (var client in clients.OrderBy(c => c.Hostname))
            {
                client.Status = GetClientStatus(client.LastSeen);
                _clients.Add(client);
            }
        });
    }

    private async Task RefreshResponsesAsync()
    {
        using var response = await _httpClient.GetAsync("responses");
        if (!response.IsSuccessStatusCode)
        {
            return;
        }

        var entries = await response.Content.ReadFromJsonAsync<ResponseEntry[]>(JsonOptions) ?? [];
        await Dispatcher.InvokeAsync(() =>
        {
            foreach (var entry in entries.OrderBy(e => e.Time))
            {
                if (_responses.Any(existing =>
                        existing.Time == entry.Time &&
                        existing.Source == entry.Source &&
                        existing.Message == entry.Message))
                {
                    continue;
                }

                _responses.Insert(0, entry);
            }
        });
    }

    private static string GetClientStatus(DateTime lastSeen)
    {
        if (lastSeen == default)
        {
            return "offline";
        }

        return DateTime.UtcNow - lastSeen.ToUniversalTime() <= TimeSpan.FromSeconds(10)
            ? "online"
            : "offline";
    }

    private async Task SendDiagnosticAsync()
    {
        if (ClientsGrid.SelectedItem is not BotInfo selected)
        {
            MessageBox.Show("Выберите узел из списка.", "SafeOps", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        if (DiagnosticActionBox.SelectedItem is not ComboBoxItem item || item.Content is not string action)
        {
            MessageBox.Show("Выберите диагностическое действие.", "SafeOps", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        if (!SafeDiagnosticActions.Contains(action, StringComparer.OrdinalIgnoreCase))
        {
            MessageBox.Show("Доступны только безопасные диагностические действия.", "SafeOps", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        var payload = new
        {
            client_id = selected.Id,
            action,
            note = CommandTextBox.Text.Trim()
        };

        try
        {
            using var response = await _httpClient.PostAsJsonAsync("diagnostics", payload, JsonOptions);
            response.EnsureSuccessStatusCode();
            AddLocalLog(selected.Hostname, $"Диагностика отправлена: {action}");
            CommandTextBox.Clear();
            await RefreshResponsesAsync();
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or InvalidOperationException)
        {
            AddLocalLog("network", $"Не удалось отправить диагностику: {ex.Message}");
            FooterText = $"Не удалось отправить диагностику: {ex.Message}";
            StatusText = "Ошибка";
            ConnectionBrush = Brushes.OrangeRed;
        }
    }

    private void AddLocalLog(string source, string message)
    {
        var entry = new ResponseEntry
        {
            Time = DateTime.Now,
            Source = source,
            Message = message
        };

        _responses.Insert(0, entry);
    }

    private async void RefreshButton_Click(object sender, RoutedEventArgs e)
    {
        await RefreshDataAsync();
    }

    private async void SendButton_Click(object sender, RoutedEventArgs e)
    {
        await SendDiagnosticAsync();
    }

    private void ClearLogsButton_Click(object sender, RoutedEventArgs e)
    {
        _responses.Clear();
        AddLocalLog("panel", "Логи очищены");
    }

    private async void SaveSettingsButton_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            await SaveSettingsAsync();
            if (TryRecreateHttpClient())
            {
                ScheduleRefresh();
                FooterText = "Настройки сохранены и применены.";
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            MessageBox.Show($"Не удалось сохранить настройки: {ex.Message}", "SafeOps", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private async void TestConnectionButton_Click(object sender, RoutedEventArgs e)
    {
        _settings = ReadSettingsFromControls();
        if (!TryRecreateHttpClient())
        {
            return;
        }

        try
        {
            using var response = await _httpClient.GetAsync("clients");
            FooterText = response.IsSuccessStatusCode
                ? "Связь проверена: API отвечает."
                : $"API ответил статусом {(int)response.StatusCode}.";
            StatusText = response.IsSuccessStatusCode ? "Онлайн" : "Ошибка";
            ConnectionBrush = response.IsSuccessStatusCode ? Brushes.LimeGreen : Brushes.OrangeRed;
            AddLocalLog("network", FooterText);
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or InvalidOperationException)
        {
            FooterText = $"Проверка связи не прошла: {ex.Message}";
            StatusText = "Нет связи";
            ConnectionBrush = Brushes.OrangeRed;
            AddLocalLog("network", FooterText);
        }
    }

    private void AutoRefreshCheckBox_Changed(object sender, RoutedEventArgs e)
    {
        _settings = ReadSettingsFromControls();
        ScheduleRefresh();
    }

    private void DecreaseIntervalButton_Click(object sender, RoutedEventArgs e)
    {
        RefreshIntervalTextBox.Text = AdjustInterval(-1).ToString();
    }

    private void IncreaseIntervalButton_Click(object sender, RoutedEventArgs e)
    {
        RefreshIntervalTextBox.Text = AdjustInterval(1).ToString();
    }

    private int AdjustInterval(int delta)
    {
        if (!int.TryParse(RefreshIntervalTextBox.Text.Trim(), out var current))
        {
            current = 2;
        }

        return Math.Clamp(current + delta, 1, 3600);
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        _refreshTimer.Dispose();
        _httpClient.Dispose();
        base.OnClosing(e);
    }

    private bool SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (Equals(field, value))
        {
            return false;
        }

        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        return true;
    }
}
