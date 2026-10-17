// SPDX-License-Identifier: MIT
using System.IO;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace Winawdl.Xiatiao;

internal static class AppChecks
{
    private static string Output(string[] args, string mode)
    {
        var index = Array.IndexOf(args, mode);
        if (index < 0 || index + 1 >= args.Length) throw new ArgumentException("需要证据输出目录");
        var path = Path.GetFullPath(args[index + 1]); Directory.CreateDirectory(path); return path;
    }
    public static int Run(string[] args)
    {
        var folder = Output(args, "--xiatiao-check"); var results = new List<string>();
        try
        {
            var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
            LanguageChecks.Check(folder, results);
            var receive = Path.Combine(folder, "files"); Directory.CreateDirectory(receive);
            foreach (var bad in new[] { "../outside", "C:\\escape", "file.txt:payload", "\\\\host\\share\\x" })
            {
                try { Store.ReceivedPath(receive, bad); throw new Exception("路径逃逸未被拦截：" + bad); }
                catch (IOException) { results.Add("reject path " + bad); }
            }
            var payload = Path.Combine(receive, "received.txt"); File.WriteAllText(payload, "received payload");
            using var model = new AppController(true);
            using (var asked = JsonDocument.Parse("{\"event\":\"asked\",\"id\":\"r1\",\"sender\":\"iPhone\",\"files\":[{\"name\":\"received.txt\"}]}")) model.HandleEvent(asked.RootElement);
            if (model.Pending.Count != 1 || !model.Pending[0].IsWaiting) throw new Exception("来件确认没有保留等待状态");
            results.Add("incoming requires explicit accept");
            using (var done = JsonDocument.Parse(JsonSerializer.Serialize(new { @event = "stored", id = "r1", folder = receive, names = new[] { "received.txt" } }))) model.HandleEvent(done.RootElement);
            if (model.Pending.Count != 0 || model.Files.Count != 1 || model.Files[0].Bytes != new FileInfo(payload).Length) throw new Exception("已接收文件未正确入库");
            var history = Path.Combine(folder, "history.json"); Store.Write(history, model.Files.ToArray());
            if (Store.Read(history, Array.Empty<ReceivedItem>()).Single().FullPath != payload) throw new Exception("历史记录持久化失败");
            results.Add("stored event preserves file and history roundtrip");
            model.ClearHistoryCommand.Execute(null);
            if (model.Files.Count != 0 || !File.Exists(payload)) throw new Exception("清除记录误删了文件"); results.Add("clear history preserves received files");
            ReceiverProcess.EnsureIdentity(Path.Combine(folder, "keys")); results.Add("per-user certificate generated without openssl");
            CheckSending(folder, results);
            CheckFileActions(folder, results);
            CheckTransfer(folder, results);
            Store.Write(Path.Combine(folder, "checks.json"), new { passed = true, checks = results, radioStarted = false }); return 0;
        }
        catch (Exception error) { Store.Write(Path.Combine(folder, "checks.json"), new { passed = false, checks = results, error = error.ToString() }); return 1; }
    }
    private static void CheckFileActions(string folder, List<string> results)
    {
        var root = Path.Combine(folder, "file-action-fixtures"); Directory.CreateDirectory(root);
        var src = Path.Combine(root, "original.txt"); File.WriteAllText(src, "retain original");
        var target = Path.Combine(root, "target"); Directory.CreateDirectory(target);
        File.WriteAllText(Path.Combine(target, "original.txt"), "retain destination");
        var copy = ReceivedFileActions.CopyTo(src, target);
        if (copy == src || File.ReadAllText(copy) != "retain original" || File.ReadAllText(Path.Combine(target, "original.txt")) != "retain destination") throw new Exception("复制覆盖了已有内容");
        var sink = new RecycleFiles.RecycleSink();
        if (sink.PreDeleteItem(0, IntPtr.Zero) >= 0 || !sink.PermanentDeleteBlocked) throw new Exception("永久删除回退未拒绝");
        var disposable = Path.Combine(root, "recycle-fixture-" + Guid.NewGuid().ToString("N") + ".txt"); File.WriteAllText(disposable, "disposable offline check");
        RecycleFiles.Move(disposable, IntPtr.Zero);
        if (File.Exists(disposable) || !File.Exists(src)) throw new Exception("回收站操作未正确处理专用测试文件");
        results.Add("copy preserves source and existing destination; recycle COM accepted disposable fixture; permanent fallback vetoed");
    }
    private static void CheckSending(string folder, List<string> results)
    {
        var commands = new List<JsonElement>(); var notifications = new List<string>();
        var sender = new SenderController(value => commands.Add(JsonSerializer.SerializeToElement(value)), (title, _) => notifications.Add(title));
        void Event(object value) => sender.HandleEvent(JsonSerializer.SerializeToElement(value));
        var first = Path.Combine(folder, "outgoing.txt"); File.WriteAllText(first, "test sender selection");
        sender.AddFiles(new[] { first, first });
        if (sender.Files.Count != 1 || sender.CanSend) throw new Exception("发送选择或就绪门控失败");
        sender.BeginLooking(); if (commands.Count != 0) throw new Exception("无线尚未就绪时发起了搜索");
        sender.SetReady(true);
        Event(new { @event = "peer", id = "peer-1", name = "Local test", model = "Mac" });
        if (sender.Peers.Count != 1 || sender.CanSend) throw new Exception("发现设备后不应自动发送或选择");
        sender.SelectedPeer = new ApplePeer { Id = "untrusted", Name = "not discovered" };
        if (sender.CanSend) throw new Exception("未发现的设备可被发送");
        sender.SelectedPeer = sender.Peers[0]; sender.SendCommand.Execute(null);
        var send = commands.Last(); var id = send.GetProperty("id").GetString();
        Event(new { @event = "send", id = "other", state = "sent", done = 100, total = 100 });
        if (!sender.IsSending || notifications.Count != 0) throw new Exception("其他传输事件错误结束当前发送");
        Event(new { @event = "send", id, state = "sending", done = 50, total = 100 });
        if (sender.Progress != 50 || sender.CanEditFiles) throw new Exception("真实进度或发送中编辑门控失败");
        Event(new { @event = "send", id, state = "finishing", done = 100, total = 100 });
        if (!sender.IsSending || notifications.Count != 0) throw new Exception("上传完成被误当作接收完成");
        Event(new { @event = "send", id, state = "sent", done = 100, total = 100 });
        if (sender.IsSending || notifications.Count != 1) throw new Exception("确认完成后未正确结束发送");
        results.Add("sender requires selected discovered peer; ignores foreign events; upload is not completion");
        sender.SendCommand.Execute(null); id = commands.Last().GetProperty("id").GetString(); sender.CancelCommand.Execute(null);
        Event(new { @event = "send", id, state = "sending", done = 70, total = 100 });
        if (sender.StatusText != "正在取消") throw new Exception("迟到进度覆盖了取消意图");
        Event(new { @event = "send", id, state = "cancelled", done = (int?)null, total = 100 });
        if (sender.IsSending || notifications.Count != 1) throw new Exception("取消错误报告成功");
        sender.SendCommand.Execute(null); sender.SetReady(false);
        if (sender.IsSending || sender.CanSend || sender.Peers.Count != 0) throw new Exception("无线结束后保留了可发送状态");
        results.Add("cancel suppresses late progress; null byte counts accepted; radio loss clears stale peers");
        var duplicateDir = Path.Combine(folder, "duplicate"); Directory.CreateDirectory(duplicateDir);
        var second = Path.Combine(duplicateDir, "outgoing.txt"); File.WriteAllText(second, "second");
        try { sender.AddFiles(new[] { second }); throw new Exception("重名发送未拒绝"); }
        catch (IOException) { }
        if (sender.Files.Count != 1) throw new Exception("无效选择破坏了原文件列表");
        results.Add("duplicate outgoing names rejected atomically");
    }
    private static void CheckTransfer(string folder, List<string> results)
    {
        var receive = Path.Combine(folder, "loopback-received"); Directory.CreateDirectory(receive);
        var data = Path.Combine(folder, "loopback-state");
        var source = Path.Combine(folder, "loopback-payload.txt");
        File.WriteAllText(source, "Airdrop for Windows真实 HTTPS/CPIO 回环接收验证 " + Guid.NewGuid());
        var events = new BlockingCollection<JsonElement>();
        using var receiver = new ReceiverProcess();
        receiver.Event += events.Add;
        receiver.Failed += text => events.Add(JsonSerializer.SerializeToElement(new { @event = "failed", reason = text }));
        using var model = new AppController(true);
        JsonElement Wait(string expected)
        {
            var limit = DateTime.UtcNow.AddSeconds(25);
            while (DateTime.UtcNow < limit)
            {
                if (!events.TryTake(out var message, 500)) continue;
                model.HandleEvent(message);
                var type = message.GetProperty("event").GetString();
                if (type == expected) return message;
                if (type is "fatal" or "failed" or "lost") throw new IOException(message.ToString());
            }
            throw new TimeoutException("没有收到接收服务事件：" + expected);
        }
        receiver.Start("虾条回环验证", receive, data, loopback: true, port: 0);
        Process? sender = null;
        try
        {
            var ready = Wait("receiving");
            if (!ready.GetProperty("on").GetBoolean()) throw new IOException(ready.ToString());
            var info = new ProcessStartInfo(Path.Combine(AppContext.BaseDirectory, "python", "python.exe"))
            { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
            foreach (var argument in new[] { "-B", "-c", "import sender,sys; result=sender.deliver('::1',int(sys.argv[1]),0,sender.client_context(sys.argv[2]),'Local check',sender.outgoing([sys.argv[3]])); print(result); sys.exit(0 if result==sender.SENT else 1)", ready.GetProperty("port").GetInt32().ToString(), Path.Combine(data, "keys"), source }) info.ArgumentList.Add(argument);
            sender = Process.Start(info) ?? throw new IOException("测试发送进程未启动");
            var stdout = sender.StandardOutput.ReadToEndAsync(); var stderr = sender.StandardError.ReadToEndAsync();
            var asked = Wait("asked");
            if (Directory.GetFiles(receive).Length != 0 || !model.Pending.Single().IsWaiting) throw new Exception("未确认就保存了文件");
            receiver.Send(new { @do = "answer", id = asked.GetProperty("id").GetString(), accept = true });
            Wait("stored");
            if (!sender.WaitForExit(10000)) throw new TimeoutException("测试发送进程未退出");
            if (sender.ExitCode != 0) throw new IOException(stderr.GetAwaiter().GetResult());
            var stored = model.Files.Single();
            if (SHA256.HashData(File.ReadAllBytes(source)).SequenceEqual(SHA256.HashData(File.ReadAllBytes(stored.FullPath))) == false) throw new Exception("实际传输后的文件摘要不一致");
            results.Add("packaged Python engine -> HTTPS/CPIO -> explicit accept -> controller history -> identical SHA256");
            Store.Write(Path.Combine(folder, "loopback.json"), new { passed = true, radioStarted = false, bytes = stored.Bytes, sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(stored.FullPath))).ToLowerInvariant() });
        }
        finally
        {
            if (sender is not null) { if (!sender.HasExited) sender.Kill(true); sender.Dispose(); }
            receiver.StopAsync().GetAwaiter().GetResult();
        }
    }
    public static int Render(string[] args)
    {
        var folder = Output(args, "--xiatiao-render");
        try
        {
            var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
            var languageIndex = Array.IndexOf(args, "--language");
            L.SetLanguage(languageIndex >= 0 && languageIndex + 1 < args.Length ? args[languageIndex + 1] : "zh-CN");
            var bindingErrors = new StringWriter();
            System.Diagnostics.PresentationTraceSources.DataBindingSource.Listeners.Add(new System.Diagnostics.TextWriterTraceListener(bindingErrors));
            using var empty = new AppController(true); empty.Preview(false);
            var blank = new XiatiaoWindow(empty); Save(blank, 860, 900, Path.Combine(folder, "empty.png"));
            CheckSwitchAlignment(blank, folder);
            using var full = new AppController(true); full.Preview(true);
            full.Files.Clear();
            var sample = Path.Combine(folder, L.IsEnglish ? "Xiatiao icon.png" : "虾条图标.png"); File.Copy(Path.Combine(AppContext.BaseDirectory, "assets", "xiatiao-airdrop.png"), sample, true);
            var picture = new ReceivedItem { FullPath = sample, Bytes = new FileInfo(sample).Length, ReceivedAt = DateTimeOffset.Now }; picture.LoadThumbnail(); full.Files.Add(picture);
            var note = Path.Combine(folder, L.IsEnglish ? "Project notes.txt" : "项目资料.txt"); File.WriteAllText(note, "Offline interface preview only"); full.Files.Add(new ReceivedItem { FullPath = note, Bytes = new FileInfo(note).Length, ReceivedAt = DateTimeOffset.Now });
            full.Files.Add(new ReceivedItem { FullPath = Path.Combine(folder, L.IsEnglish ? "Moved reference document.pdf" : "已经移动的参考文件.pdf"), Bytes = 182000, ReceivedAt = DateTimeOffset.Now });
            var window = new XiatiaoWindow(full); Save(window, 860, 900, Path.Combine(folder, "incoming.png"));
            Save(window, 640, 720, Path.Combine(folder, "compact.png"));
            Save(window.CreateAboutWindow(), 470, 620, Path.Combine(folder, "about.png"));
            Save(window.CreateSendWindow(), 680, 740, Path.Combine(folder, "send-empty.png"));
            full.Sender.Preview();
            Save(window.CreateSendWindow(), 680, 740, Path.Combine(folder, "send-ready.png"));
            Save(window.CreateSendWindow(), 560, 650, Path.Combine(folder, "send-compact.png"));
            if (window.FindName("FileList") is System.Windows.Controls.ListBox files) files.SelectAll();
            Save(window, 860, 900, Path.Combine(folder, "selected.png"));
            File.WriteAllText(Path.Combine(folder, "binding-errors.txt"), bindingErrors.ToString());
            Store.Write(Path.Combine(folder, "language-render.json"), new { language = L.Language, radioStarted = false, desktopInputSent = false, userSettingsChanged = false });
            return bindingErrors.ToString().Contains("Error:") ? 1 : 0;
        }
        catch (Exception error) { File.WriteAllText(Path.Combine(folder, "render-error.txt"), error.ToString()); return 1; }
    }
    internal static void Save(Window window, double width, double height, string path)
    {
        // Render the actual WPF view without showing a native window or taking focus.
        window.Width = width; window.Height = height;
        if (window is XiatiaoWindow main) main.ApplyHeight(height);
        var view = (FrameworkElement)window.Content; view.Width = double.NaN; view.Height = double.NaN;
        view.Measure(new Size(width, height)); view.Arrange(new Rect(0, 0, width, height)); view.UpdateLayout();
        var frame = new DispatcherFrame(); Dispatcher.CurrentDispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle, new Action(() => frame.Continue = false)); Dispatcher.PushFrame(frame);
        var image = new RenderTargetBitmap((int)width, (int)height, 96, 96, PixelFormats.Pbgra32);
        var background = new DrawingVisual(); using (var context = background.RenderOpen()) context.DrawRectangle(window.Background, null, new Rect(0, 0, width, height));
        image.Render(background); image.Render(view);
        var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(image)); using var stream = File.Create(path); encoder.Save(stream);
    }

    private static void CheckSwitchAlignment(XiatiaoWindow window, string folder)
    {
        var observations = new List<object>();
        var top = (FrameworkElement)window.FindName("AirDropSwitch");
        var bottom = (FrameworkElement)window.FindName("AutoStartSwitch");
        var settings = (System.Windows.Controls.ScrollViewer)window.FindName("SettingsScroller");
        var expander = (System.Windows.Controls.Button)window.FindName("SettingsButton");
        void Record(string state, bool expanded)
        {
            var view = (Visual)window.Content;
            var topBounds = top.TransformToAncestor(view).TransformBounds(new Rect(top.RenderSize));
            var bottomBounds = bottom.TransformToAncestor(view).TransformBounds(new Rect(bottom.RenderSize));
            var delta = topBounds.Right - bottomBounds.Right;
            observations.Add(new { state, expanded, scrollbar = settings.ComputedVerticalScrollBarVisibility.ToString(),
                topRight = topBounds.Right, bottomRight = bottomBounds.Right, deltaDip = delta });
            if (top.ActualWidth < 50 || (expanded && Math.Abs(delta) > 0.5))
                throw new InvalidOperationException("开关没有沿同一条右边线对齐：" + state + " 差 " + delta);
        }
        Record("expanded-no-scrollbar", true);
        expander.RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Button.ClickEvent));
        Save(window, 640, 720, Path.Combine(folder, "alignment-collapsed.png"));
        Record("collapsed", false);
        expander.RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Button.ClickEvent));
        Save(window, 640, 720, Path.Combine(folder, "alignment-compact.png"));
        Record("compact-expanded-with-scrollbar", true);
        settings.ScrollToEnd();
        Save(window, 640, 720, Path.Combine(folder, "alignment-compact-bottom.png"));
        Record("compact-expanded-scrolled-to-bottom", true);
        settings.ScrollToHome();
        Save(window, 860, 900, Path.Combine(folder, "alignment-normal.png"));
        Record("normal-expanded-restored", true);
        Store.Write(Path.Combine(folder, "switch-alignment.json"), new { passed = true, observations, desktopInputSent = false });
    }
}
