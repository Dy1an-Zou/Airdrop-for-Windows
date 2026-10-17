// SPDX-License-Identifier: MIT
using System.Windows;

namespace Winawdl.Xiatiao;

/// <summary>Explorer-compatible file drops share the file picker's validation.</summary>
internal static class FileDropInput
{
    internal static void Attach(Window window, SenderController sender, Action? showSender = null)
    {
        window.AllowDrop = true;
        window.PreviewDragEnter += (_, e) => Over(sender, e, showSender is not null);
        window.PreviewDragOver += (_, e) => Over(sender, e, showSender is not null);
        window.PreviewDrop += (_, e) => Drop(sender, e, showSender);
    }

    internal static void Over(SenderController sender, DragEventArgs e, bool mainWindow)
    {
        if (!e.Data.GetDataPresent(DataFormats.FileDrop)) return;
        e.Handled = true;
        e.Effects = Accepts(sender, e.Data, e.AllowedEffects, mainWindow) ? DragDropEffects.Copy : DragDropEffects.None;
    }

    internal static void Drop(SenderController sender, DragEventArgs e, Action? showSender = null)
    {
        if (!e.Data.GetDataPresent(DataFormats.FileDrop)) return;
        e.Handled = true;
        e.Effects = Receive(sender, e.Data, e.AllowedEffects, showSender);
    }

    internal static DragDropEffects Receive(SenderController sender, IDataObject data, DragDropEffects allowed, Action? showSender = null)
    {
        if (!Accepts(sender, data, allowed, showSender is not null) ||
            data.GetData(DataFormats.FileDrop) is not string[] { Length: > 0 } paths) return DragDropEffects.None;
        // The sender reports validation failures inside its window, without a modal dialog.
        showSender?.Invoke();
        return sender.TryAddFiles(paths) ? DragDropEffects.Copy : DragDropEffects.None;
    }

    internal static bool Accepts(SenderController sender, IDataObject data, DragDropEffects allowed, bool mainWindow) =>
        sender.CanEditFiles && data.GetDataPresent(DataFormats.FileDrop) && (allowed & DragDropEffects.Copy) != 0 &&
        // Dragging across the received shelf must not open our send dialog underneath the pointer.
        !(mainWindow && data.GetDataPresent(ReceivedFileDrag.SourceFormat));
}
