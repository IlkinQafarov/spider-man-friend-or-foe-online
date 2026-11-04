using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32.SafeHandles;

// Self-contained player bundle bootstrapper. Its archive has no path metadata supplied by the input.
internal static class Bundle
{
    private sealed class Item { public string Path; public long Size; public string Hash; public byte[] Data; }
    private static readonly string[] Core = new string[] {
        "launcher/fof-private-launcher.exe", "probe/native-launcher.exe",
        "probe/native-probe.dll", "probe/d3d9.dll", "probe/version.dll",
        "probe/safedisc-source.zip", "probe/SAFE-DISC-LICENSE.txt", "steam/native/native-relay.exe",
        "steam/native/steam_appid.txt", "compatibility/baseline.json"
    };
    private static readonly string[] Rooftop = new string[] {
        "launcher/assets/rooftop/distant-city.png", "launcher/assets/rooftop/swing-sign.png", "launcher/assets/rooftop/front.png",
        "launcher/assets/rooftop/live-plates.png"
    };
    private const string Magic = "FOFBNDL1";

    [STAThread]
    private static int Main(string[] args)
    {
        try {
            if (args.Length == 1 && args[0] == "--selftest") return SelfTest();
            if (args.Length == 2 && args[0] == "--extract-selftest") return ExtractSelfTest(args[1]);
            if (args.Length != 0) throw new InvalidDataException("This app has no command-line options.");
            Run(); return 0;
        } catch (Exception ex) {
            try { MessageBox.Show("The bundled app could not start.\r\n\r\n" + ex.Message, "Friend or Foe", MessageBoxButtons.OK, MessageBoxIcon.Error); } catch { }
            return 1;
        }
    }

    private static void Run()
    {
        string exe = Assembly.GetExecutingAssembly().Location;
        string parent = Path.GetDirectoryName(exe);
        string root = Path.Combine(parent, "FriendOrFoe");
        using (Mutex mutex = new Mutex(false, "Local\\FoFBundle-" + HashText(Path.GetFullPath(root).ToUpperInvariant()))) {
            bool held = false;
            try {
                try { held = mutex.WaitOne(0); }
                catch (AbandonedMutexException) { held = true; }
                if (!held) throw new IOException("Friend or Foe is already open.");
                Extract(root, ReadEmbedded());
                string player = Path.Combine(root, "launcher\\fof-private-launcher.exe");
                CheckTree(root);
                if (!File.Exists(player) || IsReparse(File.GetAttributes(player))) throw new IOException("The extracted launcher is missing or unsafe.");
                ProcessStartInfo start = new ProcessStartInfo(player);
                start.WorkingDirectory = root;
                using (Process child = Process.Start(start)) { if (child == null) throw new IOException("Could not open the player launcher."); child.WaitForExit(); }
            } finally { if (held) mutex.ReleaseMutex(); }
        }
    }

    private static List<Item> ReadEmbedded()
    {
        Assembly a = Assembly.GetExecutingAssembly();
        using (Stream s = a.GetManifestResourceStream("fof.bundle")) {
            if (s == null) throw new InvalidDataException("Embedded package is missing.");
            using (BinaryReader r = new BinaryReader(s, Encoding.UTF8)) {
                if (Encoding.ASCII.GetString(r.ReadBytes(8)) != Magic) throw new InvalidDataException("Invalid embedded package.");
                int count = r.ReadInt32();
                if (count != Core.Length + Rooftop.Length) throw new InvalidDataException("Invalid embedded file count.");
                List<Item> items = new List<Item>(); HashSet<string> names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                for (int i = 0; i < count; i++) {
                    string path = r.ReadString(); long size = r.ReadInt64(); string hash = r.ReadString();
                    if (!Allowed(path) || !names.Add(path) || size < 0 || size > 1024L * 1024 * 1024 || hash.Length != 64)
                        throw new InvalidDataException("Invalid embedded entry.");
                    Item item = new Item(); item.Path = path; item.Size = size; item.Hash = hash;
                    item.Data = ReadExact(r, size);
                    if (!String.Equals(HashBytes(item.Data), hash, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Embedded file hash mismatch: " + path);
                    items.Add(item);
                }
                foreach (string required in Core) if (!names.Contains(required)) throw new InvalidDataException("Required embedded file is missing.");
                if (s.Position != s.Length) throw new InvalidDataException("Trailing embedded data.");
                return items;
            }
        }
    }

    private static bool Allowed(string path)
    {
        foreach (string p in Core) if (String.Equals(p, path, StringComparison.Ordinal)) return true;
        foreach (string p in Rooftop) if (String.Equals(p, path, StringComparison.Ordinal)) return true;
        return false;
    }

    private static void Extract(string root, List<Item> items)
    {
        CheckExistingParents(Path.GetDirectoryName(root));
        if (!Directory.Exists(root)) Directory.CreateDirectory(root);
        CheckTree(root);
        for (int i = 0; i < items.Count; i++) {
            Item item = items[i];
            string target = SafeTarget(root, item.Path);
            EnsureParent(root, Path.GetDirectoryName(target));
            if (File.Exists(target)) {
                if (IsReparse(File.GetAttributes(target))) throw new IOException("Unsafe existing bundle file.");
                byte[] old = File.ReadAllBytes(target);
                if (String.Equals(HashBytes(old), item.Hash, StringComparison.OrdinalIgnoreCase)) continue;
            } else if (Directory.Exists(target)) throw new IOException("File path is an existing directory.");
            string temp = target + ".fof-new-" + Guid.NewGuid().ToString("N");
            try {
                using (FileStream fs = new FileStream(temp, FileMode.CreateNew, FileAccess.Write, FileShare.None)) { fs.Write(item.Data, 0, item.Data.Length); fs.Flush(true); }
                if (File.Exists(target)) File.Replace(temp, target, null); else File.Move(temp, target);
            } finally { if (File.Exists(temp)) File.Delete(temp); }
        }
    }

    private static byte[] ReadExact(BinaryReader r, long size)
    {
        if (size > Int32.MaxValue) throw new InvalidDataException("Bundle entry is too large.");
        byte[] data = r.ReadBytes((int)size);
        if (data.Length != (int)size) throw new EndOfStreamException("Truncated embedded file.");
        return data;
    }

    private static string SafeTarget(string root, string relative)
    {
        if (!Allowed(relative) || relative.StartsWith("/") || relative.IndexOf('\\') >= 0 || relative.IndexOf(':') >= 0)
            throw new InvalidDataException("Unsafe bundle path.");
        string full = Path.GetFullPath(Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar)));
        string prefix = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (!full.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Bundle path escaped its directory.");
        return full;
    }

    private static void EnsureParent(string root, string parent)
    {
        string fullRoot = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar);
        string full = Path.GetFullPath(parent);
        if (!full.StartsWith(fullRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new IOException("Unsafe bundle directory.");
        string rel = full.Substring(fullRoot.Length + 1);
        string current = fullRoot;
        foreach (string part in rel.Split(Path.DirectorySeparatorChar)) {
            current = Path.Combine(current, part);
            if (Directory.Exists(current)) { if (IsReparse(File.GetAttributes(current))) throw new IOException("Reparse directory in bundle path."); }
            else if (File.Exists(current)) throw new IOException("File blocks bundle directory.");
            else Directory.CreateDirectory(current);
        }
    }

    private static void CheckExistingParents(string path)
    {
        string p = Path.GetFullPath(path);
        while (!String.IsNullOrEmpty(p)) {
            if (Directory.Exists(p) && IsReparse(File.GetAttributes(p))) throw new IOException("Reparse directory in bundle location.");
            p = Path.GetDirectoryName(p);
        }
    }
    private static void CheckTree(string root) { CheckExistingParents(root); }
    private static bool IsReparse(FileAttributes a) { return (a & FileAttributes.ReparsePoint) != 0; }
    private static string HashBytes(byte[] b) { using (SHA256 h = SHA256.Create()) return Hex(h.ComputeHash(b)); }
    private static string HashText(string s) { return HashBytes(Encoding.UTF8.GetBytes(s)); }
    private static string Hex(byte[] b) { StringBuilder s = new StringBuilder(); foreach (byte x in b) s.Append(x.ToString("x2")); return s.ToString(); }

    private static int SelfTest()
    {
        List<Item> embedded = ReadEmbedded();
        if (!Allowed(Core[0]) || !Allowed(Rooftop[2]) || Allowed("profiles/settings.json") || Allowed("assets/rooftop/other.png"))
            throw new Exception("Allowlist fixture failed");
        foreach (string rooftop in Rooftop) {
            Item layer = embedded.Find(delegate(Item x) { return x.Path == rooftop; });
            if (layer == null || !String.Equals(HashBytes(layer.Data), layer.Hash, StringComparison.OrdinalIgnoreCase))
                throw new Exception("Rooftop layer resource hash fixture failed: " + rooftop);
        }
        string temp = Path.Combine(Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location), "own-bundle-selftest-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temp);
        try {
            string root = Path.Combine(temp, "FriendOrFoe");
            Directory.CreateDirectory(root);
            string profile = Path.Combine(root, "profiles"); Directory.CreateDirectory(profile);
            string setting = Path.Combine(profile, "ui.json"); File.WriteAllText(setting, "synthetic user data");
            string display = Path.Combine(root, "display.json"); File.WriteAllText(display, "synthetic display data");
            string[] playerFiles={"profile/game-settings.dat","test/English/own-import-sentinel.dat","sessions/own-session-sentinel.log","ui-language.json"};
            foreach(string relative in playerFiles){string path=Path.Combine(root,relative);Directory.CreateDirectory(Path.GetDirectoryName(path));File.WriteAllText(path,"synthetic retained player data");}
            Extract(root, embedded);
            byte[] fixture = Encoding.UTF8.GetBytes("synthetic own-code fixture");
            string target = SafeTarget(root, "launcher/fof-private-launcher.exe");
            File.WriteAllBytes(target, fixture);
            Extract(root, embedded); // Re-extract the actual bundle as an upgrade over a modified runtime file.
            Item expected = embedded.Find(delegate(Item x) { return x.Path == "launcher/fof-private-launcher.exe"; });
            if (expected == null || HashBytes(File.ReadAllBytes(target)) != expected.Hash) throw new Exception("Upgrade hash fixture failed");
            if (File.ReadAllText(setting) != "synthetic user data" || File.ReadAllText(display) != "synthetic display data")
                throw new Exception("User settings preservation fixture failed");
            foreach(string relative in playerFiles)if(File.ReadAllText(Path.Combine(root,relative))!="synthetic retained player data")throw new Exception("Player data preservation fixture failed: "+relative);
            bool rejected = false; try { SafeTarget(root, "../escape"); } catch (InvalidDataException) { rejected = true; }
            if (!rejected) throw new Exception("Traversal fixture failed");
            Console.WriteLine("BUNDLE_SELFTEST_PASS embedded-framing/digests/allowlist/traversal/upgrade/settings-profile-import-session-preserved"); return 0;
        } finally { try { Directory.Delete(temp, true); } catch { } }
    }

    private static int ExtractSelfTest(string output)
    {
        string root = Path.GetFullPath(output);
        if (Directory.Exists(root) || File.Exists(root)) throw new IOException("Extraction selftest requires a new output directory.");
        CheckExistingParents(Path.GetDirectoryName(root));
        Extract(root, ReadEmbedded());
        Console.WriteLine("BUNDLE_EXTRACT_SELFTEST_PASS " + root);
        return 0;
    }
}
