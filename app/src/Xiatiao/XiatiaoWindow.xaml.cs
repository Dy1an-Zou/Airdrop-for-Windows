using System;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace Winawdl.Xiatiao;

/// <summary>The view owns presentation only; receive and storage decisions live in AppController.</summary>
public partial class XiatiaoWindow : Window
{
    private readonly AppController _model;
    private ImageSource? _brand;
    private Window? _about;
    private bool _settingsManuallySelected;

    public XiatiaoWindow(AppController model)
    {
        _model = model;
        InitializeComponent();
        DataContext = model;
        L.Changed += LanguageChanged;
        Closed += (_, _) => L.Changed -= LanguageChanged;
        IsVisibleChanged += (_, _) => { if (!IsVisible) _sendWindow?.Close(); };
        var artwork = Path.Combine(AppContext.BaseDirectory, "assets", "xiatiao-airdrop.png");
        if (File.Exists(artwork))
        {
            var bitmap = new BitmapImage();
            bitmap.BeginInit();
            bitmap.CacheOption = BitmapCacheOption.OnLoad;
            bitmap.UriSource = new Uri(artwork, UriKind.Absolute);
            bitmap.EndInit();
            bitmap.Freeze();
            BrandLogo.Source = _brand = bitmap;
        }
        var icon = Path.Combine(AppContext.BaseDirectory, "assets", "xiatiao-airdrop.ico");
        if (File.Exists(icon)) Icon = BitmapFrame.Create(new Uri(icon, UriKind.Absolute));
    }

    private void AboutClicked(object sender, RoutedEventArgs e) => ShowAbout();

    private void LanguageChanged(object? sender, EventArgs e)
    {
        SetSettingsExpanded(SettingsScroller.Visibility == Visibility.Visible);
        RefreshFileSummary();
    }

    private void WindowSizeChanged(object sender, SizeChangedEventArgs e)
    {
        ApplyHeight(ActualHeight > 0 ? ActualHeight : Height);
    }

    internal void ApplyHeight(double height)
    {
        if (SettingsScroller is null) return;
        // Reserve the file shelf; an explicitly opened small-window settings pane scrolls.
        SettingsScroller.MaxHeight = Math.Clamp(height - 620, 140, 240);
        if (!_settingsManuallySelected) SetSettingsExpanded(height >= 820);
    }

    private void SettingsClicked(object sender, RoutedEventArgs e)
    {
        _settingsManuallySelected = true;
        SetSettingsExpanded(SettingsScroller.Visibility != Visibility.Visible);
    }

    private void SetSettingsExpanded(bool expanded)
    {
        SettingsScroller.Visibility = expanded ? Visibility.Visible : Visibility.Collapsed;
        SettingsButton.Content = L.T(expanded ? "收起设置" : "设置");
        System.Windows.Automation.AutomationProperties.SetHelpText(SettingsButton,
            L.T(expanded ? "设置已展开，可收起以留出更多文件空间" : "设置已收起，可展开更改名称、储存位置与开机启动"));
    }

    // Called only by explicit About actions (window or tray); never by background events.
    public void ShowAbout()
    {
        if (_about is not null) { _about.Activate(); return; }
        var dialog = CreateAboutWindow();
        if (IsVisible) dialog.Owner = this;
        else dialog.WindowStartupLocation = WindowStartupLocation.CenterScreen;
        _about = dialog;
        try { dialog.ShowDialog(); }
        finally { _about = null; }
    }

    internal Window CreateAboutWindow()
    {
        var dialog = new Window
        {
            Width = 470, Height = 620,
            MinWidth = 420, MinHeight = 540, ResizeMode = ResizeMode.CanResize,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            FontFamily = FontFamily, Background = (Brush)FindResource("CanvasBrush"),
            Foreground = (Brush)FindResource("InkBrush"), Icon = Icon,
            ShowInTaskbar = false, UseLayoutRounding = true
        };
        L.Bind(dialog, Window.TitleProperty, "关于 Airdrop for Windows");
        dialog.Resources.MergedDictionaries.Add(Resources);
        var layout = new Grid { Margin = new Thickness(28, 22, 28, 22) };
        layout.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        layout.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        // Cards/alexruix_tough-shrimp-75: image-led centered identity, without its animation.
        var body = new StackPanel { HorizontalAlignment = HorizontalAlignment.Stretch };
        body.Children.Add(new Image { Source = _brand, Height = 140, Width = 140, Margin = new Thickness(0, 5, 0, 12), Stretch = Stretch.Uniform });
        body.Children.Add(Centered("Airdrop for Windows", 25, FontWeights.SemiBold));
        body.Children.Add(Centered("作者  Dy1an-Zou", 15, FontWeights.Medium, 22));
        body.Children.Add(Centered("版本 0.2.4", 13, FontWeights.Normal, 7));
        body.Children.Add(Centered("基于 winawdl 及开放源代码项目构建", 13, FontWeights.Normal, 22));
        body.Children.Add(LinkButton("winawdl · 上游项目", "https://github.com/raph559/winawdl"));
        body.Children.Add(LinkButton("Uiverse / Galaxy · 界面组件来源", "https://github.com/uiverse-io/galaxy"));
        var notices = new Button
        {
            Command = _model.OpenNoticesCommand,
            Style = (Style)FindResource("QuietButton"), HorizontalAlignment = HorizontalAlignment.Center,
            Margin = new Thickness(0, 3, 0, 0)
        };
        L.Bind(notices, ContentControl.ContentProperty, "第三方项目与许可证");
        body.Children.Add(notices);
        body.Children.Add(Centered("AirDrop 是 Apple Inc. 的商标。\n本项目为独立软件，与 Apple 无关联或背书。", 11, FontWeights.Normal, 17));
        var scroller = new ScrollViewer { Content = body, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        layout.Children.Add(scroller);
        var done = new Button
        {
            Width = 100, IsDefault = true, IsCancel = true,
            Style = (Style)FindResource("PrimaryButton"), HorizontalAlignment = HorizontalAlignment.Right,
            Margin = new Thickness(0, 18, 0, 0)
        };
        L.Bind(done, ContentControl.ContentProperty, "确定");
        done.Click += (_, _) => dialog.Close();
        Grid.SetRow(done, 1);
        layout.Children.Add(done);
        dialog.Content = layout;
        return dialog;
    }

    private TextBlock Centered(string text, double size, FontWeight weight, double top = 0)
    {
        var block = new TextBlock
        {
            FontSize = size, FontWeight = weight,
            TextAlignment = TextAlignment.Center, TextWrapping = TextWrapping.Wrap,
            Foreground = (Brush)FindResource(size <= 13 ? "MutedBrush" : "InkBrush"),
            Margin = new Thickness(0, top, 0, 0), LineHeight = size * 1.55
        };
        L.Bind(block, TextBlock.TextProperty, text);
        return block;
    }

    private Button LinkButton(string text, string url)
    {
        var button = new Button
        {
            Style = (Style)FindResource("QuietButton"),
            HorizontalAlignment = HorizontalAlignment.Center,
            Margin = new Thickness(0, 3, 0, 0), FontSize = 12, ToolTip = url
        };
        L.Bind(button, ContentControl.ContentProperty, text);
        button.Click += (_, _) =>
        {
            try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); }
            catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
            {
                L.Bind(button, ContentControl.ContentProperty, "浏览器未能打开，请稍后重试");
            }
        };
        return button;
    }
}
