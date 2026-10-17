// SPDX-License-Identifier: MIT
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Windows.Input;
using System.Windows.Threading;
using Microsoft.Win32;

namespace Winawdl.Xiatiao;

public sealed class AppController : Observable, IDisposable
{
    private readonly Dispatcher _ui;
    private readonly string _data;
    private readonly bool _preview;
    private readonly Preferences _settings;
    private readonly RadioClient _radio = new();
    private ReceiverProcess? _receiver;
    private readonly HashSet<string> _receiveFolders = new(StringComparer.OrdinalIgnoreCase);
    private bool _enabled, _busy, _disposed, _faultCleanup, _detailIsLiteral;
    private string _name, _status = "AirDrop 已关闭", _detail = "打开后，附近的 Apple 设备可以向你发送文件。", _error = "";
    private object[]? _errorArguments;
    private TaskCompletionSource? _radioStopped;
    public event Action<string, string>? Notification;
    public ObservableCollection<ReceivedItem> Files { get; } = new();
    public ObservableCollection<TransferItem> Pending { get; } = new();
    public SenderController Sender { get; }
    public string DeviceName { get => _name; set { _name = value; Changed(); } }
    public string SaveFolder => _settings.Folder;
    public LanguageOption[] LanguageOptions => L.Options;
    public string Language
    {
        get => _settings.Language;
        set
        {
            var next = L.Normalize(value);
            if (_settings.Language == next) return;
            var previous = _settings.Language;
            try { _settings.Language = next; Save(); L.SetLanguage(next); Changed(); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            { _settings.Language = previous; Error("语言设置未保存：" + error.Message); Changed(); }
        }
    }
    public bool AutoStart
    {
        get => _settings.AutoStart;
        set
        {
            if (_settings.AutoStart == value) return;
            try
            {
                if (!_preview)
                {
                    using var run = Registry.CurrentUser.CreateSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run");
                    if (value) run.SetValue("XiatiaoAirDrop", "\"" + Path.Combine(AppContext.BaseDirectory, "XiatiaoAirDrop.exe") + "\" --hidden");
                    else run.DeleteValue("XiatiaoAirDrop", false);
                }
                _settings.AutoStart = value; Save(); Changed();
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException) { Error("自启动设置未保存：" + e.Message); Changed(); }
        }
    }
    public bool IsEnabled { get => _enabled; set { if ((!_busy || !value) && value != _enabled) _ = SetEnabledAsync(value); } }
    public bool IsBusy => _busy;
    public string StatusText => L.T(_status);
    public string StatusDetail => _detailIsLiteral ? _detail : L.T(_detail);
    public string ErrorText => _errorArguments is null ? L.Error(_error) :
        L.F(_error, _errorArguments.Select(argument => argument is Exception error ? L.Error(error.Message) : argument).ToArray());
    public bool HasError => !string.IsNullOrEmpty(_error);
    public string FileCountText => L.F("{0} 个文件", Files.Count);
    public ICommand SaveNameCommand { get; }
    public ICommand BrowseFolderCommand { get; }
    public ICommand OpenFolderCommand { get; }
    public ICommand OpenFileCommand { get; }
    public ICommand ShowFileCommand { get; }
    public ICommand AcceptCommand { get; }
    public ICommand DeclineCommand { get; }
    public ICommand ClearHistoryCommand { get; }
    public ICommand OpenNoticesCommand { get; }

    public AppController(bool preview = false, string? dataRoot = null)
    {
        _ui = Dispatcher.CurrentDispatcher; _preview = preview; _data = dataRoot ?? Store.Root;
        _settings = preview ? new() : Store.Read(Path.Combine(_data, "settings.json"), new Preferences());
        _settings.Language = L.Normalize(_settings.Language);
        if (!preview) L.SetLanguage(_settings.Language);
        else _settings.Language = L.Language;
        _name = _settings.Name;
        Sender = new SenderController(command =>
        {
            if (_preview) throw new IOException("预览模式不会发送文件");
            if (_receiver is null) throw new IOException("请先打开 AirDrop");
            _receiver.Send(command);
        }, Notify);
        Sender.PropertyChanged += (_, args) => { if (args.PropertyName is nameof(SenderController.StatusText) or nameof(SenderController.IsSending)) RecordState(); };
        if (!preview)
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run");
            _settings.AutoStart = key?.GetValue("XiatiaoAirDrop") is string;
            foreach (var item in Store.Read(Path.Combine(_data, "history.json"), new List<ReceivedItem>()))
                if (Path.IsPathFullyQualified(item.FullPath)) { item.LoadThumbnail(); Files.Add(item); }
        }
        SaveNameCommand = new ActionCommand(_ => Protect(SaveName));
        BrowseFolderCommand = new ActionCommand(_ => Protect(BrowseFolder));
        OpenFolderCommand = new ActionCommand(_ => Protect(() => { Directory.CreateDirectory(SaveFolder); Open(SaveFolder); }));
        OpenFileCommand = new ActionCommand(item => Protect(() => OpenItem(item, false)));
        ShowFileCommand = new ActionCommand(item => Protect(() => OpenItem(item, true)));
        AcceptCommand = new ActionCommand(item => Answer(item, true));
        DeclineCommand = new ActionCommand(item => Answer(item, false));
        ClearHistoryCommand = new ActionCommand(_ => Protect(() => { Files.Clear(); SaveHistory(); }));
        OpenNoticesCommand = new ActionCommand(_ => Protect(() => Open(Path.Combine(AppContext.BaseDirectory, "licenses", "THIRD-PARTY-NOTICES.md"))));
        _radio.StateChanged += (state, detail) => Dispatch(() => RadioState(state, detail));
        L.Changed += LanguageChanged;
    }
    public void StartSaved() { if (_settings.Enabled && !_preview) _ = SetEnabledAsync(true); }
    private void Save() { if (!_preview) Store.Write(Path.Combine(_data, "settings.json"), _settings); }
    private void SaveHistory() { if (!_preview) Store.Write(Path.Combine(_data, "history.json"), Files.ToArray()); Changed(nameof(FileCountText)); }
    public void RemoveHistory(IEnumerable<ReceivedItem> items) => Protect(() =>
    {
        foreach (var item in items.ToArray()) Files.Remove(item);
        SaveHistory();
    });
    public void ReportFileError(string message) => Error(message);
    public void ReportFileError(string template, params object[] arguments)
    {
        _error = template; _errorArguments = arguments;
        Changed(nameof(ErrorText)); Changed(nameof(HasError)); RecordState();
    }
    private void SaveName()
    {
        var name = DeviceName.Trim();
        if (name.Length == 0 || Encoding.UTF8.GetByteCount(name) > 63 || name.Any(char.IsControl)) throw new IOException("名称请使用 1–63 个 UTF-8 字节，不能包含控制字符。");
        _receiver?.Send(new { @do = "set", name });
        _settings.Name = name; DeviceName = name; Save(); ClearError();
        if (_enabled) Status("接收已开启", "名称已保存，新的发送请求将使用这个名称。");
    }
    private void BrowseFolder()
    {
        if (Pending.Count != 0) throw new IOException("请先完成或拒绝当前来件，再更换储存位置。");
        var picker = new OpenFolderDialog { Title = L.T("选择 AirDrop 文件储存位置"), InitialDirectory = Directory.Exists(SaveFolder) ? SaveFolder : "" };
        if (picker.ShowDialog() != true) return;
        var chosen = Path.GetFullPath(picker.FolderName);
        Store.ReceivedPath(chosen, ".xiatiao-path-check");
        var probe = Path.Combine(chosen, ".xiatiao-" + Guid.NewGuid().ToString("N"));
        using (new FileStream(probe, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1, FileOptions.DeleteOnClose)) { }
        _receiver?.Send(new { @do = "set", folder = chosen });
        _settings.Folder = chosen; _receiveFolders.Add(chosen); Save(); Changed(nameof(SaveFolder)); ClearError();
    }
    private void Answer(object? item, bool accept) => Protect(() =>
    {
        if (item is not TransferItem transfer || !Pending.Contains(transfer) || !transfer.IsWaiting) return;
        if (_receiver is null) throw new IOException("接收服务已关闭");
        _receiver.Send(new { @do = "answer", id = transfer.Id, accept });
        transfer.Update(accept ? "已接收，等待发送端传输…" : "已拒绝", transferring: accept);
    });
    private void OpenItem(object? item, bool reveal)
    {
        if (item is not ReceivedItem file || !Files.Contains(file)) return;
        if (!file.Exists) throw new IOException("文件已被移动或删除，历史记录仍保留。");
        if (reveal)
        {
            var info = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "explorer.exe")) { UseShellExecute = true };
            info.Arguments = "/select,\"" + file.FullPath.Replace("\"", "") + "\""; Process.Start(info);
        }
        else
        {
            // Never launch an executable/script received over the radio from a thumbnail click.
            var allowed = new[] { ".jpg", ".jpeg", ".png", ".heic", ".gif", ".webp", ".bmp", ".tif", ".tiff", ".pdf", ".txt", ".mp4", ".mov", ".mp3", ".wav", ".m4a" };
            if (!allowed.Contains(Path.GetExtension(file.FullPath).ToLowerInvariant())) { OpenItem(file, true); return; }
            Open(file.FullPath);
        }
    }
    private static void Open(string path) => Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
    public async Task SetEnabledAsync(bool enabled)
    {
        if ((_busy && enabled) || _disposed) return;
        _enabled = enabled; _settings.Enabled = enabled; _busy = true; _faultCleanup = false; ClearError(); NotifyState();
        try
        {
            Save();
            if (_preview) { _busy = false; Status(enabled ? "接收已开启" : "AirDrop 已关闭", "预览模式"); NotifyState(); return; }
            if (enabled) { Status("正在开启…", "正在准备无线连接和接收服务。"); await _radio.StartAsync(); }
            else
            {
                Status("正在关闭…", "正在结束文件传输并关闭无线连接。"); Sender.SetReady(false);
                if (_receiver is not null) { await _receiver.StopAsync(); _receiver = null; }
                Pending.Clear(); _radioStopped = new(TaskCreationOptions.RunContinuationsAsynchronously);
                await _radio.StopAsync();
            }
        }
        catch (Exception e) { Error(e.Message); _busy = false; _enabled = false; NotifyState(); }
    }
    private void RadioState(string state, string detail)
    {
        if (_disposed) return;
        if (state == "on")
        {
            if (!_enabled) { _ = _radio.StopAsync(); return; }
            try
            {
                if (_receiver is null)
                {
                    var receiver = new ReceiverProcess(); _receiver = receiver;
                    receiver.Event += message => Dispatch(() => { if (ReferenceEquals(_receiver, receiver)) HandleEvent(message); });
                    receiver.Failed += message => Dispatch(() => { if (ReferenceEquals(_receiver, receiver)) { Error(message); _busy = false; _enabled = false; NotifyState(); _ = StopFailedAsync(); } });
                    _receiveFolders.Clear(); _receiveFolders.Add(Path.GetFullPath(SaveFolder));
                    receiver.Start(_settings.Name, SaveFolder, _data);
                    Status("正在开启…", "无线连接已就绪，正在启动接收服务。");
                }
            }
            catch (Exception e) { _busy = false; _enabled = false; Error(e.Message); NotifyState(); _ = StopFailedAsync(); }
        }
        else if (state == "off") { _busy = false; _enabled = false; Sender.SetReady(false); Status("AirDrop 已关闭", detail); NotifyState(); _radioStopped?.TrySetResult(); }
        else if (state == "error") { _busy = false; _enabled = false; Sender.SetReady(false); Error(detail); NotifyState(); _radioStopped?.TrySetException(new IOException(detail)); if (!_faultCleanup) _ = StopFailedAsync(); }
        else { _busy = true; Status(state == "starting" ? "正在开启…" : "正在关闭…", detail); NotifyState(); }
    }
    private async Task StopFailedAsync()
    {
        if (_faultCleanup) return;
        _faultCleanup = true; Sender.SetReady(false);
        if (_receiver is { } receiver) { _receiver = null; await receiver.StopAsync(); }
        await _radio.StopAsync();
    }
    internal void HandleEvent(JsonElement message)
    {
        try
        {
            var type = Text(message, "event"); var id = Text(message, "id"); var item = Pending.FirstOrDefault(x => x.Id == id);
            Sender.HandleEvent(message);
            switch (type)
            {
                case "receiving":
                    if (message.GetProperty("on").GetBoolean()) { _busy = false; Sender.SetReady(true); Status("AirDrop 已开启", "附近设备可以向「" + _settings.Name + "」发送文件，也可点击“发送文件”。"); NotifyState(); }
                    else if (_enabled) { Sender.SetReady(false); Status("等待无线连接", Text(message, "reason")); }
                    break;
                case "asked":
                    var files = message.TryGetProperty("files", out var list) ? list.EnumerateArray().Select(x => Text(x, "name")).ToArray() : Array.Empty<string>();
                    var incoming = new TransferItem { Id = id, Sender = Text(message, "sender"), Description = files.Length == 1 ? files[0] : "", FileCount = files.Length == 1 ? null : files.Length };
                    Pending.Add(incoming); Notify("有文件要传给你", L.F("{0}：{1}。点击打开并确认接收。", incoming.Sender, incoming.Description)); break;
                case "answered":
                    if (Text(message, "how") == "accepted") item?.Update("等待发送端传输…", transferring: true);
                    else if (item is not null) { Pending.Remove(item); Status(_enabled ? "接收已开启" : "AirDrop 已关闭", "本次请求已结束。"); } break;
                case "incoming":
                    Status("正在接收文件", item?.SourceDescription ?? "文件传输中…", detailIsLiteral: item is { FileCount: null });
                    var done = message.TryGetProperty("done", out var d) && d.TryGetDouble(out var dv) ? dv : 0;
                    var total = message.TryGetProperty("total", out var t) && t.TryGetDouble(out var tv) ? tv : 0;
                    item?.Update(total > 0 ? $"正在接收 · {Math.Min(100, done * 100 / total):0}%" : "正在接收…", transferring: true, progress: total > 0 ? done * 100 / total : 0); break;
                case "stored":
                    var folder = Path.GetFullPath(Text(message, "folder"));
                    if (!_receiveFolders.Contains(folder) && !_preview) throw new IOException("接收文件目录与当前设置不一致");
                    foreach (var entry in message.GetProperty("names").EnumerateArray())
                    {
                        var path = Store.ReceivedPath(folder, entry.GetString() ?? ""); var file = new FileInfo(path);
                        if (!file.Exists) throw new IOException("接收服务报告完成，但文件未找到");
                        var record = new ReceivedItem { FullPath = path, Bytes = file.Length, ReceivedAt = DateTimeOffset.Now }; record.LoadThumbnail(); Files.Insert(0, record);
                    }
                    SaveHistory(); if (item is not null) Pending.Remove(item);
                    Status("接收已开启", "文件已收到，可以继续发送。");
                    Notify("文件已收到", L.F("已储存到「{0}」。", folder)); break;
                case "lost": if (item is not null) Pending.Remove(item); Error("本次传输中断：" + Text(message, "reason")); break;
                case "fatal": case "error": Error("接收服务：" + Text(message, "detail", Text(message, "reason"))); break;
            }
        }
        catch (Exception e) when (e is IOException or InvalidOperationException or ArgumentException or KeyNotFoundException) { Error(e.Message); }
    }
    private static string Text(JsonElement message, string key, string fallback = "") => message.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? fallback : fallback;
    private void Dispatch(Action action) { if (!_disposed) _ui.BeginInvoke(() => { if (!_disposed) action(); }); }
    private void Protect(Action action) { try { action(); } catch (Exception e) { Error(e.Message); } }
    private void ClearError() { _error = ""; _errorArguments = null; Changed(nameof(ErrorText)); Changed(nameof(HasError)); }
    private void Error(string message) { _error = message; _errorArguments = null; Changed(nameof(ErrorText)); Changed(nameof(HasError)); RecordState(); }
    private void Status(string status, string detail, bool detailIsLiteral = false) { _status = status; _detail = detail; _detailIsLiteral = detailIsLiteral; Changed(nameof(StatusText)); Changed(nameof(StatusDetail)); RecordState(); }
    private void NotifyState() { Changed(nameof(IsEnabled)); Changed(nameof(IsBusy)); RecordState(); }
    private void Notify(string title, string detail) => Notification?.Invoke(L.T(title), L.T(detail));
    private void LanguageChanged(object? sender, EventArgs args)
    {
        Changed(nameof(Language)); Changed(nameof(StatusText)); Changed(nameof(StatusDetail)); Changed(nameof(ErrorText)); Changed(nameof(FileCountText));
        foreach (var file in Files) file.RefreshLanguage();
        foreach (var item in Pending) item.RefreshLanguage();
        Sender.RefreshLanguage();
    }
    private void RecordState()
    {
        if (_preview) return;
        try { Store.Write(Path.Combine(_data, "runtime-state.json"), new { updatedUtc = DateTimeOffset.UtcNow, enabled = _enabled, busy = _busy, status = _status, detail = _detail, error = _error, processId = Environment.ProcessId, sending = Sender.IsSending, sendStatus = Sender.StatusText }); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }
    internal void Preview(bool filled, string? image = null)
    {
        _settings.Name = DeviceName = "Airdrop for Windows";
        _settings.Folder = @"C:\Users\You\Downloads\AirDrop";
        _enabled = true; Status("AirDrop 已开启", "附近设备可以向「Airdrop for Windows」发送文件，也可点击“发送文件”。"); NotifyState();
        if (filled)
        {
            var incoming = new TransferItem { Id = "preview", Sender = L.IsEnglish ? "My iPhone" : "我的 iPhone", Description = L.IsEnglish ? "Weekend walk.jpg" : "周末散步的照片.jpg" }; Pending.Add(incoming);
            foreach (var name in new[] { "IMG_9303.jpg", "海边的下午.png", "项目参考资料.pdf", "IMG_20261016_一张很长名字的照片.jpg" })
            {
                var record = new ReceivedItem { FullPath = image ?? Path.Combine(SaveFolder, name), Bytes = 394085, ReceivedAt = DateTimeOffset.Now };
                record.LoadThumbnail(); Files.Add(record);
            }
            Changed(nameof(FileCountText));
        }
    }
    public async Task ShutdownAsync()
    {
        if (_disposed) return;
        _enabled = false; _busy = true; Sender.SetReady(false); NotifyState();
        if (_receiver is { } receiver) { _receiver = null; await receiver.StopAsync(); }
        if (!_preview)
        {
            _radioStopped = new(TaskCreationOptions.RunContinuationsAsynchronously); await _radio.StopAsync();
            await _radioStopped.Task.WaitAsync(TimeSpan.FromSeconds(30));
        }
    }
    public void Dispose() { _disposed = true; L.Changed -= LanguageChanged; _receiver?.Dispose(); _radio.Dispose(); }
}
