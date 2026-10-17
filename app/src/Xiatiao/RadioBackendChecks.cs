// SPDX-License-Identifier: MIT
using System.IO;
using System.Text.Json;

namespace Winawdl.Xiatiao;

internal static class RadioBackendChecks
{
    // No installed app, driver, process, adapter, or network is opened.
    internal static int Run(string? driverFile = null)
    {
        int checks = 0;
        void Expect(bool value) { checks++; if (!value) throw new Exception("backend check " + checks); }
        void Reject(Action action)
        {
            bool rejected = false;
            try { action(); } catch (Exception e) when (e is IOException or InvalidDataException or JsonException or InvalidOperationException or ArgumentException) { rejected = true; }
            Expect(rejected);
        }
        JsonElement Json(object value) => JsonSerializer.SerializeToElement(value);
        try
        {
            var mt = RadioBackend.MediaTek;
            var rtl = RadioBackend.Realtek;
            Expect(RadioBackend.FromManifest(Json(new { schema = "xiatiao.runtime.v1", sourceRuntime = mt.SourceRuntime })) == mt);
            foreach (var profile in new[] { mt, rtl })
            {
                Expect(RadioBackend.FromManifest(Json(new { schema = "xiatiao.runtime.v1", sourceRuntime = profile.SourceRuntime, radioBackend = profile.Id })) == profile);
                Reject(() => RadioBackend.FromManifest(Json(new { schema = "other", sourceRuntime = profile.SourceRuntime, radioBackend = profile.Id })));
                Reject(() => RadioBackend.FromManifest(Json(new { schema = "xiatiao.runtime.v1", sourceRuntime = "other", radioBackend = profile.Id })));
                Expect(profile.Pins.ContainsKey("runtime/" + profile.Executable));
                Expect(profile.Pins.All(p => p.Value.Length == 64 && p.Value.All(Uri.IsHexDigit)));
            }
            foreach (var id in new string?[] { null, "", "RTL8852CE", "rtl8852be", "../other" })
                Reject(() => RadioBackend.FromManifest(Json(new { schema = "xiatiao.runtime.v1", sourceRuntime = rtl.SourceRuntime, radioBackend = id })));
            Reject(() => RadioBackend.FromManifest(Json(new { schema = "xiatiao.runtime.v1", sourceRuntime = rtl.SourceRuntime })));
            Expect(rtl.RadioArguments(@"C:\Program Files\XiatiaoAirDrop").SequenceEqual(new[] { "149", "--width", "80" }));
            Expect(rtl.AwdlArguments.SequenceEqual(new[] { "-c", "149", "-r", "-s", "1", "-l" }));
            Expect(mt.RadioArguments(@"C:\Program Files\XiatiaoAirDrop").Contains("--fixed-channel"));
            Expect(mt.AwdlArguments[1] == "6" && !mt.Pins.ContainsKey("runtime/winawdl_radio.exe"));
            const string instance = @"PCI\VEN_10EC&DEV_C852&SUBSYS_12345678&REV_01\6&ABC&0&0020";
            Expect(RadioBackend.ValidateRealtekInstance(instance) == instance);
            foreach (var bad in new[] { instance.Replace("C852", "B852"), instance.Replace("10EC", "14C3"), instance + "'", instance + "\\other", instance.Replace("12345678", "1234"), @"PCI\VEN_10EC&DEV_C852" })
                Reject(() => RadioBackend.ValidateRealtekInstance(bad));
            var fields = new Dictionary<string, object?> { ["instance"] = instance, ["status"] = "OK", ["service"] = "WUDFRd", ["problem"] = 0, ["classGuid"] = RadioBackend.RealtekClass, ["hardwareIds"] = new[] { rtl.Hardware } };
            Expect(RadioBackend.ValidateRealtekDevice(Json(fields)) == instance);
            var fullIds = new Dictionary<string, object?>(fields) { ["hardwareIds"] = new[] { rtl.Hardware + "&SUBSYS_12345678&REV_01", rtl.Hardware + "&CC_028000" } };
            Expect(RadioBackend.ValidateRealtekDevice(Json(fullIds)) == instance);
            foreach (var invalid in new Dictionary<string, object?> { ["status"] = "ERROR", ["service"] = "rtwlane", ["problem"] = 10, ["classGuid"] = "{00000000-0000-0000-0000-000000000000}", ["hardwareIds"] = new[] { @"PCI\VEN_10EC&DEV_B852" } })
            {
                var copy = new Dictionary<string, object?>(fields) { [invalid.Key] = invalid.Value };
                Reject(() => RadioBackend.ValidateRealtekDevice(Json(copy)));
            }
            const string driverInf = @"C:\Windows\System32\DriverStore\FileRepository\winawdl_regproxy.inf_amd64_test\winawdl_regproxy.inf";
            Expect(RadioBackend.RealtekDriverPath(@"C:\Windows", driverInf).EndsWith(@"\winawdl_regproxy.dll"));
            foreach (var bad in new[] { @"C:\Temp\winawdl_regproxy.inf", driverInf.Replace("winawdl_regproxy.inf", "other.inf"), @"..\winawdl_regproxy.inf", @"C:\Windows\System32\DriverStore\FileRepository\..\winawdl_regproxy.inf" })
                Reject(() => RadioBackend.RealtekDriverPath(@"C:\Windows", bad));
            // Exercise the real unsigned upstream image when supplied; otherwise use
            // a PE-shaped fixture. Never load the DLL or write to its source file.
            var image = driverFile is null ? new byte[110592] : File.ReadAllBytes(driverFile);
            void Put(byte[] target, int offset, uint value) => BitConverter.GetBytes(value).CopyTo(target, offset);
            if (driverFile is null) { Put(image, 60, 264); Put(image, 264, 0x4550); image[288] = 0x0b; image[289] = 2; }
            var digest = RadioBackend.RealtekDriverImageHash(image);
            if (driverFile is not null) Expect(digest == RadioBackend.RealtekDriverSha);
            var signed = new byte[image.Length + 16]; image.CopyTo(signed, 0);
            Put(signed, 352, 123456); Put(signed, 432, 110592); Put(signed, 436, 16);
            Expect(RadioBackend.RealtekDriverImageHash(signed) == digest);
            var tampered = (byte[])signed.Clone(); tampered[10000] ^= 1;
            Expect(RadioBackend.RealtekDriverImageHash(tampered) != digest);
            foreach (var change in new[] { (60, 265u), (264, 0u), (432, 110591u), (436, 15u), (436, uint.MaxValue) })
            {
                var broken = (byte[])signed.Clone(); Put(broken, change.Item1, change.Item2);
                Reject(() => RadioBackend.RealtekDriverImageHash(broken));
            }
            Reject(() => RadioBackend.RealtekDriverImageHash(image[..100]));
            Reject(() => RadioBackend.RealtekDriverImageHash(new byte[1159169]));
            var trailer = new byte[110593]; image.CopyTo(trailer, 0);
            Reject(() => RadioBackend.RealtekDriverImageHash(trailer));
            Expect(rtl.StopConfirmed(true, true, false, true, true, true));
            Expect(!rtl.StopConfirmed(true, false, true, true, true, true));
            Expect(mt.StopConfirmed(true, false, true, true, true, true));
            Expect(!mt.StopConfirmed(true, true, false, true, true, true));
            foreach (var profile in new[] { mt, rtl })
                foreach (var gate in new[] { 0, 3, 4, 5 })
                {
                    var gates = Enumerable.Repeat(true, 6).ToArray(); gates[gate] = false;
                    Expect(!profile.StopConfirmed(gates[0], gates[1], gates[2], gates[3], gates[4], gates[5]));
                }
            Console.WriteLine(JsonSerializer.Serialize(new { checks, passed = true, radioStarted = false, systemChanged = false }));
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
}
