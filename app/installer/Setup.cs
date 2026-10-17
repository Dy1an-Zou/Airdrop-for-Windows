// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dy1an-Zou
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;

[assembly: AssemblyTitle("Airdrop for Windows Setup")]
[assembly: AssemblyProduct("Airdrop for Windows")]
[assembly: AssemblyCompany("Dy1an-Zou")]
[assembly: AssemblyVersion("0.2.4.0")]
[assembly: AssemblyFileVersion("0.2.4.0")]

internal static class Setup
{
    internal const string Version = "0.2.4";
    internal static readonly JavaScriptSerializer Json = new JavaScriptSerializer { MaxJsonLength = 8 * 1024 * 1024 };
    internal static readonly string SetupData = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "AirdropForWindowsSetup");
    internal static string Self { get { return Assembly.GetExecutingAssembly().Location; } }
    internal static byte[] Resource(string name)
    {
        using (var input = Assembly.GetExecutingAssembly().GetManifestResourceStream(name))
        using (var output = new MemoryStream())
        {
            if (input == null) throw new InvalidDataException("Missing installer resource: " + name);
            input.CopyTo(output); return output.ToArray();
        }
    }
    internal static string Hash(byte[] bytes)
    { using (var hash = SHA256.Create()) return BitConverter.ToString(hash.ComputeHash(bytes)).Replace("-", "").ToLowerInvariant(); }
    internal static string ManifestHash { get { return Encoding.UTF8.GetString(Resource("manifest.sha256")).Trim(); } }
    internal static string Quote(string value)
    {
        if (value.IndexOf('"') >= 0 || value.IndexOf('\n') >= 0 || value.IndexOf('\r') >= 0) throw new ArgumentException("Invalid argument");
        return "\"" + value + "\"";
    }
    internal static void NoLinks(string path)
    {
        for (var cursor = Path.GetFullPath(path); !string.IsNullOrEmpty(cursor); cursor = Path.GetDirectoryName(cursor))
            if ((File.Exists(cursor) || Directory.Exists(cursor)) && (File.GetAttributes(cursor) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Linked paths are not supported: " + cursor);
    }
    internal static void ProtectDirectory(string path)
    {
        NoLinks(path);
        var acl = new DirectorySecurity();
        var admins = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
        acl.SetOwner(admins); acl.SetAccessRuleProtection(true, false);
        foreach (var sid in new[] { admins, new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null) })
            acl.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl, InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
        acl.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null), FileSystemRights.ReadAndExecute, InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
        if (!Directory.Exists(path)) Directory.CreateDirectory(path, acl);
        else
        {
            var actual = Directory.GetAccessControl(path);
            var owner = (SecurityIdentifier)actual.GetOwner(typeof(SecurityIdentifier));
            if (!owner.IsWellKnown(WellKnownSidType.BuiltinAdministratorsSid) && !owner.IsWellKnown(WellKnownSidType.LocalSystemSid))
                throw new IOException("Setup data directory is not administrator-owned.");
            foreach (FileSystemAccessRule rule in actual.GetAccessRules(true, true, typeof(SecurityIdentifier)))
                if (rule.AccessControlType == AccessControlType.Allow &&
                    (rule.FileSystemRights & (FileSystemRights.Write | FileSystemRights.Delete | FileSystemRights.ChangePermissions | FileSystemRights.TakeOwnership)) != 0 &&
                    !((SecurityIdentifier)rule.IdentityReference).IsWellKnown(WellKnownSidType.BuiltinAdministratorsSid) &&
                    !((SecurityIdentifier)rule.IdentityReference).IsWellKnown(WellKnownSidType.LocalSystemSid))
                    throw new IOException("Setup data directory is writable by an untrusted account.");
        }
    }
    internal static Dictionary<string, object> VerifyPayload(string destination)
    {
        using (var data = new MemoryStream(Resource("payload.zip")))
        using (var zip = new ZipArchive(data, ZipArchiveMode.Read))
        {
            var manifestEntry = zip.GetEntry("runtime-manifest.json");
            if (manifestEntry == null) throw new InvalidDataException("Payload manifest missing");
            byte[] manifest;
            using (var stream = manifestEntry.Open()) using (var memory = new MemoryStream()) { stream.CopyTo(memory); manifest = memory.ToArray(); }
            if (Hash(manifest) != ManifestHash) throw new InvalidDataException("Payload manifest hash mismatch");
            var document = Json.Deserialize<Dictionary<string, object>>(Encoding.UTF8.GetString(manifest).TrimStart('\uFEFF'));
            if ((string)document["schema"] != "xiatiao.runtime.v1") throw new InvalidDataException("Unsupported payload manifest");
            var files = (Dictionary<string, object>)document["files"];
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            long total = 0;
            foreach (var item in zip.Entries)
            {
                var name = item.FullName;
                if (name.Contains("\\") || name.Contains(":") || name.StartsWith("/") || name.Split('/').Any(p => p == ".." || p == "." || p.Length == 0) || !seen.Add(name))
                    throw new InvalidDataException("Unsafe or duplicate payload path");
                if (item.Length > 128 * 1024 * 1024 || (total += item.Length) > 512 * 1024 * 1024) throw new InvalidDataException("Payload size exceeds limit");
                byte[] bytes;
                using (var stream = item.Open()) using (var memory = new MemoryStream()) { stream.CopyTo(memory); bytes = memory.ToArray(); }
                if (name != "runtime-manifest.json" && (!files.ContainsKey(name) || Hash(bytes) != (string)files[name])) throw new InvalidDataException("Payload file mismatch: " + name);
                if (destination != null)
                {
                    var path = Path.GetFullPath(Path.Combine(destination, name.Replace('/', Path.DirectorySeparatorChar)));
                    if (!path.StartsWith(Path.GetFullPath(destination) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Payload path escaped");
                    NoLinks(path); Directory.CreateDirectory(Path.GetDirectoryName(path));
                    using (var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write)) output.Write(bytes, 0, bytes.Length);
                }
            }
            if (seen.Count != files.Count + 1 || files.Keys.Any(k => !seen.Contains(k))) throw new InvalidDataException("Payload incomplete");
            return new Dictionary<string, object> { { "status", "verified" }, { "files", files.Count }, { "manifestSha256", ManifestHash }, { "bytes", total } };
        }
    }
    internal static int Worker(string[] args)
    {
        if (args.Length != 5 || (args[1] != "install" && args[1] != "uninstall")) return 2;
        Guid id; if (!Guid.TryParseExact(args[3], "N", out id) || (args[4] != "preserve" && args[4] != "enabled" && args[4] != "disabled")) return 2;
        if (!new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator) || WindowsIdentity.GetCurrent().User.Value != args[2]) return 3;
        string work = null;
        try
        {
            ProtectDirectory(SetupData);
            work = Path.Combine(SetupData, id.ToString("N"));
            if (Directory.Exists(work) || File.Exists(work)) throw new IOException("Setup job already exists");
            ProtectDirectory(work);
            var script = Path.Combine(work, "setup-backend.ps1"); File.WriteAllBytes(script, Resource("setup-backend.ps1"));
            string payload = Path.Combine(work, "payload");
            if (args[1] == "install") { ProtectDirectory(payload); VerifyPayload(payload); }
            var result = Path.Combine(work, "result.json");
            var parameters = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + Quote(script) + " -Mode " + args[1] + " -ExpectedOwnerSid " + Quote(args[2]) + " -ExpectedManifestSha256 " + ManifestHash + " -ResultPath " + Quote(result);
            if (args[1] == "install") parameters += " -PackageDir " + Quote(payload) + " -InstallerPath " + Quote(Self) + " -AutoStart " + args[4];
            var info = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), @"WindowsPowerShell\v1.0\powershell.exe"), parameters)
            { UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true };
            int code;
            using (var process = Process.Start(info))
            {
                var output = process.StandardOutput.ReadToEndAsync(); var error = process.StandardError.ReadToEndAsync();
                process.WaitForExit(); code = process.ExitCode;
                File.WriteAllText(Path.Combine(work, "backend.log"), output.Result + Environment.NewLine + error.Result, Encoding.UTF8);
            }
            if (!File.Exists(result)) throw new IOException("Setup returned no result. See " + Path.Combine(work, "backend.log"));
            return code;
        }
        catch (Exception error)
        {
            if (work != null && Directory.Exists(work))
            {
                var path = Path.Combine(work, "result.json");
                if (!File.Exists(path)) File.WriteAllText(path, Json.Serialize(new { status = "failed", error = error.Message }), Encoding.UTF8);
            }
            return 1;
        }
        finally
        {
            // Remove only this job's verified extraction after the backend exits.
            if (work != null)
            {
                var payload = Path.Combine(work, "payload");
                try { NoLinks(payload); if (Directory.Exists(payload)) Directory.Delete(payload, true); } catch { }
            }
        }
    }
    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Length > 0 && args[0] == "--worker") return Worker(args);
        if (args.Length == 2 && args[0] == "--verify")
        { File.WriteAllText(args[1], Json.Serialize(VerifyPayload(null)), Encoding.UTF8); return 0; }
        Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
        if (args.Length == 3 && args[0] == "--render")
        {
            using (var form = new SetupForm(false, args[1] == "en"))
            {
                // Create child handles explicitly: an invisible parent otherwise
                // defers them, leaving an empty DrawToBitmap preview.
                var handle = form.Handle;
                foreach (Control control in form.Controls) { var childHandle = control.Handle; }
                form.PerformLayout();
                using (var bitmap = new Bitmap(form.Width, form.Height))
                { form.DrawToBitmap(bitmap, new Rectangle(Point.Empty, form.Size)); bitmap.Save(args[2]); }
            }
            return 0;
        }
        if (args.Any(a => a != "--uninstall")) return 2;
        Application.Run(new SetupForm(args.Contains("--uninstall"), !System.Globalization.CultureInfo.CurrentUICulture.Name.StartsWith("zh", StringComparison.OrdinalIgnoreCase)));
        return 0;
    }
}

internal sealed class SetupForm : Form
{
    readonly bool uninstall;
    bool english, working;
    readonly Label title = new Label(), description = new Label(), state = new Label(), author = new Label();
    readonly Button chinese = new Button(), englishButton = new Button();
    readonly CheckBox startup = new CheckBox();
    readonly Button action = new Button(), close = new Button();
    readonly ProgressBar progress = new ProgressBar();
    readonly LinkLabel dotnet = new LinkLabel();
    string T(string zh, string en) { return english ? en : zh; }
    public SetupForm(bool uninstallMode, bool useEnglish)
    {
        SuspendLayout();
        AutoScaleDimensions = new SizeF(96, 96); AutoScaleMode = AutoScaleMode.Dpi;
        uninstall = uninstallMode; english = useEnglish;
        Text = "Airdrop for Windows"; ClientSize = new Size(680, 470);
        FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Segoe UI", 10); BackColor = Color.FromArgb(246, 245, 239);
        using (var stream = new MemoryStream(Setup.Resource("app.ico"))) Icon = new Icon(stream);
        var logo = new PictureBox { Location = new Point(28, 30), Size = new Size(68, 68), SizeMode = PictureBoxSizeMode.Zoom };
        using (var stream = new MemoryStream(Setup.Resource("app.png"))) using (var original = Image.FromStream(stream)) logo.Image = new Bitmap(original);
        title.SetBounds(112, 32, 390, 36); title.Font = new Font("Segoe UI", 18, FontStyle.Bold); title.Text = "Airdrop for Windows";
        author.SetBounds(113, 74, 350, 24); author.Text = "0.2.4  ·  Dy1an-Zou"; author.ForeColor = Color.FromArgb(80, 80, 72);
        chinese.SetBounds(510, 40, 60, 30); chinese.Text = "中文"; chinese.AccessibleName = "简体中文";
        englishButton.SetBounds(578, 40, 72, 30); englishButton.Text = "English";
        foreach (var button in new[] { chinese, englishButton })
        { button.FlatStyle = FlatStyle.Flat; button.FlatAppearance.BorderSize = 1; button.UseCompatibleTextRendering = true; }
        chinese.Click += delegate { english = false; RefreshCopy(); };
        englishButton.Click += delegate { english = true; RefreshCopy(); };
        description.SetBounds(30, 130, 620, 112); description.ForeColor = Color.FromArgb(42, 42, 35);
        description.UseCompatibleTextRendering = true;
        startup.SetBounds(30, 251, 610, 28); startup.Checked = true; startup.Visible = !uninstall;
        dotnet.SetBounds(30, 288, 620, 28); dotnet.LinkColor = Color.FromArgb(0, 87, 177);
        dotnet.LinkClicked += delegate { Process.Start(new ProcessStartInfo("https://dotnet.microsoft.com/en-us/download/dotnet/10.0") { UseShellExecute = true }); };
        state.SetBounds(30, 326, 620, 60); state.ForeColor = Color.FromArgb(70, 70, 63); state.UseCompatibleTextRendering = true;
        progress.SetBounds(30, 393, 620, 5); progress.Style = ProgressBarStyle.Marquee; progress.Visible = false;
        action.SetBounds(393, 416, 154, 32); action.BackColor = Color.FromArgb(18, 112, 224); action.ForeColor = Color.White; action.FlatStyle = FlatStyle.Flat; action.FlatAppearance.BorderSize = 0;
        close.SetBounds(558, 416, 92, 32); close.FlatStyle = FlatStyle.System;
        close.Click += delegate { Close(); }; action.Click += async delegate { await ApplyAsync(); };
        FormClosing += delegate(object sender, FormClosingEventArgs e) { if (working) e.Cancel = true; };
        chinese.TabIndex = 0; englishButton.TabIndex = 1; startup.TabIndex = 2; dotnet.TabIndex = 3; action.TabIndex = 4; close.TabIndex = 5;
        Controls.AddRange(new Control[] { logo, title, author, chinese, englishButton, description, startup, dotnet, state, progress, action, close });
        RefreshCopy();
        ResumeLayout(true);
    }
    void RefreshCopy()
    {
        description.Text = uninstall ? T("卸载 Airdrop for Windows。\r\n\r\n接收的文件、个人设置和网卡驱动会保留。\r\n请先从系统托盘退出软件。", "Remove Airdrop for Windows.\r\n\r\nReceived files, preferences and the network driver will be kept.\r\nExit the app from the system tray first.")
            : T("在 Windows 上使用 AirDrop 收发文件。\r\n\r\n适用于与本安装包匹配的专用网卡和实验驱动。\r\n安装前请退出软件；安装程序会检查环境，并保留已有设置与文件。", "Send and receive files with AirDrop on Windows.\r\n\r\nRequires the dedicated adapter and matching experimental\r\ndriver already configured. Exit the app before installing.\r\nSetup checks requirements and keeps your settings and files.");
        foreach (var button in new[] { chinese, englishButton })
        {
            bool selected = (button == englishButton) == english;
            button.BackColor = selected ? Color.FromArgb(18, 112, 224) : BackColor;
            button.ForeColor = selected ? Color.White : Color.FromArgb(42, 42, 35);
            button.FlatAppearance.BorderColor = selected ? Color.FromArgb(18, 112, 224) : Color.FromArgb(160, 159, 150);
        }
        startup.Text = T("登录后自动隐藏启动（更新时保留原设置）", "Start hidden at sign-in (updates preserve your setting)");
        dotnet.Text = T("需要 .NET Desktop Runtime 10 x64 · 打开微软下载页", "Requires .NET Desktop Runtime 10 x64 · Microsoft download");
        state.Text = T("安装位置：C:\\Program Files\\XiatiaoAirDrop", "Install location: C:\\Program Files\\XiatiaoAirDrop");
        action.Text = uninstall ? T("卸载", "Uninstall") : T("安装 / 更新", "Install / Update"); close.Text = T("关闭", "Close");
    }
    async Task ApplyAsync()
    {
        working = true; action.Enabled = false; close.Enabled = false; chinese.Enabled = false; englishButton.Enabled = false; startup.Enabled = false; progress.Visible = true;
        state.Text = T("正在检查并处理，请完成 Windows 管理员确认…", "Checking and applying. Please approve the Windows administrator prompt…");
        try
        {
            string id = Guid.NewGuid().ToString("N"), sid = WindowsIdentity.GetCurrent().User.Value;
            string mode = uninstall ? "uninstall" : "install";
            string auto = Directory.Exists(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "XiatiaoAirDrop")) ? "preserve" : (startup.Checked ? "enabled" : "disabled");
            var info = new ProcessStartInfo(Setup.Self, "--worker " + mode + " " + Setup.Quote(sid) + " " + id + " " + auto)
            { UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden };
            int code = await Task.Run(delegate { using (var process = Process.Start(info)) { process.WaitForExit(); return process.ExitCode; } });
            var path = Path.Combine(Setup.SetupData, id, "result.json");
            if (!File.Exists(path)) throw new IOException(T("未完成安装。请使用当前 Windows 管理员账户确认，并重新尝试。", "Setup did not complete. Approve with your current Windows administrator account and try again."));
            var result = Setup.Json.Deserialize<Dictionary<string, object>>(File.ReadAllText(path));
            string status = result.ContainsKey("status") ? Convert.ToString(result["status"]) : "";
            if (code != 0 || (status != "installed" && status != "updated" && status != "uninstalled"))
                throw new IOException(result.ContainsKey("error") ? Convert.ToString(result["error"]) : "Setup failed. See " + path);
            state.Text = uninstall ? T("卸载完成，个人文件和设置已保留。", "Uninstalled. Your files and preferences have been kept.") : T("安装完成。可从开始菜单打开 Airdrop for Windows。", "Installed. Open Airdrop for Windows from the Start menu.");
            if (result.ContainsKey("setupCleanupPending") && Convert.ToBoolean(result["setupCleanupPending"]))
            {
                state.Text = T("软件已卸载。当前运行的卸载程序文件仍保留，关闭后可删除。", "App removed. The running uninstaller file remains and can be deleted after closing.");
                MessageBox.Show(this, T("个人文件和设置已保留。关闭本窗口后，可删除以下安装器文件夹：\r\n", "Your files and preferences have been kept. After closing this window, you may delete this installer folder:\r\n") + Convert.ToString(result["setupFolder"]), "Airdrop for Windows", MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            else if (result.ContainsKey("cleanupWarning") && !string.IsNullOrWhiteSpace(Convert.ToString(result["cleanupWarning"])))
                MessageBox.Show(this, Convert.ToString(result["cleanupWarning"]), T("安装完成，清理提示", "Setup complete — cleanup notice"), MessageBoxButtons.OK, MessageBoxIcon.Information);
            action.Visible = false;
        }
        catch (System.ComponentModel.Win32Exception error) { state.Text = error.NativeErrorCode == 1223 ? T("已取消管理员确认，未执行安装。", "Administrator approval cancelled. No installation was performed.") : error.Message; }
        catch (Exception error) { state.Text = error.Message; MessageBox.Show(this, error.Message, "Airdrop for Windows", MessageBoxButtons.OK, MessageBoxIcon.Information); }
        finally { working = false; action.Enabled = true; close.Enabled = true; chinese.Enabled = true; englishButton.Enabled = true; startup.Enabled = true; progress.Visible = false; }
    }
}
