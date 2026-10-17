// SPDX-License-Identifier: MIT
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;

namespace Winawdl.Xiatiao;

/// <summary>Focused offscreen checks: actual list containers and production drag logic.</summary>
internal static class UiChecks
{
    internal static int Run(string folder)
    {
        Directory.CreateDirectory(folder);
        var results = new List<string>();
        try
        {
            var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
            using var model = new AppController(true); model.Preview(false);
            var fixtures = Path.Combine(folder, "fixtures"); Directory.CreateDirectory(fixtures);
            for (var i = 0; i < 24; i++)
            {
                var path = Path.Combine(fixtures, i == 0 ? "虾条图标.png" : $"资料 {i:00}.txt");
                if (i == 0) File.Copy(Path.Combine(AppContext.BaseDirectory, "assets", "xiatiao-airdrop.png"), path, true);
                else File.WriteAllText(path, "离线界面示例");
                var item = new ReceivedItem { FullPath = path, Bytes = new FileInfo(path).Length,
                    ReceivedAt = new DateTimeOffset(2026, 10, 16, 12, 0, 0, TimeSpan.FromHours(8)) };
                item.LoadThumbnail(); model.Files.Add(item);
            }
            var window = new XiatiaoWindow(model);
            AppChecks.Save(window, 860, 900, Path.Combine(folder, "main.png"));
            var list = (ListBox)window.FindName("FileList");
            var drag = FileMarqueeSelection.State(list) ?? throw new Exception("框选没有接到实际文件列表");
            Rect Bounds(int index) => ((ListBoxItem)list.ItemContainerGenerator.ContainerFromIndex(index))
                .TransformToAncestor(list).TransformBounds(new Rect(((ListBoxItem)list.ItemContainerGenerator.ContainerFromIndex(index)).RenderSize));
            var first = Bounds(0); var second = Bounds(1);
            var start = new Point(Math.Max(0, first.Left - 1), Math.Max(0, first.Top - 2));
            var end = new Point(second.Right - 1, second.Bottom - 1);
            void Expect(params int[] indexes)
            {
                if (!list.SelectedItems.Cast<object>().ToHashSet().SetEquals(indexes.Select(i => (object)model.Files[i])))
                    throw new Exception("框选结果不符，期望：" + string.Join(",", indexes) + "，实际：" + string.Join(",", list.SelectedItems.Cast<ReceivedItem>().Select(i => model.Files.IndexOf(i))));
            }
            list.SelectedItems.Add(model.Files[2]);
            drag.Begin(start, ModifierKeys.None); drag.Update(new Point(start.X + 1, start.Y + 1));
            Expect(2); if (drag.Dragging) throw new Exception("微小移动被当成框选"); drag.Finish();
            results.Add("drag threshold retains selection");
            drag.Begin(start, ModifierKeys.None); drag.Update(end); Expect(0, 1);
            AppChecks.Save(window, 860, 900, Path.Combine(folder, "marquee.png")); drag.Finish();
            if (!((TextBlock)window.FindName("FileSelectionSummary")).Text.Contains("2")) throw new Exception("选择数没有同步");
            results.Add("actual WrapPanel containers: drag selects two files and updates summary");
            drag.Begin(end, ModifierKeys.None); drag.Update(start); Expect(0, 1); drag.Finish();
            results.Add("reverse direction gives same selection");
            list.UnselectAll(); list.SelectedItems.Add(model.Files[1]); list.SelectedItems.Add(model.Files[2]);
            drag.Begin(start, ModifierKeys.Control); drag.Update(end); Expect(0, 2);
            drag.Update(new Point(end.X - 1, end.Y)); Expect(0, 2); drag.Finish();
            results.Add("Ctrl toggles against initial selection without repeated toggling");
            list.UnselectAll(); list.SelectedItems.Add(model.Files[2]);
            drag.Begin(start, ModifierKeys.Shift); drag.Update(end); Expect(0, 1, 2); drag.Finish();
            results.Add("Shift adds to initial selection");
            list.UnselectAll(); list.SelectedItems.Add(model.Files[2]);
            drag.Begin(start, ModifierKeys.None); drag.Update(end); drag.Cancel(); Expect(2);
            if (drag.Pending || drag.Dragging) throw new Exception("取消未释放拖动状态");
            results.Add("cancel restores initial selection and clears overlay state");
            drag.Begin(start, ModifierKeys.None); drag.Update(end); list.IsEnabled = false; Expect(2);
            if (drag.Pending) throw new Exception("禁用列表后仍在框选"); list.IsEnabled = true;
            results.Add("busy/disabled cancels drag");
            var viewer = FileMarqueeSelection.FindChild<ScrollViewer>(list)!;
            drag.Begin(start, ModifierKeys.None); drag.Update(end);
            var bottom = new Point(second.Right - 1, list.ActualHeight - 2);
            for (var i = 0; i < 24; i++) { drag.AutoScroll(bottom); drag.Update(bottom); }
            if (viewer.VerticalOffset <= 0 || list.SelectedItems.Count <= 4 || !list.SelectedItems.Contains(model.Files[0]))
                throw new Exception("跨屏框选没有保留起点或自动滚动失败");
            drag.Finish(); results.Add("edge scrolling extends selection across rows and preserves origin");
            viewer.ScrollToVerticalOffset(0); list.UnselectAll(); list.UpdateLayout();
            for (var i = model.Files.Count - 1; i >= 6; i--) model.Files.RemoveAt(i);
            AppChecks.Save(window, 860, 900, Path.Combine(folder, "main.png"));
            AppChecks.Save(window, 640, 720, Path.Combine(folder, "compact.png"));
            AppChecks.Save(window.CreateAboutWindow(), 470, 620, Path.Combine(folder, "about.png"));
            model.Sender.Preview(); AppChecks.Save(window.CreateSendWindow(), 680, 740, Path.Combine(folder, "send.png"));
            CheckSwitch(window, folder, results);
            SwitchChecks.Run((Style)window.FindResource("Switch"), folder, results);
            Store.Write(Path.Combine(folder, "ui-checks.json"), new { passed = true, checks = results,
                desktopInputSent = false, radioStarted = false, note = "Offscreen WPF render and drag controller checks; not physical mouse acceptance." });
            return 0;
        }
        catch (Exception error)
        {
            Store.Write(Path.Combine(folder, "ui-checks.json"), new { passed = false, checks = results, error = error.ToString() });
            return 1;
        }
    }
    private static void CheckSwitch(XiatiaoWindow main, string folder, List<string> results)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(35), VerticalAlignment = VerticalAlignment.Center };
        foreach (var enabled in new[] { false, true })
        {
            var toggle = new ToggleButton { Style = (Style)main.FindResource("Switch"), IsChecked = enabled,
                Margin = new Thickness(20), LayoutTransform = new ScaleTransform(2, 2) };
            row.Children.Add(toggle);
        }
        var window = new Window { Background = new SolidColorBrush(Color.FromRgb(224, 224, 224)), Content = row };
        AppChecks.Save(window, 420, 190, Path.Combine(folder, "switches.png"));
        foreach (ToggleButton toggle in row.Children)
        {
            var track = (Border)toggle.Template.FindName("Track", toggle);
            if (track.CornerRadius.TopLeft != track.ActualHeight / 2 || toggle.FocusVisualStyle is not null)
                throw new Exception("开关轨道仍非胶囊或仍带默认焦点外圈");
            toggle.RaiseEvent(new MouseButtonEventArgs(Mouse.PrimaryDevice, 0, MouseButton.Left) { RoutedEvent = Mouse.PreviewMouseDownEvent });
            if (SwitchMotion.GetKeyboardFocusVisible(toggle) || ((Border)toggle.Template.FindName("KeyboardFocusOutline", toggle)).Visibility != Visibility.Collapsed)
                throw new Exception("鼠标点击仍显示外圈");
        }
        results.Add("actual switch template: capsule radius, on/off render, mouse focus outline hidden");
    }
}
