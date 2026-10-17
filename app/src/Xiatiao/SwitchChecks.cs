// SPDX-License-Identifier: MIT
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using Path = System.IO.Path;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;

namespace Winawdl.Xiatiao;

/// <summary>Observe the production switch clocks in a hidden, non-activating presentation source.</summary>
internal static class SwitchChecks
{
    internal static void Run(Style style, string folder, List<string> results)
    {
        var observations = new List<object>();
        var animationEnabled = SystemParameters.ClientAreaAnimation;
        var loaded = false;
        var dynamicVerified = false;
        Exception? failure = null;
        try
        {
            // No WS_VISIBLE, ShowWindow, keyboard/mouse input or activation call.
            var parameters = new HwndSourceParameters("AirDrop hidden switch clock check")
            {
                WindowStyle = unchecked((int)0x80000000), // WS_POPUP, hidden
                ExtendedWindowStyle = 0x08000080, // WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW
                PositionX = -32000, PositionY = -32000, Width = 180, Height = 110
            };
            using var source = new HwndSource(parameters);
            var toggle = new ToggleButton { Style = style, IsChecked = false,
                HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top,
                Margin = new Thickness(30) };
            var root = new Border { Child = toggle, Background = Brushes.Transparent };
            source.RootVisual = root;
            root.Measure(new Size(180, 110)); root.Arrange(new Rect(0, 0, 180, 110)); root.UpdateLayout();
            PumpUntil(() => toggle.IsLoaded, TimeSpan.FromSeconds(2));
            loaded = toggle.IsLoaded;
            if (!loaded) throw new InvalidOperationException("隐藏开关未在两秒内 Loaded，不能验证真实 Changed 动画路径。");
            PumpFor(TimeSpan.FromMilliseconds(35));

            double Position()
            {
                var thumb = (Ellipse)toggle.Template.FindName("Thumb", toggle);
                return ((TranslateTransform)thumb.RenderTransform).X;
            }
            string TrackColor()
            {
                var track = (Border)toggle.Template.FindName("Track", toggle);
                return ((SolidColorBrush)track.Background).Color.ToString();
            }
            void Record(string phase, Stopwatch clock, int nominalMs)
            {
                var thumb = (Ellipse)toggle.Template.FindName("Thumb", toggle);
                var shift = (TranslateTransform)thumb.RenderTransform;
                var track = (Border)toggle.Template.FindName("Track", toggle);
                var brush = (SolidColorBrush)track.Background;
                observations.Add(new { phase, nominalMs, elapsedMs = clock.Elapsed.TotalMilliseconds,
                    x = Position(), trackColor = TrackColor(),
                    baseX = (double)shift.GetAnimationBaseValue(TranslateTransform.XProperty),
                    baseTrackColor = brush.GetAnimationBaseValue(SolidColorBrush.ColorProperty).ToString(),
                    thumbAnimated = shift.HasAnimatedProperties, trackAnimated = brush.HasAnimatedProperties });
            }
            static void Require(bool condition, string message)
            { if (!condition) throw new InvalidOperationException(message); }

            Require(Math.Abs(Position()) < 0.15, "关闭初态滑块位置不正确。");
            var onClock = Stopwatch.StartNew();
            toggle.IsChecked = true;
            Record("on", onClock, 0);
            if (!animationEnabled)
            {
                Require(Math.Abs(Position() - 27.2) < 0.15, "系统减少动画时未立即切到开启位置。");
                toggle.IsChecked = false;
                Require(Math.Abs(Position()) < 0.15, "系统减少动画时未立即切到关闭位置。");
                results.Add("switch reduced-motion immediate states verified; dynamic checks skipped because system animations are disabled");
                return;
            }

            Require(Math.Abs(Position()) < 0.5, "Changed 路径先跳到终点，开启动画没有从关闭位置开始。");
            var movedBetweenEndpoints = false;
            foreach (var targetMs in new[] { 100, 220, 450 })
            {
                PumpUntil(() => onClock.Elapsed.TotalMilliseconds >= targetMs, TimeSpan.FromSeconds(2));
                Record("on", onClock, targetMs);
                var x = Position();
                Require(double.IsFinite(x) && x > -1 && x < 32, "开关动画出现非有限值或超出原曲线范围。");
                if (onClock.Elapsed.TotalMilliseconds < 350 && x > 0.5 && Math.Abs(x - 27.2) > 0.25)
                    movedBetweenEndpoints = true;
            }
            Require(movedBetweenEndpoints, "未采到开关运动中的位置；不能把静态终态当作动态验证。");
            Require(Math.Abs(Position() - 27.2) < 0.2, "400ms 开启动画结束后没有收敛到目标位置。");

            var offClock = Stopwatch.StartNew();
            toggle.IsChecked = false;
            Record("off-before-reverse", offClock, 0);
            Require(Math.Abs(Position() - 27.2) < 0.5, "关闭动画没有从开启位置开始。");
            PumpUntil(() => offClock.Elapsed.TotalMilliseconds >= 90, TimeSpan.FromSeconds(2));
            Record("off-before-reverse", offClock, 90);
            var beforeReverse = Position();
            Require(offClock.Elapsed.TotalMilliseconds < 350 && double.IsFinite(beforeReverse) && beforeReverse > -5 && beforeReverse < 32 &&
                Math.Abs(beforeReverse) > 0.25 && Math.Abs(beforeReverse - 27.2) > 0.25,
                "未采到关闭运动中的反转时刻，无法核实中途反向连续性。");
            var reverseClock = Stopwatch.StartNew();
            toggle.IsChecked = true;
            var afterReverse = Position();
            Record("reverse-on", reverseClock, 0);
            Require(Math.Abs(afterReverse - beforeReverse) < 0.8, "快速反向切换时滑块从当前位置跳变。");
            PumpUntil(() => reverseClock.Elapsed.TotalMilliseconds >= 450, TimeSpan.FromSeconds(2));
            Record("reverse-on", reverseClock, 450);
            Require(Math.Abs(Position() - 27.2) < 0.2, "反向动画结束后没有收敛到开启位置。");
            dynamicVerified = true;
            results.Add("hidden actual switch: Loaded Changed animation sampled at 0/100/220/450ms; mid-animation reversal continuous and final position correct");
        }
        catch (Exception error) { failure = error; throw; }
        finally
        {
            Store.Write(Path.Combine(folder, "switch-animation-check.json"), new
            {
                passed = failure is null, loaded, animationEnabled, dynamicVerified,
                skipped = !animationEnabled && failure is null,
                reason = !animationEnabled ? "Windows ClientAreaAnimation is disabled; system setting was not changed." : null,
                desktopInputSent = false, visibleWindowShown = false, focusRequested = false,
                observations, error = failure?.ToString()
            });
        }
    }

    private static void PumpFor(TimeSpan duration)
    {
        var clock = Stopwatch.StartNew();
        PumpUntil(() => clock.Elapsed >= duration, duration + TimeSpan.FromSeconds(1));
    }

    private static void PumpUntil(Func<bool> complete, TimeSpan timeout)
    {
        if (complete()) return;
        var elapsed = Stopwatch.StartNew();
        var frame = new DispatcherFrame();
        var timer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromMilliseconds(5) };
        EventHandler tick = (_, _) => { if (complete() || elapsed.Elapsed >= timeout) frame.Continue = false; };
        timer.Tick += tick;
        timer.Start();
        try { Dispatcher.PushFrame(frame); }
        finally { timer.Stop(); timer.Tick -= tick; }
        if (!complete()) throw new TimeoutException("等待隐藏开关的加载或动画时钟超时。");
    }
}
