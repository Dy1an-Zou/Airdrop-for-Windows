// SPDX-License-Identifier: MIT
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Data;
using System.Windows.Markup;

namespace Winawdl.Xiatiao;

public sealed record LanguageOption(string Code, string Name);

/// <summary>One source-language catalog; only application text is translated, never paths or device names.</summary>
public static class L
{
    private static readonly Dictionary<string, string> English = Load();
    private static readonly Regex Placeholder = new(@"\{(\d+)(?::[^}]*)?\}", RegexOptions.CultureInvariant);
    private static readonly (Regex Pattern, string Translation, int[] Indices)[] Templates = English
        .Where(pair => Placeholder.IsMatch(pair.Key)).Select(pair => MakeTemplate(pair.Key, pair.Value)).ToArray();
    public static readonly LanguageOption[] Options = [new("zh-CN", "简体中文"), new("en", "English")];
    public static LocalizationSource Current { get; } = new();
    public static string Language { get; private set; } = "zh-CN";
    public static bool IsEnglish => Language == "en";
    public static CultureInfo Culture => CultureInfo.GetCultureInfo(IsEnglish ? "en-US" : "zh-CN");
    public static event EventHandler? Changed;
    public static string Normalize(string? language) => language is "en" or "en-US" ? "en" : "zh-CN";
    public static void SetLanguage(string? language)
    {
        var next = Normalize(language);
        if (Language == next) return;
        Language = next; Current.Notify(); Changed?.Invoke(null, EventArgs.Empty);
    }
    public static string T(string source)
    {
        if (source == "Exactly one supported dedicated MT7925 adapter is required.")
            return IsEnglish ? source : "需要且仅支持一块专用 MT7925 网卡。";
        if (!IsEnglish || string.IsNullOrEmpty(source)) return source;
        if (English.TryGetValue(source, out var translated)) return translated;
        // Runtime progress messages retain their original wording so switching
        // languages does not restart a transfer or freeze an old-language label.
        foreach (var (pattern, translation, indices) in Templates)
        {
            var match = pattern.Match(source);
            if (!match.Success) continue;
            var values = new object[indices.Max() + 1];
            for (var i = 0; i < indices.Length; i++) values[indices[i]] = match.Groups[i + 1].Value;
            return string.Format(Culture, translation, values);
        }
        return source;
    }
    public static string F(string source, params object[] values)
    {
        var template = T(source);
        if (IsEnglish && values.Length > 0 && values[0] is int count && count == 1)
            template = template.Replace("{0} files", "{0} file", StringComparison.Ordinal)
                .Replace("{0} selected files", "{0} selected file", StringComparison.Ordinal)
                .Replace("{0} file paths", "{0} file path", StringComparison.Ordinal)
                .Replace("{0} history entries", "{0} history entry", StringComparison.Ordinal);
        return string.Format(Culture, template, values);
    }
    public static string Error(string source) => TranslateError(source, 0);
    private static string TranslateError(string source, int depth)
    {
        if (!IsEnglish || depth >= 4) return T(source);
        // Only known error wrappers recurse. Captured file names, device names
        // and paths in ordinary messages are never passed back for translation.
        var prefixes = new[] { "无线助手未启动：", "关闭请求未确认：", "无线助手连接中断：", "本次传输中断：", "接收服务：",
            "自启动设置未保存：", "语言设置未保存：", "开启无线连接失败：", "检查专用 MT7925 网卡和驱动失败：",
            "检查专用 RTL8852CE 网卡和驱动失败：", "检查无线连接是否被占用失败：", "检查上次连接的清理状态失败：", "重新初始化专用 MT7925 网卡失败：",
            "确认专用网卡已重新就绪失败：", "启动专用网卡固件失败：", "建立 AirDrop 网络接口失败：",
            "确认 AirDrop 网络接口已就绪失败：", "配置并核验文件传输通道失败：", "配置并核验设备发现通道失败：" };
        const string suffix = "；停止未完整确认";
        if (source.EndsWith(suffix, StringComparison.Ordinal)) return TranslateError(source[..^suffix.Length], depth + 1) + T(suffix);
        foreach (var prefix in prefixes)
            if (source.StartsWith(prefix, StringComparison.Ordinal))
                return F(prefix + "{0}", TranslateError(source[prefix.Length..], depth + 1));
        return T(source);
    }
    public static void Bind(DependencyObject target, DependencyProperty property, string source) =>
        BindingOperations.SetBinding(target, property, Binding(source));
    internal static Binding Binding(string source) => new(nameof(LocalizationSource.Revision))
    { Source = Current, Mode = BindingMode.OneWay, Converter = TextConverter.Instance, ConverterParameter = source };
    internal static IReadOnlyDictionary<string, string> Catalog => English;

    private static Dictionary<string, string> Load()
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        var assembly = typeof(L).Assembly;
        foreach (var name in assembly.GetManifestResourceNames().Where(name => name.Contains("Strings.", StringComparison.Ordinal) && name.EndsWith(".en.json", StringComparison.Ordinal)).Order())
        {
            using var stream = assembly.GetManifestResourceStream(name) ?? throw new IOException("Missing language catalog: " + name);
            var entries = JsonSerializer.Deserialize<Dictionary<string, string>>(stream) ?? throw new InvalidDataException("Invalid language catalog: " + name);
            foreach (var pair in entries)
            {
                if (result.TryGetValue(pair.Key, out var existing) && existing != pair.Value)
                    throw new InvalidDataException("Conflicting translation: " + pair.Key);
                result[pair.Key] = pair.Value;
            }
        }
        return result;
    }
    private static (Regex, string, int[]) MakeTemplate(string source, string translation)
    {
        var pattern = new System.Text.StringBuilder(@"\A");
        var indices = new List<int>(); var offset = 0;
        foreach (Match field in Placeholder.Matches(source))
        {
            pattern.Append(Regex.Escape(source[offset..field.Index])).Append("(.*?)");
            indices.Add(int.Parse(field.Groups[1].Value, CultureInfo.InvariantCulture));
            offset = field.Index + field.Length;
        }
        pattern.Append(Regex.Escape(source[offset..])).Append(@"\z");
        return (new Regex(pattern.ToString(), RegexOptions.CultureInvariant | RegexOptions.Singleline, TimeSpan.FromMilliseconds(100)), translation, indices.ToArray());
    }
}

public sealed class LocalizationSource : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;
    public int Revision { get; private set; }
    internal void Notify() { Revision++; PropertyChanged?.Invoke(this, new(nameof(Revision))); }
}

[MarkupExtensionReturnType(typeof(string))]
public sealed class LocExtension : MarkupExtension
{
    public LocExtension() { }
    public LocExtension(string text) => Text = text;
    [ConstructorArgument("text")] public string Text { get; set; } = "";
    public override object ProvideValue(IServiceProvider serviceProvider) => L.Binding(Text).ProvideValue(serviceProvider);
}

internal sealed class TextConverter : IValueConverter
{
    internal static readonly TextConverter Instance = new();
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture) => L.T((string)parameter);
    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture) => Binding.DoNothing;
}
