// SPDX-License-Identifier: MIT
using System.Diagnostics;
using System.IO;
using System.Security.Principal;
using System.Windows;
using System.Windows.Threading;
using Forms = System.Windows.Forms;

namespace Winawdl.Xiatiao;

internal static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Contains("--xiatiao-radio-host")) return RadioHost.Run(Array.Empty<string>());
        if (args.Contains("--xiatiao-host-check")) return RadioHost.Check(Array.Empty<string>());
        if (args.Contains("--xiatiao-check")) return AppChecks.Run(args);
        if (Array.IndexOf(args, "--xiatiao-language-check") is var languageIndex && languageIndex >= 0 && languageIndex + 1 < args.Length) return LanguageChecks.Run(args[languageIndex + 1]);
        if (Array.IndexOf(args, "--xiatiao-ui-check") is var uiIndex && uiIndex >= 0 && uiIndex + 1 < args.Length) return UiChecks.Run(args[uiIndex + 1]);
        if (args.Contains("--xiatiao-render")) return AppChecks.Render(args);
        // The task elevates only the radio host. Never start a network receiver as admin.
        if (new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator)) return 8;
        Directory.CreateDirectory(Store.Root);
        var sid = WindowsIdentity.GetCurrent().User!.Value;
        using var mutex = new Mutex(true, "Local\\XiatiaoAirDrop.App." + sid, out var first);
        using var wake = new EventWaitHandle(false, EventResetMode.AutoReset, "Local\\XiatiaoAirDrop.Show." + sid);
        using var stop = new EventWaitHandle(false, EventResetMode.AutoReset, "Local\\XiatiaoAirDrop.Stop." + sid);
        if (args.Contains("--shutdown")) { if (!first) stop.Set(); return 0; }
        if (!first) { if (!args.Contains("--hidden")) wake.Set(); return 0; }
        var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
        var smoke = args.Contains("--tray-check");
        using var model = new AppController(preview: smoke);
        var window = new XiatiaoWindow(model);
        window.Width = Math.Min(window.Width, SystemParameters.WorkArea.Width);
        window.Height = Math.Min(window.Height, SystemParameters.WorkArea.Height);
        window.MinHeight = Math.Min(window.MinHeight, SystemParameters.WorkArea.Height);
        window.MinWidth = Math.Min(window.MinWidth, SystemParameters.WorkArea.Width);
        app.MainWindow = window;
        using var icon = new System.Drawing.Icon(Path.Combine(AppContext.BaseDirectory, "assets", "xiatiao-airdrop.ico"));
        using var tray = new Forms.NotifyIcon { Icon = icon, Text = "Airdrop for Windows", Visible = true };
        var closing = false;
        void Show() { window.Show(); window.WindowState = WindowState.Normal; window.Activate(); }
        var menu = new Forms.ContextMenuStrip();
        var open = menu.Items.Add(L.T("打开 Airdrop for Windows"), null, (_, _) => Show());
        var toggle = new Forms.ToolStripMenuItem(L.T("开启 AirDrop"));
        toggle.Click += (_, _) => model.IsEnabled = !model.IsEnabled; menu.Items.Add(toggle);
        var folder = menu.Items.Add(L.T("打开储存位置"), null, (_, _) => model.OpenFolderCommand.Execute(null));
        var about = menu.Items.Add(L.T("关于 Airdrop for Windows"), null, (_, _) => window.ShowAbout());
        menu.Items.Add(new Forms.ToolStripSeparator());
        var quit = new Forms.ToolStripMenuItem(L.T("退出")); menu.Items.Add(quit);
        void UpdateTrayLanguage(object? sender = null, EventArgs? args = null)
        {
            open.Text = L.T("打开 Airdrop for Windows"); folder.Text = L.T("打开储存位置");
            about.Text = L.T("关于 Airdrop for Windows"); quit.Text = L.T("退出");
            toggle.Text = L.T(model.IsEnabled ? "关闭 AirDrop" : "开启 AirDrop");
            var status = "Airdrop for Windows · " + model.StatusText;
            tray.Text = status[..Math.Min(63, status.Length)];
        }
        L.Changed += UpdateTrayLanguage; UpdateTrayLanguage();
        async void Exit()
        {
            if (closing) return; closing = true; quit.Enabled = false;
            try { await model.ShutdownAsync(); tray.Visible = false; app.Shutdown(); }
            catch (Exception error)
            {
                closing = false; quit.Enabled = true;
                tray.ShowBalloonTip(8000, L.T("无线连接还未确认关闭"), L.F("{0}。请稍后再退出。", L.Error(error.Message)), Forms.ToolTipIcon.Warning);
            }
        }
        quit.Click += (_, _) => Exit();
        tray.ContextMenuStrip = menu;
        tray.MouseClick += (_, e) => { if (e.Button == Forms.MouseButtons.Left) Show(); };
        tray.BalloonTipClicked += (_, _) => Show();
        model.Notification += (title, text) => tray.ShowBalloonTip(10000, title, text, Forms.ToolTipIcon.Info);
        model.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(model.IsEnabled) or nameof(model.IsBusy)) { toggle.Text = L.T(model.IsEnabled ? "关闭 AirDrop" : "开启 AirDrop"); toggle.Enabled = model.IsEnabled || !model.IsBusy; }
            if (e.PropertyName == nameof(model.StatusText)) tray.Text = ("Airdrop for Windows · " + model.StatusText)[..Math.Min(63, "Airdrop for Windows · ".Length + model.StatusText.Length)];
        };
        window.Closing += (_, e) => { if (!closing) { e.Cancel = true; window.Hide(); } };
        app.SessionEnding += (_, _) => model.Dispose();
        var registered = ThreadPool.RegisterWaitForSingleObject(wake, (_, _) => app.Dispatcher.BeginInvoke((Action)Show), null, -1, false);
        var stopRegistered = ThreadPool.RegisterWaitForSingleObject(stop, (_, _) => app.Dispatcher.BeginInvoke((Action)Exit), null, -1, false);
        app.DispatcherUnhandledException += (_, e) =>
        {
            try { File.AppendAllText(Path.Combine(Store.Root, "application.log"), DateTimeOffset.Now + " " + e.Exception + Environment.NewLine); } catch (IOException) { }
            tray.ShowBalloonTip(8000, L.T("操作未完成"), L.Error(e.Exception.Message), Forms.ToolTipIcon.Warning); e.Handled = true;
        };
        DispatcherTimer? checkTimer = null;
        app.Startup += (_, _) =>
        {
            if (smoke)
            {
                checkTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
                checkTimer.Tick += (_, _) =>
                {
                    checkTimer.Stop();
                    var index = Array.IndexOf(args, "--tray-check");
                    if (index + 1 < args.Length) Store.Write(Path.Combine(args[index + 1], "tray-check.json"), new { hiddenWindow = !window.IsVisible, trayConfigured = tray.Visible, iconLoaded = icon.Width > 0, radioStarted = false, processId = Environment.ProcessId });
                    Exit();
                };
                checkTimer.Start();
            }
            else { if (!args.Contains("--hidden")) Show(); model.StartSaved(); }
        };
        try { app.Run(); }
        finally { L.Changed -= UpdateTrayLanguage; registered.Unregister(null); stopRegistered.Unregister(null); tray.Visible = false; }
        return 0;
    }
}
