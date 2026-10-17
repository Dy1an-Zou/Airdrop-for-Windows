// SPDX-License-Identifier: MIT
using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;

namespace Winawdl.Xiatiao;

/// <summary>Harsh Gupta's Uiverse green-liger-89 transition, adapted to WPF and reduced motion.</summary>
public static class SwitchMotion
{
    public static readonly DependencyProperty EnabledProperty = DependencyProperty.RegisterAttached(
        "Enabled", typeof(bool), typeof(SwitchMotion), new PropertyMetadata(false, EnabledChanged));
    public static bool GetEnabled(DependencyObject target) => (bool)target.GetValue(EnabledProperty);
    public static void SetEnabled(DependencyObject target, bool value) => target.SetValue(EnabledProperty, value);

    private static readonly DependencyPropertyKey KeyboardFocusVisiblePropertyKey = DependencyProperty.RegisterAttachedReadOnly(
        "KeyboardFocusVisible", typeof(bool), typeof(SwitchMotion), new PropertyMetadata(false));
    public static readonly DependencyProperty KeyboardFocusVisibleProperty = KeyboardFocusVisiblePropertyKey.DependencyProperty;
    public static bool GetKeyboardFocusVisible(DependencyObject target) => (bool)target.GetValue(KeyboardFocusVisibleProperty);
    private static readonly DependencyProperty PointerPressedProperty = DependencyProperty.RegisterAttached(
        "PointerPressed", typeof(bool), typeof(SwitchMotion), new PropertyMetadata(false));

    private static void EnabledChanged(DependencyObject target, DependencyPropertyChangedEventArgs args)
    {
        if (target is not ToggleButton toggle) return;
        if ((bool)args.NewValue)
        {
            toggle.Loaded += Loaded;
            toggle.SizeChanged += Sized;
            toggle.Checked += Changed;
            toggle.Unchecked += Changed;
            toggle.PreviewMouseDown += PointerDown;
            toggle.PreviewMouseUp += PointerUp;
            toggle.PreviewKeyDown += KeyboardPressed;
            toggle.GotKeyboardFocus += KeyboardFocused;
            toggle.LostKeyboardFocus += KeyboardBlurred;
        }
        else
        {
            toggle.Loaded -= Loaded;
            toggle.SizeChanged -= Sized;
            toggle.Checked -= Changed;
            toggle.Unchecked -= Changed;
            toggle.PreviewMouseDown -= PointerDown;
            toggle.PreviewMouseUp -= PointerUp;
            toggle.PreviewKeyDown -= KeyboardPressed;
            toggle.GotKeyboardFocus -= KeyboardFocused;
            toggle.LostKeyboardFocus -= KeyboardBlurred;
            toggle.SetValue(KeyboardFocusVisiblePropertyKey, false);
            toggle.SetValue(PointerPressedProperty, false);
        }
    }

    private static void PointerDown(object sender, MouseButtonEventArgs args)
    {
        var toggle = (ToggleButton)sender;
        toggle.SetValue(PointerPressedProperty, true);
        toggle.SetValue(KeyboardFocusVisiblePropertyKey, false);
    }
    private static void PointerUp(object sender, MouseButtonEventArgs args) =>
        ((ToggleButton)sender).SetValue(PointerPressedProperty, false);
    private static void KeyboardPressed(object sender, KeyEventArgs args)
    {
        var toggle = (ToggleButton)sender;
        toggle.SetValue(PointerPressedProperty, false);
        toggle.SetValue(KeyboardFocusVisiblePropertyKey, true);
    }
    private static void KeyboardFocused(object sender, KeyboardFocusChangedEventArgs args)
    {
        var toggle = (ToggleButton)sender;
        toggle.SetValue(KeyboardFocusVisiblePropertyKey, !(bool)toggle.GetValue(PointerPressedProperty) &&
            InputManager.Current.MostRecentInputDevice is KeyboardDevice);
    }
    private static void KeyboardBlurred(object sender, KeyboardFocusChangedEventArgs args)
    {
        var toggle = (ToggleButton)sender;
        toggle.SetValue(KeyboardFocusVisiblePropertyKey, false);
        toggle.SetValue(PointerPressedProperty, false);
    }

    private static void Loaded(object sender, RoutedEventArgs args) => Apply((ToggleButton)sender, false);
    private static void Sized(object sender, SizeChangedEventArgs args) => Apply((ToggleButton)sender, false);
    private static void Changed(object sender, RoutedEventArgs args)
    {
        var toggle = (ToggleButton)sender;
        Apply(toggle, toggle.IsLoaded && SystemParameters.ClientAreaAnimation);
    }

    private static void Apply(ToggleButton toggle, bool animate)
    {
        if (toggle.Template is null || toggle.Template.FindName("Thumb", toggle) is not Ellipse thumb ||
            toggle.Template.FindName("Track", toggle) is not Border track) return;
        // Template setters provide a correct static state even before Loaded (including
        // offscreen rendering). Clone frozen/shared values before owning live animation.
        var originalShift = thumb.RenderTransform as TranslateTransform;
        var shift = originalShift?.CloneCurrentValue() ?? new TranslateTransform();
        thumb.RenderTransform = shift;
        var targetX = toggle.IsChecked == true ? 27.2d : 0d; // 1.6em at the source's 17px font-size.
        var fromX = shift.X;
        var targetColor = toggle.IsChecked == true ? Color.FromRgb(9, 116, 241) : Color.FromRgb(159, 204, 250);
        var fromColor = (track.Background as SolidColorBrush)?.Color ?? targetColor;
        // BeginAnimation takes effect on the next render tick. Keep the current
        // values as the base until then; writing the target here exposes a jump
        // before the clock starts, including during a quick direction reversal.
        shift.X = animate ? fromX : targetX;
        var trackBrush = new SolidColorBrush(animate ? fromColor : targetColor);
        track.Background = trackBrush;
        if (!animate) return;
        var duration = TimeSpan.FromMilliseconds(400);
        var ease = new CssSwitchEase { EasingMode = EasingMode.EaseIn };
        shift.BeginAnimation(TranslateTransform.XProperty, new DoubleAnimation(fromX, targetX, duration)
        { EasingFunction = ease, FillBehavior = FillBehavior.HoldEnd });
        // The CSS curve overshoots. Sample/clamp each RGB channel as browsers do,
        // while allowing the thumb's spatial overshoot to remain fully intact.
        var colors = new ColorAnimationUsingKeyFrames { Duration = duration, FillBehavior = FillBehavior.HoldEnd };
        for (var step = 0; step <= 24; step++)
        {
            var fraction = step / 24d;
            var progress = CssSwitchEase.Evaluate(fraction);
            static byte Channel(byte from, byte to, double progress) => (byte)Math.Clamp(Math.Round(from + (to - from) * progress), 0, 255);
            var color = Color.FromRgb(Channel(fromColor.R, targetColor.R, progress),
                Channel(fromColor.G, targetColor.G, progress), Channel(fromColor.B, targetColor.B, progress));
            colors.KeyFrames.Add(new LinearColorKeyFrame(color, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(400 * fraction))));
        }
        trackBrush.BeginAnimation(SolidColorBrush.ColorProperty, colors);
    }
}

/// <summary>Exact source timing curve: cubic-bezier(0.175, 0.885, 0.32, 1.275).</summary>
public sealed class CssSwitchEase : EasingFunctionBase
{
    protected override Freezable CreateInstanceCore() => new CssSwitchEase();
    protected override double EaseInCore(double normalizedTime) => Evaluate(normalizedTime);
    internal static double Evaluate(double time)
    {
        if (time <= 0) return 0;
        if (time >= 1) return 1;
        static double Curve(double t, double first, double second) =>
            3 * (1 - t) * (1 - t) * t * first + 3 * (1 - t) * t * t * second + t * t * t;
        var low = 0d;
        var high = 1d;
        // CSS time is the curve's x, not its parametric t. Invert the monotonic x.
        for (var iteration = 0; iteration < 28; iteration++)
        {
            var midpoint = (low + high) / 2;
            if (Curve(midpoint, 0.175, 0.32) < time) low = midpoint;
            else high = midpoint;
        }
        return Curve((low + high) / 2, 0.885, 1.275);
    }
}
