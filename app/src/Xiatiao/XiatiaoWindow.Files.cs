// SPDX-License-Identifier: MIT
using System;
using System.Collections.Generic;
using System.Collections.Specialized;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;

namespace Winawdl.Xiatiao;

public partial class XiatiaoWindow
{
    private bool _fileOperationBusy;
    private SendWindow? _sendWindow;
    private string _fileSummaryTemplate = "空白处框选 · 文件可拖出 · 拖入文件发送";
    private object[] _fileSummaryArguments = Array.Empty<object>();

    private void SetFileSummary(string template, params object[] arguments)
    {
        _fileSummaryTemplate = template;
        _fileSummaryArguments = arguments;
        RefreshFileSummary();
    }

    private void RefreshFileSummary()
    {
        if (FileSelectionSummary is not null)
            FileSelectionSummary.Text = L.F(_fileSummaryTemplate, _fileSummaryArguments);
    }

    private ReceivedItem[] SelectedFiles() => FileList.SelectedItems.Cast<ReceivedItem>()
        .Where(_model.Files.Contains).ToArray();

    private void FileSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (FileSelectionSummary is null || _fileOperationBusy) return;
        var count = FileList.SelectedItems.Count;
        if (count > 0) SetFileSummary("已选择 {0} 个文件 · 可拖出或右键操作", count);
        else SetFileSummary("空白处框选 · 文件可拖出 · 拖入文件发送");
    }

    private void FileRightButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.OriginalSource is not DependencyObject source ||
            ItemsControl.ContainerFromElement(FileList, source) is not ListBoxItem item) return;
        // Preserve a multi-selection when its member is right-clicked.
        if (!item.IsSelected) { FileList.SelectedItems.Clear(); item.IsSelected = true; }
        item.Focus();
    }

    private void FileMenuOpened(object sender, RoutedEventArgs e)
    {
        var selected = SelectedFiles();
        var available = selected.Length > 0 && selected.All(file => File.Exists(file.FullPath));
        OpenFileMenu.IsEnabled = RevealFileMenu.IsEnabled = !_fileOperationBusy && available && selected.Length == 1;
        CopyFilesMenu.IsEnabled = CopyToMenu.IsEnabled = RecycleFilesMenu.IsEnabled = !_fileOperationBusy && available;
        CopyPathsMenu.IsEnabled = RemoveHistoryMenu.IsEnabled = !_fileOperationBusy && selected.Length > 0;
    }

    private void FileDoubleClicked(object sender, MouseButtonEventArgs e)
    {
        if (_fileOperationBusy || e.ChangedButton != MouseButton.Left || e.OriginalSource is not DependencyObject source ||
            ItemsControl.ContainerFromElement(FileList, source) is not ListBoxItem { DataContext: ReceivedItem file }) return;
        _model.OpenFileCommand.Execute(file);
        e.Handled = true;
    }

    private void FileKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Escape && FileMarqueeSelection.Cancel(FileList)) { e.Handled = true; return; }
        if (_fileOperationBusy) return;
        var control = (Keyboard.Modifiers & ModifierKeys.Control) != 0;
        if (control && e.Key == Key.A) { FileList.SelectAll(); e.Handled = true; }
        else if (control && e.Key == Key.C)
        {
            if ((Keyboard.Modifiers & ModifierKeys.Shift) != 0) CopyPathsClicked(sender, e);
            else CopySelectedClicked(sender, e);
            e.Handled = true;
        }
        else if (e.Key == Key.Delete) { RecycleSelectedClicked(sender, e); e.Handled = true; }
        else if (e.Key == Key.Enter) { OpenSelectedClicked(sender, e); e.Handled = true; }
        else if (e.Key == Key.Escape) { FileList.UnselectAll(); e.Handled = true; }
    }

    private void OpenSelectedClicked(object sender, RoutedEventArgs e)
    { var selected = SelectedFiles(); if (!_fileOperationBusy && selected.Length == 1) _model.OpenFileCommand.Execute(selected[0]); }

    private void RevealSelectedClicked(object sender, RoutedEventArgs e)
    { var selected = SelectedFiles(); if (!_fileOperationBusy && selected.Length == 1) _model.ShowFileCommand.Execute(selected[0]); }

    private void CopySelectedClicked(object sender, RoutedEventArgs e) => FileAction(() =>
    {
        var selected = SelectedFiles();
        if (selected.Length == 0) return;
        var paths = new StringCollection();
        paths.AddRange(selected.Select(file => ReceivedFileActions.RegularFile(file.FullPath)).ToArray());
        Clipboard.SetFileDropList(paths);
        SetFileSummary("已复制 {0} 个文件，可粘贴到文件夹", selected.Length);
    });

    private void CopyPathsClicked(object sender, RoutedEventArgs e) => FileAction(() =>
    {
        var selected = SelectedFiles();
        if (selected.Length == 0) return;
        Clipboard.SetText(string.Join(Environment.NewLine, selected.Select(file => file.FullPath)));
        SetFileSummary("已复制 {0} 个文件路径", selected.Length);
    });

    private async void CopyToClicked(object sender, RoutedEventArgs e)
    {
        if (_fileOperationBusy) return;
        var selected = SelectedFiles();
        if (selected.Length == 0) return;
        var picker = new Microsoft.Win32.OpenFolderDialog { Title = L.T("将所选文件复制到"), Multiselect = false };
        if (picker.ShowDialog(this) != true) return;
        SetFileOperationBusy(true, "正在复制 {0} 个文件…", selected.Length);
        var copied = 0;
        try
        {
            await Task.Run(() =>
            {
                foreach (var file in selected) { ReceivedFileActions.CopyTo(file.FullPath, picker.FolderName); copied++; }
            });
            SetFileSummary("已复制 {0} 个文件；同名文件自动保留两份", copied);
        }
        catch (Exception error) when (FileError(error))
        { _model.ReportFileError("已复制 {0} 个文件。其余文件复制失败：{1}", copied, error); }
        finally { SetFileOperationBusy(false); }
    }

    private void RemoveSelectedClicked(object sender, RoutedEventArgs e)
    {
        if (_fileOperationBusy) return;
        var selected = SelectedFiles();
        if (selected.Length == 0) return;
        _model.RemoveHistory(selected);
        SetFileSummary("已移除 {0} 条记录，磁盘文件仍保留", selected.Length);
    }

    private async void RecycleSelectedClicked(object sender, RoutedEventArgs e)
    {
        if (_fileOperationBusy) return;
        var selected = SelectedFiles();
        if (selected.Length == 0) return;
        if (MessageBox.Show(this, L.F("将选中的 {0} 个文件移到回收站？\n对应记录也会移除。", selected.Length), L.T("移到回收站"),
                MessageBoxButton.OKCancel, MessageBoxImage.Question, MessageBoxResult.Cancel) != MessageBoxResult.OK) return;
        var owner = new WindowInteropHelper(this).Handle;
        SetFileOperationBusy(true, "正在移到回收站…");
        var recycled = new List<ReceivedItem>();
        var completion = new TaskCompletionSource<Exception?>(TaskCreationOptions.RunContinuationsAsynchronously);
        var worker = new Thread(() =>
        {
            try
            {
                foreach (var file in selected) { RecycleFiles.Move(file.FullPath, owner); recycled.Add(file); }
                completion.SetResult(null);
            }
            catch (Exception error) { completion.SetResult(error); }
        }) { IsBackground = true, Name = "AirDrop recycle files" };
        worker.SetApartmentState(ApartmentState.STA);
        worker.Start();
        try
        {
            var error = await completion.Task;
            _model.RemoveHistory(recycled);
            SetFileSummary("已将 {0} 个文件移到回收站", recycled.Count);
            if (error is not null) _model.ReportFileError("已处理 {0} 个文件，其余文件保留：{1}", recycled.Count, error);
        }
        finally { SetFileOperationBusy(false); }
    }

    private void ClearHistoryClicked(object sender, RoutedEventArgs e)
    {
        if (_fileOperationBusy || _model.Files.Count == 0) return;
        if (MessageBox.Show(this, L.T("清空所有接收记录？\n已保存的文件不会删除。"), L.T("清空记录"), MessageBoxButton.OKCancel,
                MessageBoxImage.Question, MessageBoxResult.Cancel) == MessageBoxResult.OK)
            _model.ClearHistoryCommand.Execute(null);
    }

    private void SetFileOperationBusy(bool busy, string? message = null, params object[] arguments)
    {
        _fileOperationBusy = busy;
        FileList.IsEnabled = !busy;
        if (message is not null) SetFileSummary(message, arguments);
    }

    private void FileAction(Action action)
    {
        if (_fileOperationBusy) return;
        try { action(); }
        catch (Exception error) when (FileError(error)) { _model.ReportFileError(error.Message); }
    }

    private static bool FileError(Exception error) => error is IOException or UnauthorizedAccessException or
        COMException or ArgumentException or NotSupportedException or System.Security.SecurityException;

    private void SendClicked(object sender, RoutedEventArgs e)
    {
        if (_sendWindow is not null) { _sendWindow.Activate(); return; }
        var window = CreateSendWindow();
        _sendWindow = window;
        window.Owner = this;
        window.Closed += (_, _) => { _sendWindow = null; _model.Sender.EndLooking(); };
        window.Show();
        _model.Sender.BeginLooking();
    }

    internal SendWindow CreateSendWindow() => new(_model.Sender) { Icon = Icon };
}
