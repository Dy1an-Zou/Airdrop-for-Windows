// SPDX-License-Identifier: MIT
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;

namespace Winawdl.Xiatiao;

/// <summary>The unelevated UI talks only to the installed, same-user radio task.</summary>
public sealed class RadioClient : IDisposable
{
    public const string TaskName = "XiatiaoAirDrop-Radio";
    private readonly SemaphoreSlim _connectionGate = new(1, 1);
    private readonly SemaphoreSlim _writeGate = new(1, 1);
    private readonly CancellationTokenSource _lifetime = new();
    private NamedPipeClientStream? _pipe;
    private StreamWriter? _writer;
    private bool _disposed;
    private volatile bool _requested;
    private volatile bool _radioMayBeRunning;
    public event Action<string, string>? StateChanged;

    internal static string PipeName => "XiatiaoAirDrop.Radio." +
        (WindowsIdentity.GetCurrent().User?.Value ?? throw new InvalidOperationException("无法识别当前用户"));

    public async Task StartAsync()
    {
        if (_disposed) return;
        _requested = true;
        ReportStarting("正在连接无线助手…");
        try
        {
            await EnsureConnectedAsync();
            ReportStarting("正在提交无线连接开启请求…");
            await SendRequestedAsync();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or
            InvalidOperationException or System.ComponentModel.Win32Exception or TimeoutException)
        {
            StateChanged?.Invoke("error", "无线助手未启动：" + error.Message);
        }
        catch (OperationCanceledException) { }
    }

    public async Task StopAsync()
    {
        if (_disposed) return;
        _requested = false;
        StateChanged?.Invoke("stopping", "正在关闭 AirDrop…");
        try
        {
            // START may still be establishing and authenticating its pipe. Wait
            // for that operation before deciding whether a writer is available.
            await _connectionGate.WaitAsync(_lifetime.Token);
            try
            {
                if (_pipe is not { IsConnected: true } || _writer is null)
                {
                    StateChanged?.Invoke(_radioMayBeRunning ? "error" : "off", _radioMayBeRunning ?
                        "无线助手未连接，无法确认无线已关闭" : "AirDrop 已关闭");
                    return;
                }
                await SendRequestedAsync();
            }
            finally { _connectionGate.Release(); }
        }
        catch (Exception error) when (error is IOException or InvalidOperationException)
        {
            StateChanged?.Invoke("error", "关闭请求未确认：" + error.Message);
        }
        catch (OperationCanceledException) { }
    }

    private async Task EnsureConnectedAsync()
    {
        await _connectionGate.WaitAsync(_lifetime.Token);
        try
        {
            if (_pipe is { IsConnected: true } && _writer is not null) return;
            ReportStarting("正在检查软件安装位置…");
            RadioHost.ValidateInstalledLocation();
            ReportStarting("正在连接无线助手…");
            if (!await ConnectAsync(200))
            {
                ReportStarting("正在启动无线助手…");
                var info = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "schtasks.exe"))
                {
                    UseShellExecute = false, CreateNoWindow = true,
                    RedirectStandardOutput = true, RedirectStandardError = true,
                };
                foreach (var item in new[] { "/Run", "/TN", TaskName }) info.ArgumentList.Add(item);
                using var task = Process.Start(info) ?? throw new IOException("无法启动已安装的无线助手任务");
                var output = task.StandardOutput.ReadToEndAsync();
                var errors = task.StandardError.ReadToEndAsync();
                await task.WaitForExitAsync(_lifetime.Token).WaitAsync(TimeSpan.FromSeconds(10), _lifetime.Token);
                await output;
                var reason = (await errors).Trim();
                if (task.ExitCode != 0)
                    throw new IOException(reason.Length > 0 ? reason : "请先完成 Airdrop for Windows 安装");
                ReportStarting("正在等待无线助手完成启动检查…");
                if (!await ConnectAsync(12000)) throw new IOException("无线助手没有响应，请查看诊断日志");
            }
            ReportStarting("正在核对无线助手连接…");
            var pipe = _pipe!;
            if (!GetNamedPipeServerProcessId(pipe.SafePipeHandle, out var pid))
                throw new IOException("无法验证无线助手进程");
            using var server = Process.GetProcessById(checked((int)pid));
            var expected = Path.Combine(RadioHost.InstallFolder, "XiatiaoAirDrop.exe");
            if (!string.Equals(Native.ProcessImage(server.Id), expected, StringComparison.OrdinalIgnoreCase) ||
                server.SessionId != Process.GetCurrentProcess().SessionId)
                throw new UnauthorizedAccessException("无线助手不属于已安装的当前会话");
            _writer = new StreamWriter(pipe, new UTF8Encoding(false), 1024, leaveOpen: true) { AutoFlush = true };
            _ = ReadAsync(pipe);
        }
        catch
        {
            _pipe?.Dispose();
            _pipe = null;
            _writer = null;
            throw;
        }
        finally { _connectionGate.Release(); }
    }

    private void ReportStarting(string detail)
    {
        // Connection checks can finish after STOP has been requested. A progress
        // label must not replace that newer intent with a starting state.
        if (_requested) StateChanged?.Invoke("starting", detail);
    }

    private async Task<bool> ConnectAsync(int milliseconds)
    {
        var next = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut,
            PipeOptions.Asynchronous, TokenImpersonationLevel.Identification);
        try
        {
            await next.ConnectAsync(milliseconds, _lifetime.Token);
            _pipe = next;
            return true;
        }
        catch (TimeoutException) { next.Dispose(); return false; }
        catch { next.Dispose(); throw; }
    }

    private async Task SendRequestedAsync()
    {
        await _writeGate.WaitAsync(_lifetime.Token);
        try
        {
            var writer = _writer ?? throw new IOException("无线助手已断开");
            // Read the latest user intent only after acquiring the write gate;
            // a queued START must never overtake a later STOP request.
            var command = _requested ? "start" : "stop";
            if (command == "start") _radioMayBeRunning = true;
            await writer.WriteLineAsync(JsonSerializer.Serialize(new { command }).AsMemory(), _lifetime.Token);
            await writer.FlushAsync(_lifetime.Token);
        }
        finally { _writeGate.Release(); }
    }

    private async Task ReadAsync(NamedPipeClientStream pipe)
    {
        try
        {
            using var reader = new StreamReader(pipe, Encoding.UTF8, false, 1024, leaveOpen: true);
            while (await RadioProtocol.ReadLineAsync(reader, _lifetime.Token) is { } line)
            {
                using var document = JsonDocument.Parse(line);
                var state = document.RootElement.GetProperty("state").GetString() ?? "error";
                if (state is not ("off" or "starting" or "on" or "stopping" or "error"))
                    throw new IOException("无线助手返回了无效状态");
                var detail = document.RootElement.GetProperty("detail").GetString() ?? "";
                // Server progress cannot undo a newer user request to stop.
                if (state == "off") _radioMayBeRunning = false;
                StateChanged?.Invoke(state, detail);
            }
            if (!_disposed) StateChanged?.Invoke("error", "无线助手连接已结束；接收已暂停");
        }
        catch (Exception error) when (error is IOException or InvalidDataException or JsonException or InvalidOperationException or
            KeyNotFoundException or OperationCanceledException or ObjectDisposedException)
        {
            if (!_disposed) StateChanged?.Invoke("error", "无线助手连接中断：" + error.Message);
        }
        finally
        {
            if (ReferenceEquals(_pipe, pipe)) { _pipe = null; _writer = null; }
            pipe.Dispose();
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _lifetime.Cancel();
        // EOF is an authenticated owner's shutdown, even if an explicit stop cannot be delivered.
        _pipe?.Dispose();
        _pipe = null;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetNamedPipeServerProcessId(SafePipeHandle pipe, out uint processId);
}

internal static class RadioProtocol
{
    internal static string ParseCommand(string line)
    {
        if (line.Length > 1024) throw new InvalidDataException("命令过长");
        using var json = JsonDocument.Parse(line);
        var root = json.RootElement;
        if (root.ValueKind != JsonValueKind.Object || root.EnumerateObject().Count() != 1 ||
            !root.TryGetProperty("command", out var value) || value.ValueKind != JsonValueKind.String ||
            value.GetString() is not ("start" or "stop"))
            throw new InvalidDataException("只允许开启或关闭无线助手");
        return value.GetString()!;
    }

    internal static async Task<string?> ReadLineAsync(StreamReader reader, CancellationToken cancellation)
    {
        var text = new StringBuilder();
        var one = new char[1];
        while (await reader.ReadAsync(one.AsMemory(), cancellation) != 0)
        {
            if (one[0] == '\n') return text.ToString().TrimEnd('\r');
            if (text.Length >= 1024) throw new InvalidDataException("助手消息超出长度限制");
            text.Append(one[0]);
        }
        if (text.Length != 0) throw new InvalidDataException("助手消息不完整");
        return null;
    }
}
