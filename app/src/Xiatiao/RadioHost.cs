// SPDX-License-Identifier: MIT
// Console ownership and graceful interrupt approach adapted from raph559/winawdl
// app/src/Radio/RadioSupervisor.cs, Copyright (c) 2026 Raphaël Richaud.
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;

namespace Winawdl.Xiatiao;

/// <summary>Fixed installed helper. It never installs a driver, changes test signing,
/// accepts executable paths from a client, or handles received files.</summary>
public static class RadioHost
{
    public static string InstallFolder => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "XiatiaoAirDrop");
    internal static string LogsFolder => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "XiatiaoAirDrop", "radio");
    internal const string StopMarker = "Radio STOP confirmed: stopped=1 quiescent=1 ready=0.";
    internal const string SupportedHardware = @"PCI\VEN_14C3&DEV_7925&SUBSYS_60001A3B&REV_00";
    private static string? _instance;
    internal static string Instance => _instance ?? throw new IOException("尚未确认专用 MT7925 网卡");
    private const string DriverSha = "938148d69b14c911c18c4c7a9ef00703ad5625e46d876143b8f887df6762aafb";
    internal static RadioBackend Backend { get; private set; } = RadioBackend.MediaTek;

    public static int Run(string[] args)
    {
        if (args.Length != 0) return 2;
        try
        {
            if (!new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))
                throw new UnauthorizedAccessException("无线助手必须由已安装的高权限任务运行");
            ValidateInstalledLocation();
            var manifestPath = Path.Combine(InstallFolder, "runtime-manifest.json");
            ProtectedFiles.CheckChain(manifestPath, InstallFolder);
            using var manifest = JsonDocument.Parse(File.ReadAllText(manifestPath));
            Backend = RadioBackend.FromManifest(manifest.RootElement);
            ProtectedFiles.VerifyManifest(InstallFolder, Backend.Pins);
            // Installer creates this directory; the helper never adopts an unsafe existing path.
            ProtectedFiles.CheckChain(LogsFolder,
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "XiatiaoAirDrop"));
            using var unique = new Mutex(true, @"Local\" + RadioClient.PipeName + ".host", out var first);
            if (!first) return 0;
            return ServeAsync().GetAwaiter().GetResult();
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("无线助手：" + error.Message);
            return 2;
        }
    }

    public static void ValidateInstalledLocation()
    {
        if (!string.Equals(Path.TrimEndingDirectorySeparator(Path.GetFullPath(AppContext.BaseDirectory)),
            Path.TrimEndingDirectorySeparator(InstallFolder), StringComparison.OrdinalIgnoreCase))
            throw new UnauthorizedAccessException("请从 Program Files 中安装好的 Airdrop for Windows 启动");
        ProtectedFiles.CheckChain(InstallFolder,
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles));
        ProtectedFiles.CheckChain(Path.Combine(InstallFolder, "XiatiaoAirDrop.exe"), InstallFolder);
    }

    private static async Task<int> ServeAsync()
    {
        var sid = WindowsIdentity.GetCurrent().User ?? throw new InvalidOperationException("缺少用户标识");
        var security = new PipeSecurity();
        security.SetAccessRuleProtection(true, false);
        security.AddAccessRule(new PipeAccessRule(new SecurityIdentifier(WellKnownSidType.NetworkSid, null),
            PipeAccessRights.FullControl, AccessControlType.Deny));
        security.AddAccessRule(new PipeAccessRule(sid, PipeAccessRights.FullControl, AccessControlType.Allow));
        using var pipe = NamedPipeServerStreamAcl.Create(RadioClient.PipeName, PipeDirection.InOut, 1,
            PipeTransmissionMode.Byte, PipeOptions.Asynchronous, 4096, 4096, security);
        using (var arrival = new CancellationTokenSource(TimeSpan.FromSeconds(20)))
        {
            try { await pipe.WaitForConnectionAsync(arrival.Token); }
            catch (OperationCanceledException) { return 0; }
        }
        string? clientSid = null;
        pipe.RunAsClient(() => clientSid = WindowsIdentity.GetCurrent(true)?.User?.Value);
        if (clientSid != sid.Value || !Native.GetNamedPipeClientProcessId(pipe.SafePipeHandle, out var clientPid))
            throw new UnauthorizedAccessException("无线助手拒绝了不属于当前用户的连接");
        using var owner = Process.GetProcessById(checked((int)clientPid));
        if (owner.SessionId != Process.GetCurrentProcess().SessionId ||
            !string.Equals(Native.ProcessImage(owner.Id), Path.Combine(InstallFolder, "XiatiaoAirDrop.exe"),
                StringComparison.OrdinalIgnoreCase))
            throw new UnauthorizedAccessException("无线助手仅接受当前会话中已安装的虾条 AirDrop");

        using var disconnected = new CancellationTokenSource();
        using var writer = new StreamWriter(pipe, new UTF8Encoding(false), 1024, true) { AutoFlush = true };
        using var writerGate = new SemaphoreSlim(1, 1);
        async Task Say(string state, string detail)
        {
            try
            {
                await writerGate.WaitAsync();
                try { await writer.WriteLineAsync(JsonSerializer.Serialize(new { state, detail })).WaitAsync(TimeSpan.FromSeconds(3)); }
                finally { writerGate.Release(); }
            }
            catch (Exception error) when (error is IOException or ObjectDisposedException or TimeoutException)
            { disconnected.Cancel(); pipe.Dispose(); }
        }
        using var session = new RadioSession(Say);
        _ = Task.Run(async () =>
        {
            try { await owner.WaitForExitAsync(disconnected.Token); disconnected.Cancel(); }
            catch (OperationCanceledException) { }
        });
        // Connection alone is not a radio state transition. The client has already
        // requested START; an unsolicited "off" here would revoke that request.
        try
        {
            using var reader = new StreamReader(pipe, Encoding.UTF8, false, 1024, true);
            while (await RadioProtocol.ReadLineAsync(reader, disconnected.Token) is { } line)
            {
                var command = RadioProtocol.ParseCommand(line);
                if (command == "start") _ = session.StartAsync();
                else await session.StopAsync();
            }
        }
        catch (Exception error) when (error is IOException or InvalidDataException or OperationCanceledException or JsonException)
        { /* Invalid input and disconnect both revoke the sole owner's radio session. */ }
        finally
        {
            disconnected.Cancel();
            await session.StopAsync();
        }
        return session.Faulted ? 3 : 0;
    }

    internal static async Task VerifyDeviceAsync()
    {
        if (Backend.IsRealtek) { await VerifyRealtekAsync(); return; }
        var json = await SystemCommand.PowerShellAsync(
            "$matches=@(Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object {" +
            "$_.InstanceId.StartsWith('" + SupportedHardware + "\\',[StringComparison]::OrdinalIgnoreCase)});" +
            "if($matches.Count -ne 1){throw 'Exactly one supported dedicated MT7925 adapter is required.'};$d=$matches[0];" +
            "$s=(Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Service).Data;" +
            "$p=(Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data;" +
            "$v=Get-ItemProperty -LiteralPath HKLM:\\SYSTEM\\CurrentControlSet\\Services\\mt7925_bringup;" +
            "@{instance=$d.InstanceId;status=$d.Status;service=$s;problem=$p;image=$v.ImagePath}|ConvertTo-Json -Compress");
        using var doc = JsonDocument.Parse(json);
        var value = doc.RootElement;
        var candidate = ValidateInstance(value.GetProperty("instance").GetString());
        if (_instance is not null && !string.Equals(candidate, _instance, StringComparison.OrdinalIgnoreCase))
            throw new IOException("专用 MT7925 网卡在本次连接中发生变化");
        if (value.GetProperty("status").GetString() != "OK" ||
            value.GetProperty("service").GetString() != "mt7925_bringup" ||
            value.GetProperty("problem").GetInt32() != 0)
            throw new IOException("专用 MT7925 网卡的实验驱动未就绪；没有修改其他网卡");
        var image = value.GetProperty("image").GetString() ?? "";
        var windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
        if (!image.StartsWith(@"\SystemRoot\System32\DriverStore\FileRepository\", StringComparison.OrdinalIgnoreCase))
            throw new IOException("专用网卡的驱动路径不符合已验证版本");
        var path = Path.Combine(windows, image[@"\SystemRoot\".Length..]);
        if (!File.Exists(path) || ProtectedFiles.Hash(path) != DriverSha)
            throw new IOException("专用网卡驱动与已验证的 r36 版本不同");
        _instance = candidate;
    }

    private static async Task VerifyRealtekAsync()
    {
        // The exact PnP instance is checked before any use. No driver binding or
        // restart is attempted; Get-WindowsDriver only reads the installed package.
        var json = await SystemCommand.PowerShellAsync(
            "$system=[Environment]::SystemDirectory;" +
            "foreach($dll in @('vcruntime140.dll','vcruntime140_1.dll','msvcp140.dll')){" +
            "if(-not (Test-Path -LiteralPath (Join-Path $system $dll))){throw 'RTL8852CE requires Microsoft Visual C++ 2015-2022 x64 Redistributable'}};" +
            "$priv=& (Join-Path $system 'whoami.exe') /priv /fo csv /nh;" +
            "if($LASTEXITCODE -ne 0 -or -not ($priv -match 'SeLockMemoryPrivilege')){throw 'RTL8852CE requires Lock pages in memory for this account; sign out and sign in after granting it'};" +
            "$matches=@(Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object {" +
            "$_.InstanceId -match '^PCI\\\\VEN_10EC&DEV_C852(?:&|\\\\)'});" +
            "if($matches.Count -ne 1){throw '需要且仅支持一块专用 RTL8852CE 网卡。'};$d=$matches[0];" +
            "$s=(Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Service).Data;" +
            "$p=(Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data;" +
            "$g=(Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_ClassGuid).Data;" +
            "$ids=@((Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_HardwareIds).Data);" +
            "$inf=(Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data;" +
            "if($inf -notmatch '^oem[0-9]+\\.inf$'){throw 'RTL8852CE driver INF is not an installed OEM package'};" +
            "$packages=@(Get-WindowsDriver -Online -Driver $inf -ErrorAction Stop);" +
            "if($packages.Count -ne 1){throw 'RTL8852CE driver package is ambiguous'};" +
            "@{instance=$d.InstanceId;status=$d.Status;service=$s;problem=$p;classGuid=[string]$g;hardwareIds=$ids;" +
            "inf=$packages[0].OriginalFileName}|ConvertTo-Json -Compress");
        using var doc = JsonDocument.Parse(json);
        var candidate = RadioBackend.ValidateRealtekDevice(doc.RootElement);
        if (_instance is not null && !string.Equals(candidate, _instance, StringComparison.OrdinalIgnoreCase))
            throw new IOException("专用无线网卡在本次连接中发生变化");
        var windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
        var path = RadioBackend.RealtekDriverPath(windows, doc.RootElement.GetProperty("inf").GetString());
        ProtectedFiles.CheckChain(path, Path.Combine(windows, "System32", "DriverStore", "FileRepository"));
        if (RadioBackend.RealtekDriverImageHash(File.ReadAllBytes(path)) != RadioBackend.RealtekDriverSha)
            throw new IOException("RTL8852CE 驱动与固定的 winawdl 0.3.0 版本不同");
        _instance = candidate;
    }

    internal static string ValidateInstance(string? value)
    {
        var prefix = SupportedHardware + "\\";
        if (value is null || !value.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) ||
            value.Length <= prefix.Length || value.Length > 240 ||
            !value[prefix.Length..].All(c => char.IsAsciiLetterOrDigit(c) || c is '&' or '_'))
            throw new InvalidDataException("专用 MT7925 网卡标识无效");
        return value;
    }

    /// <summary>Offline checks only: no task, pipe server, firewall, driver handle, or RF is opened.</summary>
    public static int Check(string[] args)
    {
        if (args.Length != 0) return 2;
        var checks = 0;
        void Expect(bool condition) { checks++; if (!condition) throw new InvalidOperationException("host check " + checks); }
        void Refuses(Action action)
        {
            var refused = false;
            try { action(); } catch (Exception e) when (e is IOException or InvalidDataException or JsonException or ArgumentException or UnauthorizedAccessException) { refused = true; }
            Expect(refused);
        }
        try
        {
            var supported = SupportedHardware + @"\6&1234ABCD&0&0011";
            Expect(ValidateInstance(supported) == supported);
            foreach (var invalid in new[] { SupportedHardware, SupportedHardware + "\\", supported + "'", supported + "\\other", supported.Replace("DEV_7925", "DEV_7922"), "USB\\VEN_14C3&DEV_7925" })
                Refuses(() => ValidateInstance(invalid));
            Expect(RadioProtocol.ParseCommand("{\"command\":\"start\"}") == "start");
            Expect(RadioProtocol.ParseCommand("{\"command\":\"stop\"}") == "stop");
            foreach (var invalid in new[] { "{}", "[]", "{\"command\":true}", "{\"command\":\"exec\"}",
                "{\"command\":\"start\",\"path\":\"evil.exe\"}", "{\"command\":\"start\",\"command\":\"stop\"}", new string('a', 1025) })
                Refuses(() => RadioProtocol.ParseCommand(invalid));
            foreach (var path in new[] { "../evil.exe", "runtime/../../evil", "C:/evil.exe", "/evil", "runtime/a:b", "runtime//a", "runtime/./a", "runtime\\..\\a" })
                Refuses(() => ProtectedFiles.MemberPath(@"C:\\Program Files\\XiatiaoAirDrop", path));
            Expect(ProtectedFiles.MemberPath(@"C:\Program Files\XiatiaoAirDrop", "runtime/awdld.exe") == @"C:\Program Files\XiatiaoAirDrop\runtime\awdld.exe");
            var clean = new RadioStatus(0, 0, 0, 0, 1, 1, 1, 1);
            Expect(clean.IsStopped && !clean.IsFresh);
            Expect(new RadioStatus(0, 0, 0, 0, 0, 0, 0, 0).IsFresh);
            foreach (var bad in new[] { clean with { Error = 1 }, clean with { StopError = 1 }, clean with { Ready = 1 },
                clean with { Stopped = 0 }, clean with { Quiescent = 0 }, clean with { Result = 1 } }) Expect(!bad.IsStopped);
            Expect(StopDecision.Confirmed(true, true, true, true, true));
            for (var omitted = 0; omitted < 5; omitted++)
            {
                var gates = Enumerable.Repeat(true, 5).ToArray(); gates[omitted] = false;
                Expect(!StopDecision.Confirmed(gates[0], gates[1], gates[2], gates[3], gates[4]));
            }
            // A writable owner remains unsafe even if its data ACL happens to be read-only.
            Expect(!ProtectedFiles.TrustedWriter(new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null)));
            Expect(!ProtectedFiles.TrustedWriter(WindowsIdentity.GetCurrent().User!));
            Expect(ProtectedFiles.TrustedWriter(new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null)));
            using (var reader = new StreamReader(new MemoryStream(Encoding.UTF8.GetBytes("{\"command\":\"stop\"}\n"))))
                Expect(RadioProtocol.ReadLineAsync(reader, CancellationToken.None).GetAwaiter().GetResult() == "{\"command\":\"stop\"}");
            foreach (var invalid in new[] { "unterminated", new string('x', 1025) + "\n" })
                Refuses(() => { using var reader = new StreamReader(new MemoryStream(Encoding.UTF8.GetBytes(invalid))); RadioProtocol.ReadLineAsync(reader, CancellationToken.None).GetAwaiter().GetResult(); });
            var simulatedAcl = new DirectorySecurity();
            simulatedAcl.SetOwner(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null));
            simulatedAcl.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null), FileSystemRights.ReadAndExecute, AccessControlType.Allow));
            ProtectedFiles.CheckAcl(simulatedAcl, "read-only-users"); checks++;
            simulatedAcl.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null), FileSystemRights.WriteData, AccessControlType.Allow));
            Refuses(() => ProtectedFiles.CheckAcl(simulatedAcl, "writable-users"));
            var userOwnedAcl = new DirectorySecurity();
            userOwnedAcl.SetOwner(WindowsIdentity.GetCurrent().User!);
            Refuses(() => ProtectedFiles.CheckAcl(userOwnedAcl, "user-owned-read-only"));
            var scratch = Directory.CreateTempSubdirectory("xiatiao-host-check-");
            var writeProbe = Path.Combine(scratch.FullName, "ordinary-user-write.txt");
            try
            {
                // Exercise the real ordinary-user directory as created. Changing its
                // owner or DACL is unnecessary and may itself require extra privileges.
                File.WriteAllText(writeProbe, "ordinary-user writable");
                Refuses(() => ProtectedFiles.CheckChain(scratch.FullName, scratch.FullName));
            }
            finally { if (File.Exists(writeProbe)) File.Delete(writeProbe); scratch.Delete(false); }
            Console.WriteLine(JsonSerializer.Serialize(new { checks, passed = true, radioStarted = false, systemChanged = false }));
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
}

internal static class ProtectedFiles
{
    private const FileSystemRights WriteRights = FileSystemRights.Write | FileSystemRights.Delete |
        FileSystemRights.DeleteSubdirectoriesAndFiles | FileSystemRights.ChangePermissions | FileSystemRights.TakeOwnership;
    internal static bool TrustedWriter(SecurityIdentifier sid) => sid.Value is "S-1-5-18" or "S-1-5-32-544" or
        "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

    internal static string Hash(string path)
    {
        using var file = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(file)).ToLowerInvariant();
    }

    internal static string MemberPath(string root, string relative)
    {
        var components = relative.Replace('\\', '/').Split('/');
        if (Path.IsPathRooted(relative) || relative.Contains(':') ||
            components.Any(part => part is "" or "." or ".." || part.EndsWith('.') || part.EndsWith(' ')))
            throw new InvalidDataException("安装清单路径无效");
        var result = Path.GetFullPath(Path.Combine(root, Path.Combine(components)));
        if (!result.StartsWith(Path.TrimEndingDirectorySeparator(Path.GetFullPath(root)) + Path.DirectorySeparatorChar,
            StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("安装清单路径越界");
        return result;
    }

    internal static void CheckChain(string path, string boundary)
    {
        var full = Path.GetFullPath(path);
        var basePath = Path.TrimEndingDirectorySeparator(Path.GetFullPath(boundary));
        if (!string.Equals(full, basePath, StringComparison.OrdinalIgnoreCase) &&
            !full.StartsWith(basePath + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new UnauthorizedAccessException("安装路径越界");
        var cursor = full;
        while (true)
        {
            FileSystemInfo item = Directory.Exists(cursor) ? new DirectoryInfo(cursor) : new FileInfo(cursor);
            if (!item.Exists || (item.Attributes & FileAttributes.ReparsePoint) != 0)
                throw new UnauthorizedAccessException("助手路径不存在或经过重解析点：" + cursor);
            FileSystemSecurity acl = item is DirectoryInfo directory ? directory.GetAccessControl() : ((FileInfo)item).GetAccessControl();
            CheckAcl(acl, cursor);
            if (string.Equals(cursor, basePath, StringComparison.OrdinalIgnoreCase)) return;
            cursor = Path.GetDirectoryName(cursor) ?? throw new UnauthorizedAccessException("无法验证安装路径");
        }
    }

    internal static void CheckAcl(FileSystemSecurity acl, string path)
    {
        if (acl.GetOwner(typeof(SecurityIdentifier)) is not SecurityIdentifier owner || !TrustedWriter(owner))
            throw new UnauthorizedAccessException("助手文件所有者不受保护：" + path);
        foreach (FileSystemAccessRule rule in acl.GetAccessRules(true, true, typeof(SecurityIdentifier)))
        {
            if (rule.AccessControlType != AccessControlType.Allow ||
                (rule.PropagationFlags & PropagationFlags.InheritOnly) != 0 || (rule.FileSystemRights & WriteRights) == 0) continue;
            if (!TrustedWriter((SecurityIdentifier)rule.IdentityReference))
                throw new UnauthorizedAccessException("助手路径允许普通用户修改：" + path);
        }
    }

    internal static void VerifyManifest(string root, IReadOnlyDictionary<string, string> pinned)
    {
        var manifestPath = Path.Combine(root, "runtime-manifest.json");
        CheckChain(manifestPath, root);
        using var doc = JsonDocument.Parse(File.ReadAllText(manifestPath));
        if (doc.RootElement.GetProperty("schema").GetString() != "xiatiao.runtime.v1")
            throw new InvalidDataException("无线助手安装清单版本不匹配");
        var members = doc.RootElement.GetProperty("files");
        if (members.ValueKind != JsonValueKind.Object) throw new InvalidDataException("无线助手安装清单无文件");
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var member in members.EnumerateObject())
        {
            var name = member.Name.Replace('\\', '/');
            if (!seen.Add(name)) throw new InvalidDataException("无线助手安装清单有重复文件");
            var path = MemberPath(root, name);
            CheckChain(path, root);
            var expected = member.Value.GetString() ?? "";
            if (expected.Length != 64 || !expected.All(Uri.IsHexDigit) || Hash(path) != expected.ToLowerInvariant())
                throw new IOException("安装文件校验失败：" + name);
            if (pinned.TryGetValue(name, out var fixedHash) && expected.ToLowerInvariant() != fixedHash)
                throw new IOException("无线组件与已验证版本不同：" + name);
        }
        foreach (var required in pinned.Keys.Concat(new[] { "XiatiaoAirDrop.exe", "XiatiaoAirDrop.dll", "python/python.exe", "receiver/engine.py" }))
            if (!seen.Contains(required)) throw new InvalidDataException("安装清单缺少必需文件：" + required);
        // An unlisted Python/DLL module must not enter the installed execution chain.
        foreach (var folder in new[] { "runtime", "firmware", "python", "receiver" })
            foreach (var path in Directory.Exists(Path.Combine(root, folder))
                ? Directory.EnumerateFileSystemEntries(Path.Combine(root, folder), "*", SearchOption.AllDirectories) : [])
            {
                CheckChain(path, root);
                if (File.Exists(path) && !seen.Contains(Path.GetRelativePath(root, path).Replace('\\', '/')))
                    throw new IOException("安装目录包含未登记文件：" + Path.GetRelativePath(root, path));
            }
    }
}

internal static class StopDecision
{
    internal static bool Confirmed(bool processes, bool marker, bool driver, bool adapters, bool rules) =>
        processes && marker && driver && adapters && rules;
}

internal sealed class RadioSession(Func<string, string, Task> say) : IDisposable
{
    private readonly SemaphoreSlim _gate = new(1, 1);
    private CancellationTokenSource? _starting;
    private DaemonProcess? _radio, _awdl;
    private SafeFileHandle? _job;
    private readonly List<string> _ownedRules = [];
    private string? _sessionFolder;
    private bool _running;
    private bool _launchAttempted;
    public bool Faulted { get; private set; }
    private static readonly string[] Rules = ["XiatiaoAirDrop-TCP", "XiatiaoAirDrop-mDNS"];

    public async Task StartAsync()
    {
        // Repeated UI requests must not queue a second START behind a pending STOP.
        if (!await _gate.WaitAsync(0)) return;
        var stage = "开启无线连接";
        Task ProgressAsync(string next)
        {
            stage = next;
            return say("starting", "正在" + next + "…");
        }
        try
        {
            if (_running) { await say("on", "AirDrop 已开启"); return; }
            if (Faulted) { await say("error", "上次关闭未完整确认，请查看诊断日志"); return; }
            _starting = new CancellationTokenSource();
            _sessionFolder = null;
            _launchAttempted = false;
            var token = _starting.Token;
            await ProgressAsync("检查专用 " + RadioHost.Backend.Name + " 网卡和驱动");
            RadioHost.ValidateInstalledLocation();
            await RadioHost.VerifyDeviceAsync();
            await ProgressAsync("检查无线连接是否被占用");
            if (Process.GetProcessesByName("winawdl_mt7925").Length != 0 || Process.GetProcessesByName("winawdl_radio").Length != 0 || Process.GetProcessesByName("awdld").Length != 0 ||
                await AdapterCountAsync() != 0) throw new IOException("已有无线进程或 awdl0 接口，请先关闭原接收端");
            await ProgressAsync("检查上次连接的清理状态");
            foreach (var rule in Rules)
                if (await RuleExistsAsync(rule)) throw new IOException("无线助手的旧防火墙规则仍存在，请查看诊断日志");
            token.ThrowIfCancellationRequested();
            if (!RadioHost.Backend.IsRealtek)
            {
                var status = RadioStatus.Read();
                if (!status.IsFresh)
                {
                if (!status.IsStopped) throw new IOException("专用网卡没有确认停止，未尝试重新启动");
                // The experimental driver permits one START per generation. This restarts only that PnP instance.
                await ProgressAsync("重新初始化专用 MT7925 网卡");
                await SystemCommand.RunAsync(Path.Combine(Environment.SystemDirectory, "pnputil.exe"),
                    ["/restart-device", RadioHost.Instance], TimeSpan.FromSeconds(65));
                await ProgressAsync("确认专用网卡已重新就绪");
                await RadioHost.VerifyDeviceAsync();
                if (!RadioStatus.Read().IsFresh) throw new IOException("专用网卡重新初始化未完成");
                }
            }
            token.ThrowIfCancellationRequested();
            await ProgressAsync("启动专用网卡固件");
            _sessionFolder = Path.Combine(RadioHost.LogsFolder, DateTime.UtcNow.ToString("yyyyMMddTHHmmssfffZ") + "-" + Guid.NewGuid().ToString("N")[..8]);
            Directory.CreateDirectory(_sessionFolder);
            ProtectedFiles.CheckChain(_sessionFolder, RadioHost.LogsFolder);
            _job = Native.KillOnCloseJob();
            _launchAttempted = true;
            _radio = DaemonProcess.Launch("radio", Path.Combine(RadioHost.InstallFolder, "runtime", RadioHost.Backend.Executable),
                RadioHost.Backend.RadioArguments(RadioHost.InstallFolder), _sessionFolder, _job);
            await _radio.WaitForAsync(RadioHost.Backend.ReadyMarker, TimeSpan.FromSeconds(65), token);
            await ProgressAsync("建立 AirDrop 网络接口");
            _awdl = DaemonProcess.Launch("awdl", Path.Combine(RadioHost.InstallFolder, "runtime", "awdld.exe"),
                RadioHost.Backend.AwdlArguments, _sessionFolder, _job);
            await _awdl.WaitForAsync("host interface awdl0, index ", TimeSpan.FromSeconds(35), token);
            await ProgressAsync("确认 AirDrop 网络接口已就绪");
            if (await AdapterCountAsync() != 1) throw new IOException("没有找到唯一的 AirDrop 网络接口");
            for (var index = 0; index < Rules.Length; index++)
            {
                token.ThrowIfCancellationRequested();
                await ProgressAsync(index == 0 ? "配置并核验文件传输通道" : "配置并核验设备发现通道");
                var name = Rules[index];
                if (await RuleExistsAsync(name)) throw new IOException("无线助手规则已被其他进程占用");
                _ownedRules.Add(name);
                await AddRuleAsync(name, index == 0 ? "TCP" : "UDP", index == 0 ? 8771 : 5353);
            }
            token.ThrowIfCancellationRequested();
            _running = true;
            await say("on", "AirDrop 已开启");
            _ = MonitorAsync(_radio, _awdl);
        }
        catch (OperationCanceledException) { await CleanupAsync(); }
        catch (Exception error)
        {
            var cleaned = await CleanupAsync();
            await say("error", stage + "失败：" + error.Message + (cleaned ? "" : "；停止未完整确认"));
        }
        finally { _gate.Release(); }
    }

    private async Task MonitorAsync(DaemonProcess radio, DaemonProcess awdl)
    {
        await Task.WhenAny(radio.Process.WaitForExitAsync(), awdl.Process.WaitForExitAsync());
        await _gate.WaitAsync();
        try
        {
            if (!_running || !ReferenceEquals(_radio, radio)) return;
            await CleanupAsync();
            Faulted = true;
            await say("error", "无线进程意外结束，AirDrop 已暂停；请查看诊断日志");
        }
        finally { _gate.Release(); }
    }

    public async Task StopAsync()
    {
        _starting?.Cancel();
        await _gate.WaitAsync();
        try
        {
            if (_radio is null && _awdl is null && _ownedRules.Count == 0)
            { await say(Faulted ? "error" : "off", Faulted ? "停止未完整确认，请查看诊断日志" : "AirDrop 已关闭"); return; }
            await say("stopping", "正在停止无线连接…");
            var clean = await CleanupAsync();
            await say(clean ? "off" : "error", clean ? "AirDrop 已关闭" : "停止未完整确认，请查看诊断日志");
        }
        finally { _gate.Release(); }
    }

    private async Task<bool> CleanupAsync()
    {
        _running = false;
        var radio = _radio;
        var awdl = _awdl;
        if (radio is null && awdl is null && !_launchAttempted && _ownedRules.Count == 0) return true;
        var notes = new List<string>();
        var processOk = true;
        if (awdl is not null && !awdl.Process.HasExited) Native.Interrupt(awdl.Process);
        if (radio is not null && !radio.Process.HasExited)
        {
            try { await radio.Process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(2)); }
            catch (TimeoutException) { Native.Interrupt(radio.Process); }
        }
        foreach (var daemon in new[] { radio, awdl })
        {
            if (daemon is null) continue;
            try { await daemon.Process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(25)); }
            catch (TimeoutException)
            {
                processOk = false; notes.Add(daemon.Name + " graceful stop timed out; forced termination is a fault");
                try { daemon.Process.Kill(entireProcessTree: true); await daemon.Process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5)); }
                catch (Exception e) when (e is Win32Exception or InvalidOperationException or TimeoutException) { notes.Add(e.Message); }
            }
            if (!daemon.Process.HasExited || daemon.Process.ExitCode != 0) processOk = false;
            if (daemon.Process.HasExited) daemon.Process.WaitForExit(); // Flush redirected line callbacks.
        }
        var rulesOk = true;
        foreach (var rule in _ownedRules.ToArray())
        {
            try
            {
                await SystemCommand.PowerShellAsync("Get-NetFirewallRule -PolicyStore PersistentStore -Name '" + rule +
                    "' -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction Stop");
                if (await RuleExistsAsync(rule)) throw new IOException("防火墙规则仍存在：" + rule);
                _ownedRules.Remove(rule);
            }
            catch (Exception e) when (e is IOException or TimeoutException or Win32Exception) { rulesOk = false; notes.Add(e.Message); }
        }
        var adapterOk = false;
        var driverOk = !_launchAttempted;
        try { adapterOk = await AdapterCountAsync() == 0; }
        catch (Exception e) when (e is IOException or JsonException or TimeoutException or Win32Exception) { notes.Add(e.Message); }
        if (_launchAttempted)
        {
            try
            {
                if (RadioHost.Backend.IsRealtek) { await RadioHost.VerifyDeviceAsync(); driverOk = true; }
                else driverOk = RadioStatus.Read().IsStopped;
            }
            catch (Exception e) when (e is IOException or InvalidDataException or Win32Exception or TimeoutException or JsonException or UnauthorizedAccessException) { notes.Add(e.Message); }
        }
        var marker = !_launchAttempted || radio is not null && radio.Saw(RadioHost.StopMarker);
        var ready = !_launchAttempted || radio is not null && radio.Saw(RadioHost.Backend.ReadyMarker);
        var clean = RadioHost.Backend.StopConfirmed(processOk, ready, marker, driverOk, adapterOk, rulesOk);
        Faulted |= !clean;
        if (_sessionFolder is not null)
        {
            try
            {
                File.WriteAllText(Path.Combine(_sessionFolder, "stop.json"), JsonSerializer.Serialize(new
                { utc = DateTime.UtcNow, backend = RadioHost.Backend.Id, clean, processOk, ready, marker, driverOk, adapterOk, rulesOk,
                    hardwareQuiescenceReadback = !RadioHost.Backend.IsRealtek, notes }, new JsonSerializerOptions { WriteIndented = true }));
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { clean = false; Faulted = true; }
        }
        radio?.Dispose(); awdl?.Dispose();
        _radio = _awdl = null;
        _job?.Dispose(); _job = null;
        _launchAttempted = false;
        return clean;
    }

    private static async Task<int> AdapterCountAsync()
    {
        var result = await SystemCommand.PowerShellAsync("@(Get-NetAdapter -IncludeHidden | Where-Object { $_.Name -eq 'awdl0' -or " +
            "$_.InterfaceGuid -eq '{2BE941EA-A750-450A-BA41-FDD8DF0870B6}' }).Count | ConvertTo-Json -Compress");
        return JsonSerializer.Deserialize<int>(result);
    }

    private static async Task<bool> RuleExistsAsync(string name) => JsonSerializer.Deserialize<bool>(
        await SystemCommand.PowerShellAsync("[bool](Get-NetFirewallRule -PolicyStore PersistentStore -Name '" + name +
            "' -ErrorAction SilentlyContinue) | ConvertTo-Json -Compress"));

    private static async Task AddRuleAsync(string name, string protocol, int port)
    {
        var program = Path.Combine(RadioHost.InstallFolder, "python", "python.exe").Replace("'", "''");
        await SystemCommand.PowerShellAsync("New-NetFirewallRule -PolicyStore PersistentStore -Name '" + name +
            "' -DisplayName '虾条 AirDrop " + protocol + "' -Direction Inbound -Action Allow -Enabled True -Profile Any " +
            "-InterfaceAlias awdl0 -Program '" + program + "' -RemoteAddress fe80::/64 -Protocol " + protocol +
            " -LocalPort " + port + " -EdgeTraversalPolicy Block -ErrorAction Stop | Out-Null;" +
            "$r=Get-NetFirewallRule -PolicyStore PersistentStore -Name '" + name + "';" +
            "$a=$r|Get-NetFirewallApplicationFilter;$i=$r|Get-NetFirewallInterfaceFilter;" +
            "$n=$r|Get-NetFirewallAddressFilter;$p=$r|Get-NetFirewallPortFilter;" +
            "if($r.Enabled -ne 'True' -or $r.Action -ne 'Allow' -or $r.Direction -ne 'Inbound' -or $r.Profile -ne 'Any' " +
            "-or $r.EdgeTraversalPolicy -ne 'Block' -or $a.Program -ne '" + program +
            "' -or @($i.InterfaceAlias).Count -ne 1 -or $i.InterfaceAlias -ne 'awdl0' -or " +
            "@($n.RemoteAddress).Count -ne 1 -or $n.RemoteAddress -notin @('fe80::/64','fe80::/255.255.255.255.255.255.255.255.0.0.0.0.0.0.0.0') " +
            "-or $p.LocalPort -ne '" + port + "' -or $p.Protocol -ne '" + protocol + "'){throw 'Firewall scope verification failed'}");
    }

    public void Dispose()
    {
        _starting?.Cancel();
        _job?.Dispose();
    }
}

internal sealed class DaemonProcess : IDisposable
{
    private readonly HashSet<string> _markers = [];
    private readonly object _gate = new();
    private readonly StreamWriter _log;
    private long _written;
    private bool _disposed;
    public string Name { get; }
    public Process Process { get; }
    private DaemonProcess(string name, Process process, StreamWriter log) { Name = name; Process = process; _log = log; }
    internal bool Saw(string marker) { lock (_gate) return _markers.Contains(marker); }

    internal static DaemonProcess Launch(string name, string executable, string[] arguments, string logs, SafeFileHandle job)
    {
        var info = new ProcessStartInfo(executable)
        {
            UseShellExecute = false, CreateNoWindow = false, WindowStyle = ProcessWindowStyle.Hidden,
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
            WorkingDirectory = Path.GetDirectoryName(executable),
        };
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        var process = new Process { StartInfo = info };
        var log = new StreamWriter(new FileStream(Path.Combine(logs, name + ".log"), FileMode.CreateNew,
            FileAccess.Write, FileShare.Read), new UTF8Encoding(false)) { AutoFlush = true };
        var daemon = new DaemonProcess(name, process, log);
        process.OutputDataReceived += daemon.Record;
        process.ErrorDataReceived += daemon.Record;
        try
        {
            process.Start();
            if (!Native.AssignProcessToJobObject(job, process.Handle))
            {
                Native.Interrupt(process);
                if (!process.WaitForExit(25000)) process.Kill();
                throw new Win32Exception(Marshal.GetLastWin32Error(), "无法约束无线子进程生命周期");
            }
            process.BeginOutputReadLine(); process.BeginErrorReadLine();
            return daemon;
        }
        catch { daemon.Dispose(); throw; }
    }

    private void Record(object sender, DataReceivedEventArgs line)
    {
        if (line.Data is not { } text) return;
        lock (_gate)
        {
            if (_disposed) return;
            foreach (var marker in new[] { RadioBackend.MediaTek.ReadyMarker, RadioBackend.Realtek.ReadyMarker, "host interface awdl0, index ", RadioHost.StopMarker })
                if (text.Contains(marker, StringComparison.Ordinal)) _markers.Add(marker);
            if (_written < 4 * 1024 * 1024)
            {
                _log.WriteLine(text);
                _written += Encoding.UTF8.GetByteCount(text) + 2;
            }
        }
    }

    internal async Task WaitForAsync(string marker, TimeSpan limit, CancellationToken cancellation)
    {
        var clock = Stopwatch.StartNew();
        while (!Saw(marker))
        {
            cancellation.ThrowIfCancellationRequested();
            if (Process.HasExited) throw new IOException(Name + " 启动失败，请查看诊断日志");
            if (clock.Elapsed >= limit) throw new TimeoutException(Name + " 启动超时");
            await Task.Delay(150, cancellation);
        }
    }

    public void Dispose() { lock (_gate) { _disposed = true; _log.Dispose(); } Process.Dispose(); }
}

internal static class SystemCommand
{
    internal static Task<string> PowerShellAsync(string script) => RunAsync(
        Path.Combine(Environment.SystemDirectory, "WindowsPowerShell", "v1.0", "powershell.exe"),
        ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-EncodedCommand",
         Convert.ToBase64String(Encoding.Unicode.GetBytes("$ErrorActionPreference='Stop';" + script))], TimeSpan.FromSeconds(35));

    internal static async Task<string> RunAsync(string executable, string[] arguments, TimeSpan timeout)
    {
        var info = new ProcessStartInfo(executable)
        { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var arg in arguments) info.ArgumentList.Add(arg);
        using var process = Process.Start(info) ?? throw new IOException("无法启动系统工具");
        var output = process.StandardOutput.ReadToEndAsync();
        var errors = process.StandardError.ReadToEndAsync();
        try { await process.WaitForExitAsync().WaitAsync(timeout); }
        catch (TimeoutException) { process.Kill(entireProcessTree: true); throw; }
        var result = await output;
        var error = await errors;
        if (process.ExitCode != 0) throw new IOException(error.Trim().Length > 0 ? error.Trim() : "系统工具执行失败：" + process.ExitCode);
        return result.Trim();
    }
}

internal readonly record struct RadioStatus(uint Result, uint Error, uint StopError, uint Ready,
    uint StartAttempted, uint Stopped, uint Published, uint Quiescent)
{
    internal bool IsFresh => Result == 0 && Error == 0 && StopError == 0 && Ready == 0 && StartAttempted == 0 && Stopped == 0 && Published == 0;
    internal bool IsStopped => Result == 0 && Error == 0 && StopError == 0 && Ready == 0 && Stopped == 1 && Quiescent == 1;
    internal static RadioStatus Read()
    {
        var guid = new Guid("3c9e8b97-7d9b-4db2-913d-da4279250001");
        var list = Native.SetupDiGetClassDevs(ref guid, null, IntPtr.Zero, 0x12);
        if (list == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
        string? path = null;
        try
        {
            for (uint index = 0; index < 16; index++)
            {
                var entry = new Native.DeviceInterface { Size = Marshal.SizeOf<Native.DeviceInterface>() };
                if (!Native.SetupDiEnumDeviceInterfaces(list, IntPtr.Zero, ref guid, index, ref entry))
                {
                    if (Marshal.GetLastWin32Error() == 259) break;
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                }
                Native.SetupDiGetDeviceInterfaceDetail(list, ref entry, IntPtr.Zero, 0, out var bytes, IntPtr.Zero);
                if (bytes < 8 || bytes > 65536) throw new IOException("网卡接口长度无效");
                var memory = Marshal.AllocHGlobal(checked((int)bytes));
                try
                {
                    Marshal.WriteInt32(memory, IntPtr.Size == 8 ? 8 : 6);
                    if (!Native.SetupDiGetDeviceInterfaceDetail(list, ref entry, memory, bytes, out _, IntPtr.Zero))
                        throw new Win32Exception(Marshal.GetLastWin32Error());
                    if (path is not null) throw new IOException("存在多个实验网卡接口，未启动无线");
                    path = Marshal.PtrToStringUni(memory + 4);
                }
                finally { Marshal.FreeHGlobal(memory); }
            }
        }
        finally { Native.SetupDiDestroyDeviceInfoList(list); }
        if (path is null) throw new IOException("没有找到已安装的 MT7925 实验驱动");
        using var handle = Native.CreateFile(path, 0x80000000, 0, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
        var data = new byte[96];
        if (!Native.DeviceIoControl(handle, (0x22u << 16) | (1u << 14) | (0x806u << 2), IntPtr.Zero, 0,
            data, 96, out var returned, IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error());
        uint At(int offset) => BitConverter.ToUInt32(data, offset * 4);
        if (returned != 96 || At(0) != 96 || At(1) != 1) throw new IOException("实验驱动状态格式不匹配");
        return new RadioStatus(At(2), At(3), At(4), At(5), At(6), At(7), At(9), At(10));
    }
}

internal static class Native
{
    [DllImport("kernel32.dll", SetLastError = true)] private static extern SafeProcessHandle OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", EntryPoint = "QueryFullProcessImageNameW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool QueryFullProcessImageName(SafeProcessHandle process, uint flags, StringBuilder name, ref uint length);
    internal static string ProcessImage(int pid)
    {
        // Limited query works from the ordinary client to its elevated helper; MainModule needs stronger access.
        using var process = OpenProcess(0x1000, false, pid);
        var name = new StringBuilder(32768); uint length = 32768;
        if (process.IsInvalid || !QueryFullProcessImageName(process, 0, name, ref length))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "无法验证无线助手路径");
        return name.ToString();
    }
    [StructLayout(LayoutKind.Sequential)] internal struct DeviceInterface { public int Size; public Guid Guid; public int Flags; public UIntPtr Reserved; }
    [DllImport("setupapi.dll", EntryPoint = "SetupDiGetClassDevsW", CharSet = CharSet.Unicode, SetLastError = true)]
    internal static extern IntPtr SetupDiGetClassDevs(ref Guid guid, string? enumerator, IntPtr window, uint flags);
    [DllImport("setupapi.dll", SetLastError = true)] internal static extern bool SetupDiEnumDeviceInterfaces(IntPtr list, IntPtr device, ref Guid guid, uint index, ref DeviceInterface entry);
    [DllImport("setupapi.dll", EntryPoint = "SetupDiGetDeviceInterfaceDetailW", SetLastError = true)] internal static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr list, ref DeviceInterface entry, IntPtr detail, uint size, out uint needed, IntPtr device);
    [DllImport("setupapi.dll")] internal static extern bool SetupDiDestroyDeviceInfoList(IntPtr list);
    [DllImport("kernel32.dll", EntryPoint = "CreateFileW", CharSet = CharSet.Unicode, SetLastError = true)] internal static extern SafeFileHandle CreateFile(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)] internal static extern bool DeviceIoControl(SafeFileHandle handle, uint code, IntPtr input, uint inputBytes, byte[] output, uint capacity, out uint length, IntPtr overlap);
    [DllImport("kernel32.dll", SetLastError = true)] internal static extern bool GetNamedPipeClientProcessId(SafePipeHandle pipe, out uint processId);
    [DllImport("kernel32.dll", SetLastError = true)] internal static extern bool AttachConsole(int pid);
    [DllImport("kernel32.dll")] private static extern bool FreeConsole();
    [DllImport("kernel32.dll")] private static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GenerateConsoleCtrlEvent(uint ctrl, uint group);
    [DllImport("kernel32.dll", EntryPoint = "CreateJobObjectW", CharSet = CharSet.Unicode, SetLastError = true)] private static extern SafeFileHandle CreateJobObject(IntPtr security, string? name);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool SetInformationJobObject(SafeFileHandle job, int kind, ref ExtendedLimits info, int length);
    [DllImport("kernel32.dll", SetLastError = true)] internal static extern bool AssignProcessToJobObject(SafeFileHandle job, IntPtr process);
    [StructLayout(LayoutKind.Sequential)] private struct BasicLimits { public long ProcessTime, JobTime; public uint Flags; public UIntPtr MinWorkingSet, MaxWorkingSet; public uint Active; public UIntPtr Affinity; public uint Priority, Scheduling; }
    [StructLayout(LayoutKind.Sequential)] private struct IoCounters { public ulong ReadOps, WriteOps, OtherOps, ReadBytes, WriteBytes, OtherBytes; }
    [StructLayout(LayoutKind.Sequential)] private struct ExtendedLimits { public BasicLimits Basic; public IoCounters Io; public UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory; }
    internal static SafeFileHandle KillOnCloseJob()
    {
        var job = CreateJobObject(IntPtr.Zero, null);
        var limits = new ExtendedLimits { Basic = new BasicLimits { Flags = 0x2000 } };
        if (job.IsInvalid || !SetInformationJobObject(job, 9, ref limits, Marshal.SizeOf<ExtendedLimits>()))
        { job.Dispose(); throw new Win32Exception(Marshal.GetLastWin32Error(), "无法创建无线进程监督组"); }
        return job;
    }
    internal static void Interrupt(Process process)
    {
        if (process.HasExited || !AttachConsole(process.Id)) return;
        SetConsoleCtrlHandler(IntPtr.Zero, true);
        GenerateConsoleCtrlEvent(0, 0);
        FreeConsole();
        Thread.Sleep(200);
        SetConsoleCtrlHandler(IntPtr.Zero, false);
    }
}
