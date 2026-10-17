// SPDX-License-Identifier: MIT
using System.IO;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Windows;

namespace Winawdl.Xiatiao;

internal static class LanguageChecks
{
    internal static int Run(string folder)
    {
        var results = new List<string>();
        try
        {
            _ = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
            Check(folder, results);
            Store.Write(Path.Combine(folder, "language-checks.json"), new { passed = true, checks = results, radioStarted = false, userSettingsChanged = false });
            return 0;
        }
        catch (Exception error)
        {
            Store.Write(Path.Combine(folder, "language-checks.json"), new { passed = false, checks = results, error = error.ToString() });
            return 1;
        }
    }

    internal static void Check(string folder, List<string> results)
    {
        static void Require(bool value, string reason) { if (!value) throw new InvalidOperationException(reason); }
        var before = L.Language;
        try
        {
            L.SetLanguage("zh-CN");
            Require(new Preferences().Language == "zh-CN" && L.Normalize("unknown") == "zh-CN", "The default/fallback language must remain Chinese.");
            Require(L.Catalog.Count >= 250, "The embedded language catalog is incomplete.");
            var fields = new Regex(@"\{(\d+)(?::[^}]*)?\}");
            foreach (var pair in L.Catalog)
            {
                string[] Slots(string text) => fields.Matches(text).Select(match => match.Groups[1].Value).Order().ToArray();
                Require(Slots(pair.Key).SequenceEqual(Slots(pair.Value)), "Translation placeholder mismatch: " + pair.Key);
                Require(!Regex.IsMatch(pair.Value, @"[\p{IsCJKUnifiedIdeographs}]"), "English catalog contains untranslated application copy: " + pair.Key);
            }
            var fixture = Path.Combine(folder, "language-fixture");
            var settingsFile = Path.Combine(fixture, "settings.json");
            Store.Write(settingsFile, new Preferences { Enabled = false, Name = "My test PC", Folder = Path.Combine(fixture, "files") });
            using (var saved = new AppController(dataRoot: fixture))
            {
                saved.Language = "en";
                var stored = Store.Read(settingsFile, new Preferences());
                Require(stored.Language == "en" && stored.Name == "My test PC" && !stored.Enabled, "Changing language did not persist without altering device preferences.");
            }
            L.SetLanguage("zh-CN");
            using (var restored = new AppController(dataRoot: fixture))
                Require(restored.Language == "en" && L.Language == "en", "Saved language was not restored on the real controller path.");
            results.Add("language defaults to Chinese; English persists and reloads using an isolated settings directory");

            using var model = new AppController(true, Path.Combine(folder, "language-preview"));
            model.Preview(false);
            var pending = new TransferItem { Id = "localization", Sender = "文件", Description = "正在取消", FileCount = null };
            pending.Update("正在接收 · 25%", transferring: true, progress: 25);
            model.Pending.Add(pending);
            var counts = new TransferItem { FileCount = 2 };
            model.Pending.Add(counts);
            model.ReportFileError("已处理 {0} 个文件，其余文件保留：{1}", 2, new IOException("目标文件夹不存在。"));
            Require(model.StatusText == "AirDrop is on" && pending.ProgressText == "Receiving · 25%", "Existing receive state did not use English.");
            Require(counts.Description == "2 files" && pending.Sender == "文件" && pending.Description == "正在取消", "User-provided names must remain unchanged.");
            Require(!Regex.IsMatch(model.ErrorText, @"[\p{IsCJKUnifiedIdeographs}]"), "File-operation error remained in Chinese.");
            Require(L.F("{0} 个文件", 1) == "1 file", "Singular file count is incorrect.");
            Require(L.T("正在连接「My iPhone」") == "Connecting to “My iPhone”", "Dynamic device status translation failed.");
            Require(!Regex.IsMatch(L.Error("无线助手未启动：尚未确认专用 MT7925 网卡"), @"[\p{IsCJKUnifiedIdeographs}]"), "Nested helper error was not translated.");
            var pendingCount = model.Pending.Count;
            model.Language = "zh-CN";
            Require(model.StatusText == "AirDrop 已开启" && pending.ProgressText == "正在接收 · 25%" && counts.Description == "2 个文件", "Existing state did not switch back to Chinese.");
            Require(model.ErrorText.Contains("目标文件夹不存在。", StringComparison.Ordinal) && model.Pending.Count == pendingCount && pending.Progress == 25 && model.IsEnabled,
                "Language change altered transfer state or froze an old-language error.");
            model.Language = "en";
            Require(model.StatusText == "AirDrop is on" && !Directory.Exists(Path.Combine(folder, "language-preview")), "Preview language selection wrote preferences or failed to refresh.");
            results.Add("active state/error labels switch both directions; names and transfer state remain intact; preview writes no settings");

            var literalFile = new TransferItem { Id = "literal-file", Sender = "My iPhone", Description = "文件" };
            model.Pending.Add(literalFile);
            model.HandleEvent(JsonSerializer.SerializeToElement(new { @event = "incoming", id = literalFile.Id, done = 1, total = 4 }));
            Require(model.StatusDetail == "文件", "A file name matching a catalog key was translated in the receive status.");
            model.Language = "zh-CN";
            Require(model.StatusDetail == "文件", "The receive file name changed when switching to Chinese.");
            model.Language = "en";
            Require(model.StatusDetail == "文件" && model.StatusText == L.T("正在接收文件"), "The receive file name changed when switching back to English.");
            model.HandleEvent(JsonSerializer.SerializeToElement(new { @event = "incoming", id = "unknown-transfer", done = 0, total = 0 }));
            Require(model.StatusDetail == L.T("文件传输中…") && model.StatusDetail != "文件传输中…", "The literal-file state leaked into a later application message.");
            results.Add("a received file named 文件 stays literal in both languages; later application status resumes translation");
        }
        finally { L.SetLanguage(before); }
    }
}
