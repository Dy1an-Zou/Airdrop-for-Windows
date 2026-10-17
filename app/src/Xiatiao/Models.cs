// SPDX-License-Identifier: MIT
using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace Winawdl.Xiatiao;

public abstract class Observable : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;
    protected void Changed([CallerMemberName] string? property = null) => PropertyChanged?.Invoke(this, new(property));
}
public sealed class ActionCommand(Action<object?> action) : ICommand
{
    public event EventHandler? CanExecuteChanged { add { } remove { } }
    public bool CanExecute(object? parameter) => true;
    public void Execute(object? parameter) => action(parameter);
}
public sealed class ReceivedItem : Observable
{
    public string FullPath { get; set; } = "";
    public long Bytes { get; set; }
    public DateTimeOffset ReceivedAt { get; set; }
    [System.Text.Json.Serialization.JsonIgnore] public string FileName => Path.GetFileName(FullPath);
    [System.Text.Json.Serialization.JsonIgnore] public string SizeText => Bytes >= 1048576 ? $"{Bytes / 1048576d:0.#} MB" : $"{Bytes / 1024d:0.#} KB";
    [System.Text.Json.Serialization.JsonIgnore] public string TimeText => ReceivedAt.LocalDateTime.ToString(L.IsEnglish ? "MMM d, HH:mm" : "MM月dd日 HH:mm", L.Culture);
    [System.Text.Json.Serialization.JsonIgnore] public bool Exists => File.Exists(FullPath);
    [System.Text.Json.Serialization.JsonIgnore] public string Kind => Path.GetExtension(FullPath).TrimStart('.').ToUpperInvariant() is { Length: > 0 } ext ? ext : L.T("文件");
    [System.Text.Json.Serialization.JsonIgnore] public ImageSource? Thumbnail { get; private set; }
    internal void RefreshLanguage() { Changed(nameof(TimeText)); Changed(nameof(Kind)); }
    public void LoadThumbnail()
    {
        if (!Exists || !new[] { ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp", ".tif", ".heic" }.Contains(Path.GetExtension(FullPath).ToLowerInvariant())) return;
        try
        {
            var image = new BitmapImage(); image.BeginInit(); image.CacheOption = BitmapCacheOption.OnLoad;
            image.DecodePixelWidth = 320; image.UriSource = new Uri(FullPath); image.EndInit(); image.Freeze(); Thumbnail = image;
        }
        catch (Exception e) when (e is IOException or NotSupportedException or ArgumentException or System.Runtime.InteropServices.COMException) { }
    }
}
public sealed class TransferItem : Observable
{
    public string Id { get; init; } = "";
    private string _sender = "", _description = "", _progressText = "等待你确认";
    public string Sender { get => _sender.Length == 0 ? L.T("附近设备") : _sender; init => _sender = value; }
    public int? FileCount { get; init; }
    public string Description { get => FileCount is int count ? L.F("{0} 个文件", count) : _description.Length == 0 ? L.T("文件") : _description; init => _description = value; }
    internal string SourceDescription => FileCount is int count ? $"{count} 个文件" : _description;
    public double Progress { get; private set; }
    public string ProgressText => L.T(_progressText);
    public bool IsWaiting { get; private set; } = true;
    public bool IsTransferring { get; private set; }
    public void Update(string text, bool waiting = false, bool transferring = false, double progress = 0)
    {
        _progressText = text; IsWaiting = waiting; IsTransferring = transferring; Progress = Math.Clamp(progress, 0, 100);
        Changed(nameof(ProgressText)); Changed(nameof(IsWaiting)); Changed(nameof(IsTransferring)); Changed(nameof(Progress));
    }
    internal void RefreshLanguage() { Changed(nameof(Sender)); Changed(nameof(Description)); Changed(nameof(ProgressText)); }
}
internal sealed class Preferences
{
    public string Name { get; set; } = "Airdrop for Windows";
    public string Folder { get; set; } = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads", "Airdrop for Windows");
    public bool Enabled { get; set; } = true;
    public bool AutoStart { get; set; } = true;
    public string Language { get; set; } = "zh-CN";
}
internal static class Store
{
    public static readonly string Root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "XiatiaoAirDrop");
    public static T Read<T>(string path, T fallback)
    {
        try { return File.Exists(path) ? JsonSerializer.Deserialize<T>(File.ReadAllText(path)) ?? fallback : fallback; }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException) { return fallback; }
    }
    public static void Write<T>(string path, T data)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temp = path + ".tmp"; File.WriteAllText(temp, JsonSerializer.Serialize(data, new JsonSerializerOptions { WriteIndented = true }));
        File.Move(temp, path, true);
    }
    public static string ReceivedPath(string folder, string name)
    {
        if (string.IsNullOrWhiteSpace(name) || Path.IsPathRooted(name)) throw new IOException("接收端返回了无效文件名");
        var root = Path.GetFullPath(folder).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        var result = Path.GetFullPath(Path.Combine(root, name));
        if (!result.StartsWith(root, StringComparison.OrdinalIgnoreCase) || name.Contains(':')) throw new IOException("接收文件路径越界");
        for (var part = new FileInfo(result) as FileSystemInfo; part is not null; part = part is FileInfo file ? file.Directory : ((DirectoryInfo)part).Parent)
        {
            if (part.Exists && part.Attributes.HasFlag(FileAttributes.ReparsePoint)) throw new IOException("接收路径不能经过符号链接");
            if (part.FullName.TrimEnd('\\').Equals(root.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)) break;
        }
        return result;
    }
}
