// SPDX-License-Identifier: MIT
using System;
using System.Collections.Specialized;
using System.Windows;

namespace Winawdl.Xiatiao;

public partial class SendWindow : Window
{
    private readonly SenderController _sender;

    internal SendWindow(SenderController sender)
    {
        _sender = sender;
        InitializeComponent();
        DataContext = sender;
        FileDropInput.Attach(this, sender);
        _sender.Files.CollectionChanged += FilesChanged;
        L.Changed += LanguageChanged;
        Closed += (_, _) =>
        {
            _sender.Files.CollectionChanged -= FilesChanged;
            L.Changed -= LanguageChanged;
        };
        RefreshFileCount();
    }

    private void FilesChanged(object? sender, NotifyCollectionChangedEventArgs e) => RefreshFileCount();
    private void LanguageChanged(object? sender, EventArgs e) => RefreshFileCount();
    private void RefreshFileCount() => OutgoingFileCount.Text = L.F(" · {0} 个文件", _sender.Files.Count);
}
