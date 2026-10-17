// SPDX-License-Identifier: MIT
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;

namespace Winawdl.Xiatiao;

/// <summary>Blank-space drag selection for the received-file list. No file I/O.</summary>
public static class FileMarqueeSelection
{
    public static readonly DependencyProperty EnabledProperty = DependencyProperty.RegisterAttached(
        "Enabled", typeof(bool), typeof(FileMarqueeSelection), new PropertyMetadata(false, EnabledChanged));
    private static readonly DependencyProperty StateProperty = DependencyProperty.RegisterAttached(
        "State", typeof(DragState), typeof(FileMarqueeSelection));
    public static void SetEnabled(DependencyObject value, bool enabled) => value.SetValue(EnabledProperty, enabled);
    public static bool GetEnabled(DependencyObject value) => (bool)value.GetValue(EnabledProperty);
    internal static DragState? State(ListBox list) => list.GetValue(StateProperty) as DragState;
    public static bool Cancel(ListBox list) => State(list)?.Cancel() ?? false;
    private static void EnabledChanged(DependencyObject value, DependencyPropertyChangedEventArgs e)
    {
        if (value is not ListBox list) return;
        State(list)?.Dispose();
        list.SetValue(StateProperty, (bool)e.NewValue ? new DragState(list) : null);
    }

    internal sealed class DragState : IDisposable
    {
        private readonly ListBox _list;
        private readonly DispatcherTimer _scroll;
        private HashSet<object> _before = new();
        private Point _origin, _pressed;
        private ModifierKeys _modifiers;
        private ScrollViewer? _viewer;
        private SelectionAdorner? _adorner;
        private AdornerLayer? _layer;
        internal bool Pending { get; private set; }
        internal bool Dragging { get; private set; }
        internal Rect Rectangle { get; private set; }

        internal DragState(ListBox list)
        {
            _list = list;
            _scroll = new DispatcherTimer(DispatcherPriority.Input) { Interval = TimeSpan.FromMilliseconds(35) };
            _scroll.Tick += ScrollTick;
            list.PreviewMouseLeftButtonDown += Down;
            list.PreviewMouseMove += Move;
            list.PreviewMouseLeftButtonUp += Up;
            list.LostMouseCapture += LostCapture;
            list.Unloaded += Unloaded;
            list.IsEnabledChanged += EnabledChanged;
        }

        private void Down(object sender, MouseButtonEventArgs e)
        {
            if (!_list.IsEnabled || e.OriginalSource is not DependencyObject source ||
                ItemsControl.ContainerFromElement(_list, source) is ListBoxItem || HasScrollBarAncestor(source)) return;
            var point = e.GetPosition(_list);
            if (!Viewport().Contains(point)) return;
            _list.Focus();
            if (!_list.CaptureMouse()) return;
            Begin(point, Keyboard.Modifiers);
            e.Handled = true;
        }
        internal void Begin(Point point, ModifierKeys modifiers)
        {
            _viewer = FindChild<ScrollViewer>(_list);
            _before = _list.SelectedItems.Cast<object>().ToHashSet();
            _modifiers = modifiers;
            _pressed = point;
            _origin = new Point(point.X, point.Y + Offset);
            Pending = true;
        }
        private void Move(object sender, MouseEventArgs e)
        {
            if (!Pending) return;
            if (e.LeftButton != MouseButtonState.Pressed) { Finish(); return; }
            Update(e.GetPosition(_list));
            if (Dragging) _scroll.Start();
            e.Handled = true;
        }
        internal void Update(Point point)
        {
            if (!Pending) return;
            if (!Dragging && Math.Abs(point.X - _pressed.X) < SystemParameters.MinimumHorizontalDragDistance &&
                Math.Abs(point.Y - _pressed.Y) < SystemParameters.MinimumVerticalDragDistance) return;
            if (!Dragging)
            {
                Dragging = true;
                _layer = AdornerLayer.GetAdornerLayer(_list);
                if (_layer is not null) { _adorner = new SelectionAdorner(_list); _layer.Add(_adorner); }
            }
            var viewport = Viewport();
            var end = new Point(Math.Clamp(point.X, viewport.Left, viewport.Right),
                Math.Clamp(point.Y, viewport.Top, viewport.Bottom) + Offset);
            Rectangle = new Rect(_origin, end);
            var wanted = new HashSet<object>();
            foreach (var item in _list.Items)
            {
                var hit = false;
                if (_list.ItemContainerGenerator.ContainerFromItem(item) is ListBoxItem tile)
                {
                    var bounds = tile.TransformToAncestor(_list).TransformBounds(new Rect(tile.RenderSize));
                    bounds.Offset(0, Offset);
                    var overlap = Rect.Intersect(Rectangle, bounds);
                    hit = !overlap.IsEmpty && overlap.Width > 0 && overlap.Height > 0;
                }
                var selected = (_modifiers & ModifierKeys.Control) != 0 ? _before.Contains(item) ^ hit :
                    (_modifiers & ModifierKeys.Shift) != 0 ? _before.Contains(item) || hit : hit;
                if (selected) wanted.Add(item);
            }
            SetSelection(wanted);
            if (_adorner is not null)
            {
                var visible = Rectangle; visible.Offset(0, -Offset);
                _adorner.Rectangle = visible; _adorner.Viewport = viewport; _adorner.InvalidateVisual();
            }
        }
        private void Up(object sender, MouseButtonEventArgs e)
        {
            if (!Pending) return;
            if (Dragging) Update(e.GetPosition(_list));
            else if ((_modifiers & (ModifierKeys.Control | ModifierKeys.Shift)) == 0) _list.UnselectAll();
            Finish(); e.Handled = true;
        }
        internal bool Cancel()
        {
            if (!Pending) return false;
            SetSelection(_before); Finish(); return true;
        }
        internal void Finish()
        {
            Pending = Dragging = false;
            _scroll.Stop();
            if (_adorner is not null) _layer?.Remove(_adorner);
            _adorner = null; _layer = null;
            if (_list.IsMouseCaptured) _list.ReleaseMouseCapture();
        }
        private void SetSelection(HashSet<object> wanted)
        {
            foreach (var item in _list.SelectedItems.Cast<object>().ToArray())
                if (!wanted.Contains(item)) _list.SelectedItems.Remove(item);
            foreach (var item in _list.Items)
                if (wanted.Contains(item) && !_list.SelectedItems.Contains(item)) _list.SelectedItems.Add(item);
        }
        private void ScrollTick(object? sender, EventArgs e)
        {
            if (!Dragging || Mouse.LeftButton != MouseButtonState.Pressed) { Finish(); return; }
            var pointer = Mouse.GetPosition(_list);
            AutoScroll(pointer); Update(pointer);
        }
        internal void AutoScroll(Point point)
        {
            if (_viewer is null) return;
            var viewport = Viewport();
            var delta = point.Y < viewport.Top + 24 ? -18 : point.Y > viewport.Bottom - 24 ? 18 : 0;
            if (delta == 0) return;
            _viewer.ScrollToVerticalOffset(Math.Clamp(Offset + delta, 0, _viewer.ScrollableHeight));
            _list.UpdateLayout();
        }
        private double Offset => _viewer?.VerticalOffset ?? 0;
        private Rect Viewport()
        {
            var presenter = FindChild<ScrollContentPresenter>(_list);
            return presenter is null ? new Rect(_list.RenderSize) :
                presenter.TransformToAncestor(_list).TransformBounds(new Rect(presenter.RenderSize));
        }
        private static bool HasScrollBarAncestor(DependencyObject? source)
        {
            while (source is not null)
            {
                if (source is ScrollBar) return true;
                source = source is Visual ? VisualTreeHelper.GetParent(source) : LogicalTreeHelper.GetParent(source);
            }
            return false;
        }
        private void LostCapture(object sender, MouseEventArgs e) { if (Pending) Finish(); }
        private void Unloaded(object sender, RoutedEventArgs e) => Cancel();
        private void EnabledChanged(object sender, DependencyPropertyChangedEventArgs e) { if (!(bool)e.NewValue) Cancel(); }
        public void Dispose()
        {
            Cancel();
            _scroll.Tick -= ScrollTick;
            _list.PreviewMouseLeftButtonDown -= Down; _list.PreviewMouseMove -= Move; _list.PreviewMouseLeftButtonUp -= Up;
            _list.LostMouseCapture -= LostCapture; _list.Unloaded -= Unloaded; _list.IsEnabledChanged -= EnabledChanged;
        }
    }

    internal static T? FindChild<T>(DependencyObject parent) where T : DependencyObject
    {
        for (var i = 0; i < VisualTreeHelper.GetChildrenCount(parent); i++)
        {
            var child = VisualTreeHelper.GetChild(parent, i);
            if (child is T match) return match;
            var nested = FindChild<T>(child); if (nested is not null) return nested;
        }
        return null;
    }
    private sealed class SelectionAdorner : Adorner
    {
        internal Rect Rectangle, Viewport;
        internal SelectionAdorner(UIElement element) : base(element) { IsHitTestVisible = false; }
        protected override void OnRender(DrawingContext context)
        {
            context.PushClip(new RectangleGeometry(Viewport));
            context.DrawRectangle(new SolidColorBrush(Color.FromArgb(35, 15, 126, 255)),
                new Pen(new SolidColorBrush(Color.FromRgb(15, 126, 255)), 1), Rectangle);
            context.Pop();
        }
    }
}
