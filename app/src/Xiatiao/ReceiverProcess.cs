// SPDX-License-Identifier: MIT
// Certificate creation adapted from winawdl (c) 2026 Raphaël Richaud, MIT.
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;

namespace Winawdl.Xiatiao;

internal sealed class ReceiverProcess : IDisposable
{
    private Process? _process;
    private bool _stopping;
    private readonly object _write = new();
    public event Action<JsonElement>? Event;
    public event Action<string>? Failed;
    public static void EnsureIdentity(string keys)
    {
        Directory.CreateDirectory(keys);
        var cert = Path.Combine(keys, "certificate.self-signed.pem"); var key = Path.Combine(keys, "key.self-signed.pem");
        if (File.Exists(cert) && File.Exists(key))
        {
            using var existing = X509Certificate2.CreateFromPemFile(cert, key);
            if (existing.NotAfter > DateTime.Now.AddDays(1)) return;
        }
        using var pair = RSA.Create(2048);
        var request = new CertificateRequest("CN=AirDrop receiver", pair, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        using var made = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(365));
        File.WriteAllText(key, pair.ExportPkcs8PrivateKeyPem()); File.WriteAllText(cert, made.ExportCertificatePem());
    }
    public void Start(string name, string folder, string? dataRoot = null, bool loopback = false, int port = 8771)
    {
        if (_process is not null) throw new InvalidOperationException("接收进程已启动");
        dataRoot ??= Store.Root; Directory.CreateDirectory(folder);
        var keys = Path.Combine(dataRoot, "keys"); EnsureIdentity(keys);
        var root = AppContext.BaseDirectory;
        var info = new ProcessStartInfo(Path.Combine(root, "python", "python.exe"))
        { UseShellExecute = false, CreateNoWindow = true, RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true,
          StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8, WorkingDirectory = Path.Combine(root, "receiver") };
        foreach (var arg in new[] { "-B", "-u", Path.Combine(root, "receiver", "engine.py"), "--name", name, "--outdir", folder, "--keys", keys, "--iface", "awdl0", "--port", port.ToString() }) info.ArgumentList.Add(arg);
        if (loopback) info.ArgumentList.Add("--loopback");
        info.Environment["PYTHONUTF8"] = "1";
        _stopping = false;
        _process = Process.Start(info) ?? throw new IOException("无法启动接收服务");
        _ = ReadAsync(_process, dataRoot);
    }
    private async Task ReadAsync(Process process, string dataRoot)
    {
        var errors = Task.Run(async () =>
        {
            var path = Path.Combine(dataRoot, "receiver.log");
            if (File.Exists(path) && new FileInfo(path).Length > 2_000_000) File.Move(path, path + ".previous", true);
            using var log = new StreamWriter(path, true, Encoding.UTF8) { AutoFlush = true };
            while (await process.StandardError.ReadLineAsync() is { } line) await log.WriteLineAsync(line);
        });
        try
        {
            while (await process.StandardOutput.ReadLineAsync() is { } line)
            {
                if (line.Length > 1048576) throw new IOException("接收服务消息过大");
                using var doc = JsonDocument.Parse(line); Event?.Invoke(doc.RootElement.Clone());
            }
            await process.WaitForExitAsync(); await errors;
            if (!_stopping) Failed?.Invoke("接收服务意外停止，请重新打开 AirDrop");
        }
        catch (Exception e) when (e is IOException or JsonException or InvalidOperationException) { if (!_stopping) Failed?.Invoke(e.Message); }
    }
    public void Send(object command)
    {
        lock (_write)
        {
            if (_process is null || _process.HasExited) throw new IOException("接收服务已停止");
            _process.StandardInput.WriteLine(JsonSerializer.Serialize(command)); _process.StandardInput.Flush();
        }
    }
    public async Task StopAsync()
    {
        _stopping = true; var process = _process; if (process is null) return;
        try
        {
            if (!process.HasExited)
            {
                Send(new { @do = "quit" }); process.StandardInput.Close();
                try { await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(8)); }
                catch (TimeoutException) { process.Kill(entireProcessTree: true); await process.WaitForExitAsync(); }
            }
        }
        catch (Exception e) when (e is IOException or InvalidOperationException) { }
        finally { process.Dispose(); _process = null; }
    }
    public void Dispose() { _stopping = true; try { _process?.StandardInput.Close(); } catch (IOException) { } }
}
