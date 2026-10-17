// SPDX-License-Identifier: MIT
using System.Collections.ObjectModel;
using System.IO;
using System.Text.Json;
using System.Windows.Input;
using Microsoft.Win32;

namespace Winawdl.Xiatiao;

public sealed class ApplePeer : Observable
{
    public string Id { get; init; } = "";
    public string Name { get; init; } = "";
    private string _model = "";
    public string Model { get => L.T(_model); init => _model = value; }
    internal void RefreshLanguage() => Changed(nameof(Model));
}
public sealed class OutgoingFile
{
    public string FullPath { get; init; } = "";
    public long Bytes { get; init; }
    public string FileName => Path.GetFileName(FullPath);
    public string SizeText => Bytes >= 1048576 ? $"{Bytes / 1048576d:0.#} MB" : $"{Bytes / 1024d:0.#} KB";
}

/// <summary>User-selected files and discovered peers; shares the receive engine and radio.</summary>
public sealed class SenderController : Observable
{
    private readonly Action<object> _command;
    private readonly Action<string, string> _notify;
    private ApplePeer? _selected;
    private string? _transferId;
    private OutgoingFile[] _sendingFiles = Array.Empty<OutgoingFile>();
    private bool _wanted, _cancelRequested;
    private string _targetName = "";
    public ObservableCollection<OutgoingFile> Files { get; } = new();
    public ObservableCollection<ApplePeer> Peers { get; } = new();
    public ApplePeer? SelectedPeer { get => _selected; set { _selected = value; Changed(); Changed(nameof(CanSend)); } }
    public bool IsReady { get; private set; }
    public bool IsLooking { get; private set; }
    public bool IsSending => _transferId is not null;
    public bool CanSend => IsReady && !IsSending && Files.Count > 0 && SelectedPeer is not null && Peers.Contains(SelectedPeer);
    public bool CanEditFiles => !IsSending;
    public bool IsIndeterminate { get; private set; }
    public double Progress { get; private set; }
    private string _status = "选择文件和接收设备", _detail = "请先打开 AirDrop。";
    public string StatusText => L.T(_status);
    public string DetailText => L.Error(_detail);
    public ICommand ChooseFilesCommand { get; }
    public ICommand RemoveFileCommand { get; }
    public ICommand SearchCommand { get; }
    public ICommand SendCommand { get; }
    public ICommand CancelCommand { get; }

    internal SenderController(Action<object> command, Action<string, string> notify)
    {
        _command = command; _notify = notify;
        ChooseFilesCommand = new ActionCommand(_ => Protect(() =>
        {
            if (!CanEditFiles) return;
            var picker = new OpenFileDialog { Title = L.T("选择要发送的文件"), Multiselect = true, CheckFileExists = true };
            if (picker.ShowDialog() == true) AddFiles(picker.FileNames);
        }));
        RemoveFileCommand = new ActionCommand(value => { if (CanEditFiles && value is OutgoingFile file) Files.Remove(file); });
        SearchCommand = new ActionCommand(_ => Protect(() => { if (IsLooking) EndLooking(); BeginLooking(); }));
        SendCommand = new ActionCommand(_ => Protect(Send));
        CancelCommand = new ActionCommand(_ => Protect(Cancel));
        Files.CollectionChanged += (_, _) => Changed(nameof(CanSend));
        Peers.CollectionChanged += (_, _) => Changed(nameof(CanSend));
    }

    internal void AddFiles(IEnumerable<string> paths)
    {
        if (!CanEditFiles) return;
        // Validate the whole selection before changing it; the engine validates again at send time.
        var added = new List<OutgoingFile>();
        var known = Files.Select(x => x.FullPath).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var names = Files.Select(x => x.FileName).ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var path in paths)
        {
            var full = Path.GetFullPath(path);
            if (!known.Add(full)) continue;
            var info = new FileInfo(full);
            if (!info.Exists) throw new IOException("文件不存在：" + info.Name);
            if ((info.Attributes & (FileAttributes.ReparsePoint | FileAttributes.Directory)) != 0) throw new IOException("请选择普通文件：" + info.Name);
            if (!names.Add(info.Name)) throw new IOException("本次文件中有重名，请分开发送：" + info.Name);
            if (known.Count > 256) throw new IOException("一次最多选择 256 个文件。请分批发送。");
            added.Add(new OutgoingFile { FullPath = full, Bytes = info.Length });
        }
        foreach (var file in added) Files.Add(file);
    }

    internal bool TryAddFiles(IEnumerable<string> paths)
    {
        if (!CanEditFiles) return false;
        var accepted = false;
        Protect(() => { AddFiles(paths); accepted = true; });
        return accepted;
    }

    public void BeginLooking()
    {
        _wanted = true;
        if (!IsReady) { SetStatus("等待 AirDrop 开启", "请先在主窗口打开 AirDrop，开启后会自动搜索。", false); return; }
        if (IsLooking) return;
        Protect(() =>
        {
            _command(new { @do = "look", on = true });
            IsLooking = true; Changed(nameof(IsLooking));
            if (!IsSending) SetStatus("正在搜索 Apple 设备", "正在启动附近设备发现…", true);
        });
    }
    public void EndLooking()
    {
        _wanted = false;
        if (IsReady) Protect(() => _command(new { @do = "look", on = false }));
        IsLooking = false; Peers.Clear(); SelectedPeer = null; Changed(nameof(IsLooking));
        if (!IsSending) SetStatus("搜索已停止", "点击搜索，重新查找附近的 Apple 设备。", false);
    }
    internal void SetReady(bool ready)
    {
        IsReady = ready; Changed(nameof(IsReady)); Changed(nameof(CanSend));
        if (ready) { if (_wanted) BeginLooking(); else if (!IsSending) SetStatus("选择文件和接收设备", "打开搜索，查找附近的 Apple 设备。", false); return; }
        IsLooking = false; Peers.Clear(); SelectedPeer = null; Changed(nameof(IsLooking));
        if (IsSending) Finish("发送已中断", "AirDrop 连接已关闭，未确认发送完成。", false);
        else SetStatus("AirDrop 尚未就绪", "请在主窗口打开 AirDrop。", false);
    }
    private void Send()
    {
        if (!CanSend) return;
        foreach (var file in Files) if (!File.Exists(file.FullPath)) throw new IOException("文件已移动或删除：" + file.FileName);
        var id = Guid.NewGuid().ToString("N");
        _sendingFiles = Files.ToArray();
        _targetName = SelectedPeer!.Name; _transferId = id; _cancelRequested = false; Progress = 0;
        SetStatus("正在连接「" + _targetName + "」", "正在发送接收请求…", true); NotifyTransfer();
        try { _command(new { @do = "send", id, peer = SelectedPeer.Id, files = _sendingFiles.Select(x => x.FullPath).ToArray() }); }
        catch { _transferId = null; _sendingFiles = Array.Empty<OutgoingFile>(); NotifyTransfer(); throw; }
    }
    private void Cancel()
    {
        if (_transferId is null || _cancelRequested) return;
        _command(new { @do = "cancel", id = _transferId }); _cancelRequested = true;
        SetStatus("正在取消", "正在结束这次传输…", true);
    }
    internal void HandleEvent(JsonElement message)
    {
        var type = Text(message, "event");
        switch (type)
        {
            case "peer":
                if (!IsReady || !IsLooking) return;
                var id = Text(message, "id"); var name = Text(message, "name");
                if (id.Length == 0 || name.Length == 0 || Peers.Any(x => x.Id == id)) return;
                Peers.Add(new ApplePeer { Id = id, Name = name, Model = Text(message, "model", "Apple 设备") });
                if (!IsSending) SetStatus("找到附近设备", "选择接收设备，再点击发送。", false);
                break;
            case "peer_gone":
                var peer = Peers.FirstOrDefault(x => x.Id == Text(message, "id"));
                if (peer is not null) { Peers.Remove(peer); if (ReferenceEquals(SelectedPeer, peer)) SelectedPeer = null; }
                if (!IsSending && IsLooking && Peers.Count == 0) SetStatus("正在搜索 Apple 设备", "设备暂时离开，正在继续搜索…", true);
                break;
            case "looking":
                IsLooking = IsReady && message.TryGetProperty("on", out var on) && on.ValueKind == JsonValueKind.True;
                Changed(nameof(IsLooking));
                if (IsSending) break;
                if (IsLooking)
                {
                    var beacon = message.TryGetProperty("beacon", out var b) && b.ValueKind == JsonValueKind.True;
                    SetStatus(Peers.Count > 0 ? "找到附近设备" : "正在搜索 Apple 设备", beacon ? "请让接收设备保持解锁，并将 AirDrop 设为“所有人”。" : "蓝牙发现未启动，请在 Apple 设备上保持 AirDrop 页面打开。", Peers.Count == 0);
                }
                else SetStatus("搜索已停止", Text(message, "why", "点击搜索，重新查找附近设备。"), false);
                break;
            case "unwilling":
                if (!IsSending && message.TryGetProperty("count", out var count) && count.TryGetInt32(out var n) && n > 0)
                    SetStatus("附近设备限制了接收", "请在接收设备上将 AirDrop 改为“所有人，10 分钟”。", IsLooking);
                break;
            case "send":
                if (_transferId is null || Text(message, "id") != _transferId) return;
                var state = Text(message, "state");
                if (_cancelRequested && state is "asking" or "sending" or "finishing") return;
                var done = Number(message, "done"); var total = Number(message, "total");
                switch (state)
                {
                    case "asking": SetStatus("等待「" + _targetName + "」确认", "请在接收设备上点“接受”。", true); break;
                    case "sending": Progress = total > 0 ? Math.Clamp(done * 100 / total, 0, 100) : 0; SetStatus("正在发送到「" + _targetName + "」", $"已传输 {Progress:0}%", total <= 0); break;
                    case "finishing": Progress = 100; SetStatus("等待接收完成确认", "文件已上传，正在等待对方确认。", true); break;
                    case "sent": Progress = 100; Finish("发送完成", "「" + _targetName + "」已确认收到文件。", true); break;
                    case "cancelled": Finish("已取消发送", "这次发送已结束。", false); break;
                    case "declined": Finish("对方拒绝了接收", "可以选择其他设备，或稍后再试。", false); break;
                    case "refused": Finish("接收设备不允许连接", "请将接收设备的 AirDrop 设为“所有人”。", false); break;
                    case "failed": Finish("发送未完成", FailureDetail(Text(message, "reason")), false); break;
                }
                break;
        }
    }
    private static string FailureDetail(string reason) => reason switch
    {
        "gone" => "接收设备已离开，请重新搜索并选择设备。",
        "same_name" => "文件名重复，请将同名文件分开发送。",
        "not_a_file" or "unreadable" => "文件无法读取，可能已移动、删除或被占用。",
        "no_answer" => "对方没有及时确认，请保持设备解锁后重试。",
        "stalled" => "传输长时间没有进展，请检查两端无线连接。",
        "unreachable" => "无法连接接收设备，请保持 AirDrop 页面打开后重试。",
        "broken" => "传输连接中断，对方尚未确认收到完整文件。",
        _ => "这次发送没有完成，请重新选择设备后重试。"
    };
    private static string Text(JsonElement e, string key, string fallback = "") => e.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? fallback : fallback;
    private static double Number(JsonElement e, string key) => e.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetDouble(out var n) && double.IsFinite(n) ? n : 0;
    private void Finish(string status, string detail, bool success)
    {
        // Remove only this acknowledged batch, while editing is still locked.
        // An upload, cancellation or failed transfer must leave files available for retry.
        if (success) foreach (var file in _sendingFiles) Files.Remove(file);
        _sendingFiles = Array.Empty<OutgoingFile>();
        _transferId = null; _cancelRequested = false; SetStatus(status, detail, false); NotifyTransfer();
        if (success) _notify("文件已发送", detail);
    }
    private void NotifyTransfer() { Changed(nameof(IsSending)); Changed(nameof(CanSend)); Changed(nameof(CanEditFiles)); Changed(nameof(Progress)); }
    private void SetStatus(string status, string detail, bool indeterminate)
    {
        _status = status; _detail = detail; IsIndeterminate = indeterminate;
        Changed(nameof(StatusText)); Changed(nameof(DetailText)); Changed(nameof(IsIndeterminate)); Changed(nameof(Progress));
    }
    internal void RefreshLanguage()
    {
        Changed(nameof(StatusText)); Changed(nameof(DetailText));
        foreach (var peer in Peers) peer.RefreshLanguage();
    }
    private void Protect(Action action)
    {
        try { action(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException or InvalidOperationException or NotSupportedException or System.Security.SecurityException)
        { SetStatus("操作未完成", error.Message, false); }
    }
    internal void Preview()
    {
        IsReady = true; IsLooking = true;
        Files.Add(new OutgoingFile { FullPath = L.IsEnglish ? @"C:\Preview\Seaside afternoon.jpg" : @"C:\Preview\海边的下午.jpg", Bytes = 2530000 });
        Peers.Add(new ApplePeer { Id = "preview-only", Name = L.IsEnglish ? "My iPhone" : "我的 iPhone", Model = "iPhone" });
        Peers.Add(new ApplePeer { Id = "preview-mac", Name = L.IsEnglish ? "My Mac" : "我的 Mac", Model = "Mac" });
        SelectedPeer = Peers[0]; SetStatus("找到附近设备", "选择接收设备，再点击发送。", false);
    }
}
