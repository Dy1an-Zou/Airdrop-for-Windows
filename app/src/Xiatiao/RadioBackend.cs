// SPDX-License-Identifier: MIT
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Winawdl.Xiatiao;

/// <summary>Release-pinned radio profiles. A package selects one backend; IPC cannot select an executable.</summary>
internal sealed record RadioBackend(string Id, string Name, string Hardware, string Executable,
    string ReadyMarker, string Channel, string SourceRuntime, IReadOnlyDictionary<string, string> Pins)
{
    internal bool IsRealtek => Id == "rtl8852ce";
    internal const string RealtekDriverSha = "ed0bc40efe1fa5d069ec9c42ab5f8181de9981b986aed710c92ebb5d615c23d7";
    internal const string RealtekClass = "{42c93386-3172-4834-9a73-d5dbe38443f8}";
    internal static readonly RadioBackend MediaTek = new("mt7925", "MT7925", RadioHost.SupportedHardware,
        "winawdl_mt7925.exe", "Local radio initialized;", "6", "mt7925-transfer-runtime-r39",
        new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["runtime/awdld.exe"] = "cc4cc3bdffb8ae3ed18dd9d7d7056fd1769ba1e55b9338619a2eb552d5f29130",
            ["runtime/winawdl_mt7925.exe"] = "79e1bc7422c699cd6b0dfd378c9c910dc1d6d3654d748aa904e7232e9f880df2",
            ["runtime/wintun.dll"] = "e5da8447dc2c320edc0fc52fa01885c103de8c118481f683643cacc3220dafce",
            ["firmware/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin"] = "8eb46014d2a6b4124472eee7476d995008a6f40b1daffef87eb42f30d98699e1",
            ["firmware/WIFI_RAM_CODE_MT7925_1_1.bin"] = "23ff53b4bb639b30481e2e06bb1688569ad1ba971b897936db539882abfbd120",
        });
    internal static readonly RadioBackend Realtek = new("rtl8852ce", "RTL8852CE", @"PCI\VEN_10EC&DEV_C852",
        "winawdl_radio.exe", "radio: ready on channel 149, waiting for a client", "149", "rtl8852ce-winawdl-v0.3.0",
        new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["runtime/awdld.exe"] = "ae9c4ba3eb8583e3792b4d6c10cd40191c44ed8440d1bc942cbafeb99cd91af9",
            ["runtime/winawdl_radio.exe"] = "55ec78b2a3e632f4b022c444085660a9915286b2ce9d7549d68dbd69c1bdfb77",
            ["runtime/wintun.dll"] = "e5da8447dc2c320edc0fc52fa01885c103de8c118481f683643cacc3220dafce",
        });

    internal static RadioBackend FromManifest(JsonElement manifest)
    {
        if (manifest.GetProperty("schema").GetString() != "xiatiao.runtime.v1")
            throw new InvalidDataException("无线助手安装清单版本不匹配");
        var id = manifest.TryGetProperty("radioBackend", out var backend) ? backend.GetString() : "mt7925";
        var selected = id switch
        {
            "mt7925" => MediaTek,
            "rtl8852ce" => Realtek,
            _ => throw new InvalidDataException("未知的无线后端；没有启动网卡"),
        };
        if (manifest.GetProperty("sourceRuntime").GetString() != selected.SourceRuntime)
            throw new InvalidDataException("无线后端与运行文件来源不一致");
        return selected;
    }

    internal string[] RadioArguments(string installFolder) => IsRealtek ? ["149", "--width", "80"] :
        ["--run", "--firmware", Path.Combine(installFolder, "firmware"), "--country", "00", "--channel", "6",
         "--power-half-dbm", "44", "--fixed-channel"];
    internal string[] AwdlArguments => ["-c", Channel, "-r", "-s", "1", "-l"];

    internal static string ValidateRealtekInstance(string? value)
    {
        if (value is null || value.Length > 240 || !Regex.IsMatch(value,
            @"\APCI\\VEN_10EC&DEV_C852(?:&SUBSYS_[0-9A-F]{8})?(?:&REV_[0-9A-F]{2})?\\[A-Z0-9&_]+\z",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant))
            throw new InvalidDataException("RTL8852CE 网卡标识无效");
        return value;
    }

    internal static string ValidateRealtekDevice(JsonElement device)
    {
        var instance = ValidateRealtekInstance(device.GetProperty("instance").GetString());
        if (device.GetProperty("status").GetString() != "OK" || device.GetProperty("problem").GetInt32() != 0 ||
            !string.Equals(device.GetProperty("service").GetString(), "WUDFRd", StringComparison.OrdinalIgnoreCase) ||
            !Guid.TryParse(device.GetProperty("classGuid").GetString(), out var classId) || classId != Guid.Parse(RealtekClass) ||
            !device.GetProperty("hardwareIds").EnumerateArray().Any(id => id.GetString() is { } text && Regex.IsMatch(text,
                @"\APCI\\VEN_10EC&DEV_C852(?:&SUBSYS_[0-9A-F]{8}(?:&REV_[0-9A-F]{2})?|&REV_[0-9A-F]{2}|&CC_[0-9A-F]{4}(?:[0-9A-F]{2})?)?\z",
                RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)))
            throw new IOException("RTL8852CE 的 winawdl_regproxy 实验驱动未就绪；没有修改网卡驱动");
        return instance;
    }

    internal static string RealtekDriverPath(string windows, string? inf)
    {
        var store = Path.GetFullPath(Path.Combine(windows, "System32", "DriverStore", "FileRepository")) + Path.DirectorySeparatorChar;
        if (inf is null || !Path.IsPathFullyQualified(inf) || !string.Equals(Path.GetFileName(inf), "winawdl_regproxy.inf", StringComparison.OrdinalIgnoreCase))
            throw new IOException("RTL8852CE 驱动来源不符合固定版本");
        var full = Path.GetFullPath(inf);
        if (!full.StartsWith(store, StringComparison.OrdinalIgnoreCase)) throw new IOException("RTL8852CE 驱动路径越界");
        return Path.Combine(Path.GetDirectoryName(full)!, "winawdl_regproxy.dll");
    }

    internal static string RealtekDriverImageHash(byte[] image)
    {
        // Upstream signs this DLL on each user's PC. Its code must match the
        // pinned unsigned image; only the PE checksum/certificate fields and
        // appended Authenticode certificate may differ. Not a generic PE parser.
        const int size = 110592, checksum = 352, certificate = 432;
        if (image.Length < size || image.Length > size + 1024 * 1024 ||
            BitConverter.ToInt32(image, 60) != 264 || BitConverter.ToUInt32(image, 264) != 0x4550 ||
            BitConverter.ToUInt16(image, 288) != 0x20b)
            throw new InvalidDataException("RTL8852CE 驱动映像格式不符合固定版本");
        var offset = BitConverter.ToUInt32(image, certificate);
        var bytes = BitConverter.ToUInt32(image, certificate + 4);
        if (offset == 0 ? bytes != 0 || image.Length != size :
            offset != size || bytes < 8 || (ulong)offset + bytes != (ulong)image.Length)
            throw new InvalidDataException("RTL8852CE 驱动签名区域无效");
        var normalized = image[..size];
        Array.Clear(normalized, checksum, 4);
        Array.Clear(normalized, certificate, 8);
        return Convert.ToHexStringLower(SHA256.HashData(normalized));
    }

    // RTL's upstream radio stops DMA during normal exit but exposes no MT7925-style
    // STOP IOCTL. Require a fully initialized session, normal zero exit, unchanged
    // driver identity, and cleanup. Do not invent an independent DMA-quiescence claim.
    internal bool StopConfirmed(bool processes, bool ready, bool marker, bool driver, bool adapters, bool rules) =>
        processes && (IsRealtek ? ready : marker) && driver && adapters && rules;
}
