// SPDX-License-Identifier: MIT
using System.Collections.Specialized;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace Winawdl.Xiatiao;

/// <summary>File tiles use native OLE copy; blank-space drags remain marquee selection.</summary>
internal sealed class ReceivedFileDrag
{
    internal const string SourceFormat = "AirdropForWindows.ReceivedFileDrag";
    private readonly ListBox _list;
    private readonly Action<string> _error;
    private ReceivedItem? _pressed;
    private Point _origin;
    private bool _deferSelection;

    internal ReceivedFileDrag(ListBox list, Action<string> error)
    {
        _list = list; _error = error;
        list.PreviewMouseLeftButtonDown += (_, e) =>
        {
            if (e.OriginalSource is DependencyObject source && Begin(source, e.GetPosition(list), Keyboard.Modifiers, e.ClickCount))
                e.Handled = true;
        };
        list.PreviewMouseMove += Move;
        list.PreviewMouseLeftButtonUp += (_, e) => { if (EndClick()) e.Handled = true; };
        list.Unloaded += (_, _) => Reset();
        list.LostMouseCapture += (_, _) => Reset();
    }

    internal bool Begin(DependencyObject source, Point point, ModifierKeys modifiers, int clickCount)
    {
        Reset();
        if (!_list.IsEnabled || clickCount != 1 ||
            ItemsControl.ContainerFromElement(_list, source) is not ListBoxItem { Content: ReceivedItem item } tile) return false;
        _origin = point; _pressed = item;
        // Defer normal collapse until release so a selected group can be dragged intact.
        _deferSelection = tile.IsSelected && modifiers == ModifierKeys.None;
        return _deferSelection;
    }

    internal DataObject? TakePayload(Point point, bool leftPressed)
    {
        if (!leftPressed || !_list.IsEnabled) { Reset(); return null; }
        if (_pressed is null ||
            (Math.Abs(point.X - _origin.X) < SystemParameters.MinimumHorizontalDragDistance &&
             Math.Abs(point.Y - _origin.Y) < SystemParameters.MinimumVerticalDragDistance)) return null;
        var pressed = _pressed; Reset();
        if (!_list.SelectedItems.Contains(pressed)) return null;
        var paths = _list.SelectedItems.Cast<ReceivedItem>().Select(file => ReceivedFileActions.RegularFile(file.FullPath)).ToArray();
        if (paths.Length == 0) return null;
        var files = new StringCollection(); files.AddRange(paths);
        var data = new DataObject(); data.SetFileDropList(files);
        data.SetData(SourceFormat, true);
        return data;
    }

    internal bool EndClick()
    {
        var pressed = _pressed; var collapse = _deferSelection; Reset();
        if (!collapse || pressed is null || !_list.IsEnabled || !_list.Items.Contains(pressed)) return false;
        _list.UnselectAll(); _list.SelectedItems.Add(pressed); return true;
    }

    private void Move(object sender, MouseEventArgs e)
    {
        try
        {
            var data = TakePayload(e.GetPosition(_list), e.LeftButton == MouseButtonState.Pressed);
            if (data is null) return;
            e.Handled = true;
            if (_list.IsMouseCaptured) _list.ReleaseMouseCapture();
            // No Move effect: recipients may copy/upload the files, but originals stay in the shelf.
            DragDrop.DoDragDrop(_list, data, DragDropEffects.Copy);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or COMException or
            ArgumentException or NotSupportedException or System.Security.SecurityException)
        { Reset(); e.Handled = true; _error(error.Message); }
    }

    private void Reset() { _pressed = null; _deferSelection = false; }
}
