using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;

namespace FofPrivate {
sealed class Rejected : Exception { public Rejected(string message) : base(message) {} }
sealed class Record { public long Size; public string Sha; public Record(long size,string sha){Size=size;Sha=sha;} }
sealed class OwnedJob : IDisposable {
    [StructLayout(LayoutKind.Sequential)] struct Basic {public long ProcessTime,JobTime;public uint Flags;public UIntPtr MinWorkingSet,MaxWorkingSet;public uint ActiveLimit;public UIntPtr Affinity;public uint Priority,Scheduling;}
    [StructLayout(LayoutKind.Sequential)] struct Counters {public ulong ReadOps,WriteOps,OtherOps,ReadBytes,WriteBytes,OtherBytes;}
    [StructLayout(LayoutKind.Sequential)] struct Extended {public Basic Basic;public Counters Io;public UIntPtr ProcessMemory,JobMemory,PeakProcessMemory,PeakJobMemory;}
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateJobObject(IntPtr attributes,string name);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool SetInformationJobObject(IntPtr job,int information,ref Extended value,uint size);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool AssignProcessToJobObject(IntPtr job,IntPtr process);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    IntPtr handle;
    public OwnedJob(){handle=CreateJobObject(IntPtr.Zero,null);if(handle==IntPtr.Zero)throw new Rejected("Cannot create owned session job");var value=new Extended();value.Basic.Flags=0x2000;if(!SetInformationJobObject(handle,9,ref value,(uint)Marshal.SizeOf(typeof(Extended)))){Dispose();throw new Rejected("Cannot set owned session cleanup");}}
    public void Add(Process process){if(!AssignProcessToJobObject(handle,process.Handle))throw new Rejected("Cannot assign process to owned session job");}
    public void Dispose(){if(handle!=IntPtr.Zero){CloseHandle(handle);handle=IntPtr.Zero;}}
}
static class Ownership {
    [StructLayout(LayoutKind.Sequential)] struct BasicInfo {public IntPtr Exit,Peb,Affinity,Priority,Pid,ParentPid;}
    [DllImport("ntdll.dll")] static extern int NtQueryInformationProcess(IntPtr process,int information,ref BasicInfo value,int size,out int returned);
    delegate bool WindowCallback(IntPtr window,IntPtr context);
    [DllImport("user32.dll")] static extern bool EnumWindows(WindowCallback callback,IntPtr context);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window,uint message,IntPtr wParam,IntPtr lParam);
    public static bool HasParent(Process process,int parent){var value=new BasicInfo();int returned;return NtQueryInformationProcess(process.Handle,0,ref value,Marshal.SizeOf(typeof(BasicInfo)),out returned)==0&&value.ParentPid.ToInt64()==parent;}
    public static void RequestClose(Process process){if(process.HasExited)return;uint target=(uint)process.Id;EnumWindows((window,context)=>{uint pid;GetWindowThreadProcessId(window,out pid);if(pid==target&&!process.HasExited)PostMessage(window,0x10,IntPtr.Zero,IntPtr.Zero);return true;},IntPtr.Zero);}
}
sealed class FixtureWindow : Form {
    public FixtureWindow(){Opacity=0;ShowInTaskbar=false;Text="Own launcher fixture";}
    protected override bool ShowWithoutActivation{get{return true;}}
}
sealed class Report {
    public string schema="coop-fof-preflight-v1", status="pass", language, critical_digest, locale_digest;
    public int critical_files, locale_files;
}
static class Files {
    public const string BaselineSha="5c85b8c90c60b6d615a1aa50ae95d7b0acd32bbbe24bc68c282c136e5e7af42b";
    public const string Critical="c46b90afe36571cf3f4fbe3d4e7918da833297f78119d40504e96b4f531b36ea";
    public const string SteamSha="670d654aa3255c5061cf0236a1cdbb2f5076cbd5ae44f613b6f4921558e83e2d";
    public static readonly JavaScriptSerializer Json=new JavaScriptSerializer{MaxJsonLength=4000000};
    static readonly HashSet<string> Ignore=new HashSet<string>(("docs/cs/customer_support.htm|docs/license/license.txt|docs/manual/manual.pdf|docs/msr/msr.txt|docs/readme/readme.doc|gameicon.ico|icon_mediacenter.png|mc.exe|mediacenter.mcl|sm.ico|spider-man friend or foe.exe|unins000.dat|unins000.exe|uninstall.ico").Split('|'),StringComparer.Ordinal);
    public static string Root {get{return Path.GetFullPath(Path.Combine(AppDomain.CurrentDomain.BaseDirectory,".."));}}
    public static string Normalize(string path) {
        path=path.Replace('\\','/');
        if(String.IsNullOrEmpty(path)||path.StartsWith("/")||path.IndexOfAny(new[]{'\0','\n','\r',':'})>=0||path.Split('/').Any(p=>p==""||p=="."||p==".."))throw new Rejected("Invalid relative path");
        return path.ToLowerInvariant();
    }
    public static string NoReparse(string path) {
        string full=Path.GetFullPath(path);
        // Walk lexical ancestors before any path resolution can hide a junction.
        string node=full;
        while(!String.IsNullOrEmpty(node)) {
            if((File.GetAttributes(node)&FileAttributes.ReparsePoint)!=0)throw new Rejected("Reparse/symlink path rejected: "+Path.GetFileName(node));
            string next=Path.GetDirectoryName(node); if(next==node)break; node=next;
        }
        return full;
    }
    public static string Child(string path,string parent,bool exists) {
        string full=Path.GetFullPath(path), basis=Path.GetFullPath(parent).TrimEnd('\\')+"\\";
        if(!full.StartsWith(basis,StringComparison.OrdinalIgnoreCase))throw new Rejected("Path must stay inside "+Path.GetFileName(parent));
        NoReparse(exists?full:Path.GetDirectoryName(full));
        if(!exists&&(Directory.Exists(full)||File.Exists(full)))throw new Rejected("Output must be NEW");
        return full;
    }
    public static Record HashFile(string path) {
        NoReparse(path); var before=new FileInfo(path); long size=before.Length; DateTime changed=before.LastWriteTimeUtc;
        string sha;
        using(var stream=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read,4*1024*1024,FileOptions.SequentialScan))
        using(var hash=SHA256.Create()){sha=Hex(hash.ComputeHash(stream));if(stream.Length!=size)throw new Rejected("File changed while hashing");}
        NoReparse(path); before.Refresh(); if(before.Length!=size||before.LastWriteTimeUtc!=changed)throw new Rejected("File changed while hashing");
        return new Record(size,sha);
    }
    public static string Hex(byte[] bytes){return BitConverter.ToString(bytes).Replace("-","").ToLowerInvariant();}
    public static string Digest(Dictionary<string,Record> records,string kind) {
        var value=new StringBuilder("coop-fof-"+kind+"-v1\0"+records.Count.ToString(CultureInfo.InvariantCulture)+"\n");
        foreach(string path in records.Keys.OrderBy(x=>x,StringComparer.Ordinal)){Record r=records[path];value.Append(path).Append('\0').Append(r.Size.ToString(CultureInfo.InvariantCulture)).Append('\0').Append(r.Sha).Append('\n');}
        using(var hash=SHA256.Create())return Hex(hash.ComputeHash(Encoding.UTF8.GetBytes(value.ToString())));
    }
    public static Dictionary<string,string> Scan(string root) {
        root=NoReparse(root); var result=new Dictionary<string,string>(StringComparer.Ordinal); var dirs=new Stack<string>();dirs.Push(root);
        while(dirs.Count>0)foreach(string path in Directory.EnumerateFileSystemEntries(dirs.Pop())) {
            NoReparse(path); if(Directory.Exists(path)){dirs.Push(path);continue;}
            if(!File.Exists(path))throw new Rejected("Non-regular file rejected");
            string key=Normalize(path.Substring(root.TrimEnd('\\').Length+1));
            if(result.ContainsKey(key))throw new Rejected("Duplicate case-insensitive path: "+key);
            result.Add(key,path);
        }
        return result;
    }
    public static bool Same(Record a,Record b){return a.Size==b.Size&&a.Sha==b.Sha;}
    public static bool AllowedExtra(string p){return Ignore.Contains(p)||p.StartsWith("gamelog/",StringComparison.Ordinal);}
    public static void CheckExtras(IEnumerable<string> found,HashSet<string> expected) {
        foreach(string path in found){if(path.EndsWith(".dll",StringComparison.Ordinal)&&path!="binkw32.dll")throw new Rejected("Unknown DLL: "+path);if(!expected.Contains(path)&&!AllowedExtra(path))throw new Rejected("Unexpected runtime/package file: "+path);}
    }
    public static Dictionary<string,string> GameEnvironment(){return GameEnvironment(false);}
    public static Dictionary<string,string> GameEnvironment(bool automated) {
        var d=new Dictionary<string,string>();
        foreach(DictionaryEntry entry in Environment.GetEnvironmentVariables())if(!((string)entry.Key).StartsWith("COOP_PROBE_",StringComparison.OrdinalIgnoreCase))d[(string)entry.Key]=(string)entry.Value;
        string[] pairs={"AUTOTEST=ExecutionPath","INPUTBOT=0","PLAYBOT=0","ACTIONBOT=0","HUMAN=1","HUMAN_FIXTURE=0","PATHSYNC=1","AMBIENT_SPLIT=1","CLEAN_EXIT=1","NET_FRAMES=0","NET_ADDRESS=127.0.0.1","NET_PORT=49375","AREA=Tokyo/1_Level/1_Area","CONTENT_SHA256="+Critical};
        foreach(string pair in pairs){int i=pair.IndexOf('=');d["COOP_PROBE_"+pair.Substring(0,i)]=pair.Substring(i+1);}
        if(automated){d["COOP_PROBE_HUMAN_FIXTURE"]="1";d["COOP_PROBE_NET_FRAMES"]=ConnectionEvidence.Frames.ToString(CultureInfo.InvariantCulture);d["COOP_PROBE_DIAGNOSTICS"]="1";}return d;
    }
    public static string NativeArguments(string game,string sandbox,bool automated){return (automated?"":"--play ")+Quote(game)+" "+Quote(sandbox)+(automated?" 300":"");}
    public static string Quote(string s) {
        // CommandLineToArgvW quoting, including quotes and trailing backslashes.
        return "\""+Regex.Replace(s,@"(\\*)\""",m=>new string('\\',m.Groups[1].Length*2+1)+"\"").TrimEnd('\\')+new string('\\',s.Reverse().TakeWhile(c=>c=='\\').Count()*2)+"\"";
    }
    public static void NewJson(string path,object data){using(var stream=new FileStream(path,FileMode.CreateNew,FileAccess.Write,FileShare.Read))using(var w=new StreamWriter(stream,new UTF8Encoding(false)))w.WriteLine(Json.Serialize(data));}
}
sealed class ConnectionEvidence {
    public const int Frames=3600;
    public string schema="fof-automated-connection-test-v1",status="fail";
    public string scope="Finite scripted Tokyo connection check; physical input, full campaign and reconnect are not certified.";
    public int frames_expected=Frames,frames_observed,role;
    public bool cleanup_complete;
    public List<string> errors=new List<string>();
    static readonly Regex Row=new Regex(@"^NET state tick=(\d+) rng=[0-9a-f]{8} pos0=([0-9a-f]{8},[0-9a-f]{8},[0-9a-f]{8}) pos1=([0-9a-f]{8},[0-9a-f]{8},[0-9a-f]{8}) slots=0,1 input=[0-9a-f]{8},[0-9a-f]{8} world=(\d+),[0-9a-f]{8} progress=[0-9a-f]{8} area=[0-9a-f]{8}$");
    static bool Finite(string vector){foreach(string word in vector.Split(',')){float f=BitConverter.ToSingle(BitConverter.GetBytes(UInt32.Parse(word,NumberStyles.HexNumber,CultureInfo.InvariantCulture)),0);if(Single.IsNaN(f)||Single.IsInfinity(f))return false;}return true;}
    public static string OwnFixtureProbe(int role){var text=new StringBuilder("NET READY role="+role+"\n");for(int i=0;i<Frames;i++){string p=BitConverter.ToUInt32(BitConverter.GetBytes((float)i),0).ToString("x8",CultureInfo.InvariantCulture);text.Append("NET state tick=").Append(i).Append(" rng=12345678 pos0=").Append(p).Append(",00000000,00000000 pos1=00000000,").Append(p).Append(",00000000 slots=0,1 input=11111111,22222222 world=3,12345678 progress=12345678 area=12345678\n");}return text.Append("NET SCRIPT_COMPLETE frames=3600; peer completion confirmed; no co-op QA awarded\nNET clean WM_CLOSE posted to owned game window\n").ToString();}
    public const string OwnFixtureRelay="{\"event\":\"PEER_READY\",\"peer\":333}\n{\"event\":\"STREAM_COMPLETE\",\"sent\":461008,\"received\":461008,\"discarded_after_game_reset\":0}\n";
    public static ConnectionEvidence Evaluate(string probe,string launcherText,string relayText,int expectedRole,int pid,int launcherExit,int gameExit,int relayExit,bool cancelled,bool forced,bool cleaned) {
        var r=new ConnectionEvidence{role=expectedRole,cleanup_complete=cleaned};
        if(cancelled)r.errors.Add("Session was cancelled before test completion");
        if(forced||!cleaned)r.errors.Add("Owned cleanup was forced or incomplete");
        if(pid<=0||launcherExit!=0||gameExit!=0||relayExit!=0)r.errors.Add("Owned game, native launcher and relay must all exit normally with code zero");
        if(Regex.IsMatch(probe+"\n"+launcherText,@"NET FAIL|EXCEPTION|FAIL closed|TIMEOUT:|PROBE_SCRIPT_FINISHED",RegexOptions.IgnoreCase))r.errors.Add("Game failure, exception or forced completion was recorded");
        string[] lines=probe.Split(new[]{'\r','\n'},StringSplitOptions.RemoveEmptyEntries);
        if(lines.Count(x=>x=="NET READY role="+expectedRole)!=1)r.errors.Add("Expected native peer handshake was not observed exactly once");
        if(lines.Count(x=>x=="NET SCRIPT_COMPLETE frames="+Frames+"; peer completion confirmed; no co-op QA awarded")!=1)r.errors.Add("Native FIN peer completion is missing or repeated");
        if(lines.Count(x=>x=="NET clean WM_CLOSE posted to owned game window")!=1)r.errors.Add("Native normal game close request is missing or repeated");
        var exits=Regex.Matches(launcherText,@"(?m)^PROCESS_EXIT pid=(\d+) code=(\d+)\r?$");
        if(exits.Count!=1||exits[0].Groups[1].Value!=pid.ToString(CultureInfo.InvariantCulture)||exits[0].Groups[2].Value!="0")r.errors.Add("Native exit record does not match the retained owned game handle");
        var pos0=new HashSet<string>();var pos1=new HashSet<string>();bool valid=true;
        foreach(string line in lines.Where(x=>x.StartsWith("NET state",StringComparison.Ordinal))){var m=Row.Match(line);int tick,world;
            if(!m.Success||!Int32.TryParse(m.Groups[1].Value,out tick)||tick!=r.frames_observed||!Int32.TryParse(m.Groups[4].Value,out world)||world<2||world>4096||!Finite(m.Groups[2].Value)||!Finite(m.Groups[3].Value)){valid=false;break;}
            pos0.Add(m.Groups[2].Value);pos1.Add(m.Groups[3].Value);r.frames_observed++;
        }
        if(!valid||r.frames_observed!=Frames)r.errors.Add("Expected 3600 consecutive finite state records with both player slots and progress/area hashes");
        if(pos0.Count<2||pos1.Count<2)r.errors.Add("Both scripted players must move");
        int completed=0,ready=0;bool relayGood=true;
        foreach(string line in relayText.Split(new[]{'\r','\n'},StringSplitOptions.RemoveEmptyEntries)){if(!line.StartsWith("{"))continue;try{
            var data=(Dictionary<string,object>)new JavaScriptSerializer().DeserializeObject(line);string ev=(string)data["event"];
            if(ev=="PEER_READY")ready++;
            if(ev=="RELAY_FAIL"||ev=="RELAY_CANCELLED")relayGood=false;
            if(ev=="STREAM_COMPLETE"){completed++;long sent=Convert.ToInt64(data["sent"],CultureInfo.InvariantCulture),received=Convert.ToInt64(data["received"],CultureInfo.InvariantCulture),discarded=Convert.ToInt64(data["discarded_after_game_reset"],CultureInfo.InvariantCulture);if(sent<=0||received<=0||discarded!=0)relayGood=false;}
        }catch(Exception){relayGood=false;}}
        if(!relayGood||completed!=1||ready!=1)r.errors.Add("Relay must confirm one validated peer and one normal nonempty stream with zero discarded bytes");
        if(r.errors.Count==0)r.status="pass";return r;
    }
}
sealed class Manifest {
    public Dictionary<string,Record> Core; public Dictionary<string,Dictionary<string,Record>> Locales;
    public HashSet<string> Expected;
    static Dictionary<string,Record> Records(object source) {
        var d=new Dictionary<string,Record>(StringComparer.Ordinal);
        foreach(var pair in (Dictionary<string,object>)source){string path=Files.Normalize(pair.Key);if(path!=pair.Key||d.ContainsKey(path))throw new Rejected("Invalid baseline path");var row=(object[])pair.Value;d.Add(path,new Record(Convert.ToInt64(row[0],CultureInfo.InvariantCulture),(string)row[1]));}return d;
    }
    public static Manifest Load() {
        string path=Files.NoReparse(Path.Combine(Files.Root,"compatibility","baseline.json"));
        byte[] bytes=File.ReadAllBytes(path);using(var hash=SHA256.Create())if(Files.Hex(hash.ComputeHash(bytes))!=Files.BaselineSha)throw new Rejected("Baseline SHA-256 does not match pinned manifest");
        var raw=(Dictionary<string,object>)Files.Json.DeserializeObject(Encoding.UTF8.GetString(bytes));if((string)raw["schema"]!="coop-fof-baseline-v1")throw new Rejected("Unknown baseline schema");
        var m=new Manifest{Core=Records(raw["critical"]),Locales=new Dictionary<string,Dictionary<string,Record>>()};
        foreach(var pair in (Dictionary<string,object>)raw["locales"])m.Locales.Add(pair.Key,Records(((Dictionary<string,object>)pair.Value)["files"]));
        var pins=new Dictionary<string,string>{{"English","4d7d3496ff2ecd7d4f112a7a0576b7fa82b4b4233e355e1ecae82f63a3c7fea8"},{"Russian","04053ecb5b7b995dc9c3368970d17197b7495d16d10ccac1165f070e6fc221cc"}};
        if(m.Core.Count!=1257||Files.Digest(m.Core,"critical")!=Files.Critical||m.Locales.Count!=2)throw new Rejected("Unpinned critical baseline");
        foreach(var pin in pins)if(!m.Locales.ContainsKey(pin.Key)||m.Locales[pin.Key].Count!=34||Files.Digest(m.Locales[pin.Key],"locale")!=pin.Value)throw new Rejected("Unpinned language baseline");
        m.Expected=new HashSet<string>(m.Core.Keys.Concat(m.Locales["English"].Keys));
        if(m.Expected.Count!=1291||!new HashSet<string>(m.Locales["English"].Keys).SetEquals(m.Locales["Russian"].Keys))throw new Rejected("Invalid locale path sets");return m;
    }
    public Report Verify(string root,Action<string> progress) {
        var found=Files.Scan(root);Files.CheckExtras(found.Keys,Expected);
        foreach(string path in Expected)if(!found.ContainsKey(path))throw new Rejected("Missing runtime file: "+path);
        var candidates=new HashSet<string>(Locales.Keys);int count=0;
        foreach(string path in Expected.OrderBy(p=>!p.StartsWith("ini/",StringComparison.Ordinal)).ThenBy(p=>p,StringComparer.Ordinal)) {
            Record actual=Files.HashFile(found[path]);if(Core.ContainsKey(path)){if(!Files.Same(actual,Core[path]))throw new Rejected("Critical content mismatch: "+path);}
            else {candidates.RemoveWhere(name=>!Files.Same(actual,Locales[name][path]));if(candidates.Count==0)throw new Rejected("Unknown or mixed language pack: "+path);}
            if(++count%100==0&&progress!=null)progress("Verifying runtime files "+count+" / 1291");
        }
        if(candidates.Count!=1)throw new Rejected("Ambiguous language pack");string language=candidates.First();
        return new Report{language=language,critical_digest=Files.Critical,locale_digest=Files.Digest(Locales[language],"locale"),critical_files=1257,locale_files=34};
    }
    public Report Import(string source,string language,Action<string> progress) {
        string testRoot=Path.Combine(Files.Root,"test"),destination=Path.Combine(testRoot,language.ToLowerInvariant());
        Files.NoReparse(Files.Root);if(Directory.Exists(testRoot))Files.Child(destination,testRoot,false);else if(File.Exists(testRoot))throw new Rejected("Test root is not a directory");
        Report before=Verify(source,progress);if(before.language!=language)throw new Rejected("Selected language does not match source copy");
        var found=Files.Scan(source);string stagingRoot=Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"import-staging");
        if(!Directory.Exists(stagingRoot))Directory.CreateDirectory(stagingRoot);Files.NoReparse(stagingRoot);
        string staging=Path.Combine(stagingRoot,Guid.NewGuid().ToString("N"));Directory.CreateDirectory(staging);Files.NoReparse(staging);
        // Failure leaves a private partial staging folder for review; no existing file is overwritten.
        foreach(string path in Expected.OrderBy(p=>p,StringComparer.Ordinal)) {
            string target=Path.Combine(staging,path.Replace('/','\\'));Directory.CreateDirectory(Path.GetDirectoryName(target));Files.NoReparse(Path.GetDirectoryName(target));Files.NoReparse(found[path]);
            using(var input=new FileStream(found[path],FileMode.Open,FileAccess.Read,FileShare.Read))using(var output=new FileStream(target,FileMode.CreateNew,FileAccess.Write,FileShare.None))input.CopyTo(output);
        }
        Report after=Verify(staging,progress);if(after.language!=language)throw new Rejected("Copied language mismatch");
        Directory.CreateDirectory(testRoot);Files.Child(destination,testRoot,false);Directory.Move(staging,destination);return after;
    }
}
sealed class LauncherForm : Form {
    protected override bool ShowWithoutActivation{get{return offline;}}
    readonly ComboBox language=new ComboBox{DropDownStyle=ComboBoxStyle.DropDownList};
    readonly CheckBox automated=new CheckBox{Text="Automated connection test (both players enable; scripted controls, ends automatically)",AutoSize=true};
    readonly TextBox dll=new TextBox(), lobby=new TextBox(), log=new TextBox{Multiline=true,ReadOnly=true,ScrollBars=ScrollBars.Vertical};
    readonly Label status=new Label{AutoSize=true}; readonly Button host=new Button{Text="Host"},join=new Button{Text="Join"},stop=new Button{Text="Stop session"},import=new Button{Text="Import installed folder"},verify=new Button{Text="Verify copy"};
    Process relay,launcher,game; OwnedJob job; StreamWriter relayLog,launcherLog; string session; int ownedLauncherPid;DateTime launcherEarliest; bool busy,stopping,gameStarted,cleanupForced,connectionPassed; volatile bool peerReady,cancelRequested; readonly object gate=new object(); readonly bool offline;public bool OfflinePassed;public string OfflineDirectory;
    public LauncherForm():this(false){}
    public LauncherForm(bool offlineMode) {
        offline=offlineMode;
        Text="Friend or Foe — PRIVATE RESEARCH";Width=780;Height=640;MinimumSize=new System.Drawing.Size(680,560);
        var grid=new TableLayoutPanel{Dock=DockStyle.Fill,ColumnCount=3,RowCount=9,Padding=new Padding(14)};grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,150));grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,170));Controls.Add(grid);
        var warning=new Label{Text="PRIVATE RESEARCH • Tokyo scenario only\nCampaign, menus, saving and remote gameplay are not yet verified.\nYour Host / Join action starts Steam Spacewar using your signed-in account. No invitations are sent.",AutoSize=true};grid.Controls.Add(warning,0,0);grid.SetColumnSpan(warning,3);
        language.Items.AddRange(new object[]{"English","Russian"});language.SelectedIndex=0;
        grid.Controls.Add(new Label{Text="Local language",AutoSize=true},0,1);grid.Controls.Add(language,1,1);grid.Controls.Add(import,2,1);
        grid.Controls.Add(new Label{Text="Local files",AutoSize=true},0,2);grid.Controls.Add(status,1,2);grid.Controls.Add(verify,2,2);
        grid.Controls.Add(new Label{Text="Existing x64 Steam API DLL",AutoSize=true},0,3);grid.Controls.Add(dll,1,3);dll.Dock=DockStyle.Fill;var browse=new Button{Text="Choose DLL"};grid.Controls.Add(browse,2,3);
        grid.Controls.Add(new Label{Text="Lobby code",AutoSize=true},0,4);grid.Controls.Add(lobby,1,4);lobby.Dock=DockStyle.Fill;var copy=new Button{Text="Copy code"};grid.Controls.Add(copy,2,4);
        var buttons=new FlowLayoutPanel{AutoSize=true};buttons.Controls.AddRange(new Control[]{host,join,stop});grid.Controls.Add(buttons,0,5);grid.SetColumnSpan(buttons,3);
        grid.Controls.Add(new Label{Text="Each player can use a local keyboard or controller. Exchange lobby codes manually.\nWaiting for a peer can take as long as needed; the game starts after the peer connects.",AutoSize=true},0,6);grid.SetColumnSpan(grid.GetControlFromPosition(0,6),3);
        grid.Controls.Add(automated,0,7);grid.SetColumnSpan(automated,3);
        grid.Controls.Add(log,0,8);grid.SetColumnSpan(log,3);log.Dock=DockStyle.Fill;grid.RowStyles.Clear();for(int i=0;i<8;i++)grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));grid.RowStyles.Add(new RowStyle(SizeType.Percent,100));
        stop.Enabled=false; language.SelectedIndexChanged+=(s,e)=>RefreshStatus();RefreshStatus();
        browse.Click+=(s,e)=>{using(var d=new OpenFileDialog{Filter="Steam API (steam_api64.dll)|steam_api64.dll",CheckFileExists=true})if(d.ShowDialog()==DialogResult.OK)dll.Text=d.FileName;};
        copy.Click+=(s,e)=>{if(Regex.IsMatch(lobby.Text,@"^\d{1,20}$"))Clipboard.SetText(lobby.Text);};
        verify.Click+=async(s,e)=>await Work(async()=>{var m=Manifest.Load();string test=TestDirectory();var r=await Task.Run(()=>m.Verify(test,Note));Note("PASS "+r.language+" — 1291 runtime files verified.");});
        import.Click+=async(s,e)=>{using(var d=new FolderBrowserDialog{Description="Choose your installed game folder. Source files are read only; only verified runtime files are copied."})if(d.ShowDialog()==DialogResult.OK){string source=d.SelectedPath,lang=(string)language.SelectedItem;await Work(async()=>{var m=Manifest.Load();await Task.Run(()=>m.Import(source,lang,Note));Note("Imported verified "+lang+" copy.");RefreshStatus();});}};
        host.Click+=async(s,e)=>await StartSession(true);join.Click+=async(s,e)=>await StartSession(false);stop.Click+=async(s,e)=>await StopSession();
        FormClosing+=async(s,e)=>{if(busy||relay!=null||launcher!=null){e.Cancel=true;if(!stopping){await StopSession();Note("Wait for the current verification/session cleanup to finish, then close this window.");}}};
    }
    public async Task RunOffline(bool connection) {
        Note("Own-code offline session test; no Steam or game library is loaded.");
        automated.Checked=connection;
        await StartSession(true);
        string relayText=File.Exists(Path.Combine(session,"relay.log"))?File.ReadAllText(Path.Combine(session,"relay.log")):"";
        string gameText=File.Exists(Path.Combine(session,"launcher.log"))?File.ReadAllText(Path.Combine(session,"launcher.log")):"";
        OfflinePassed=peerReady&&gameStarted&&relay==null&&launcher==null&&game==null&&job==null&&(connection?connectionPassed:relayText.Contains("FIXTURE_STOP")&&gameText.Contains("FIXTURE_GAME_EXIT 0"));OfflineDirectory=session;
    }
    string TestDirectory(){return Path.Combine(Files.Root,"test",((string)language.SelectedItem).ToLowerInvariant());}
    void RefreshStatus(){status.Text=Directory.Exists(TestDirectory())?"Copy available; verified before each launch":"Import required";}
    void Note(string text){if(IsDisposed)return;if(InvokeRequired){BeginInvoke(new Action<string>(Note),text);return;}log.AppendText(text+Environment.NewLine);}
    void ControlsBusy(bool value){busy=value;host.Enabled=join.Enabled=import.Enabled=verify.Enabled=language.Enabled=automated.Enabled=!value;stop.Enabled=value&&(relay!=null||launcher!=null);}
    async Task Work(Func<Task> action){if(busy)return;ControlsBusy(true);try{await action();}catch(Exception ex){Note("REJECTED: "+ex.Message);}finally{ControlsBusy(false);}}
    Process StartOwned(string executable,string arguments,StreamWriter output,Action<string> line,Dictionary<string,string> environment) {
        Files.NoReparse(executable);var info=new ProcessStartInfo(executable,arguments){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true,RedirectStandardInput=true,WorkingDirectory=Files.Root};
        foreach(string name in info.EnvironmentVariables.Keys.Cast<string>().ToArray())if(name.StartsWith("COOP_PROBE_",StringComparison.OrdinalIgnoreCase))info.EnvironmentVariables.Remove(name);
        if(environment!=null){info.EnvironmentVariables.Clear();foreach(var p in environment)info.EnvironmentVariables[p.Key]=p.Value;}
        var process=new Process{StartInfo=info,EnableRaisingEvents=true};
        DataReceivedEventHandler receive=(s,e)=>{if(e.Data==null)return;lock(gate){if(output!=null){try{output.WriteLine(e.Data);output.Flush();}catch(ObjectDisposedException){}catch(IOException){}}}line(e.Data);};
        process.OutputDataReceived+=receive;process.ErrorDataReceived+=receive;if(Path.GetFileName(executable)=="native-launcher.exe")launcherEarliest=DateTime.UtcNow;process.Start();
        try{job.Add(process);}catch{try{if(!process.HasExited)process.Kill();}finally{process.Dispose();}throw;}
        if(Path.GetFileName(executable)=="native-launcher.exe"||(offline&&arguments.StartsWith("--launcher-",StringComparison.Ordinal))){ownedLauncherPid=process.Id;launcherEarliest=process.StartTime.ToUniversalTime();}
        process.BeginOutputReadLine();process.BeginErrorReadLine();return process;
    }
    async Task StartSession(bool hosting) {
        if(busy)return;bool runTest=automated.Checked;ControlsBusy(true);peerReady=false;gameStarted=false;cancelRequested=false;cleanupForced=false;connectionPassed=false;session=null;
        int testPid=0,testLauncherExit=-1,testGameExit=-1,testRelayExit=-1;bool testCancelled=false;
        try {
            string test=TestDirectory(),selected=(string)language.SelectedItem,steam="";Report report;
            if(offline)report=new Report{schema="own-offline-fixture",status="fixture",language="English",critical_digest="not-checked-in-own-fixture"};
            else {
                var m=Manifest.Load();test=Files.Child(test,Path.Combine(Files.Root,"test"),true);report=await Task.Run(()=>m.Verify(test,Note));if(cancelRequested)throw new OperationCanceledException("Session cancelled before account use");if(report.language!=selected)throw new Rejected("Test folder language mismatch");
                steam=Path.GetFullPath(dll.Text);if(Files.HashFile(steam).Sha!=Files.SteamSha)throw new Rejected("Only the currently pinned existing Steam API build is supported");
                using(var fs=File.OpenRead(steam))using(var b=new BinaryReader(fs)){if(b.ReadUInt16()!=0x5a4d)throw new Rejected("Steam DLL is not PE");fs.Position=0x3c;int offset=b.ReadInt32();fs.Position=offset;if(b.ReadUInt32()!=0x4550||b.ReadUInt16()!=0x8664)throw new Rejected("Steam DLL must be x64");}
            }
            ulong code=0;if(!hosting&&(!UInt64.TryParse(lobby.Text,NumberStyles.None,CultureInfo.InvariantCulture,out code)||code==0))throw new Rejected("Enter the host's decimal lobby code");
            string sessions=offline?Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"offline-sessions"):Path.Combine(Files.Root,"sessions");if(!Directory.Exists(sessions))Directory.CreateDirectory(sessions);Files.NoReparse(sessions);
            session=Path.Combine(sessions,DateTime.UtcNow.ToString("yyyyMMddTHHmmssfff",CultureInfo.InvariantCulture)+"-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(session);Files.NoReparse(session);Files.NewJson(Path.Combine(session,"preflight.json"),report);
            job=new OwnedJob();relayLog=new StreamWriter(new FileStream(Path.Combine(session,"relay.log"),FileMode.CreateNew,FileAccess.Write,FileShare.Read));
            string args=(hosting?"host":"join")+" --steam-live "+Files.Quote(steam)+" --steam-api-sha256 "+Files.SteamSha+" --local-port 49375 --seconds 0 --ready-file "+Files.Quote(Path.Combine(session,"lobby.json"))+(hosting?"":" --lobby "+code.ToString(CultureInfo.InvariantCulture));
            string ownExe=System.Reflection.Assembly.GetExecutingAssembly().Location;
            relay=StartOwned(offline?ownExe:Path.Combine(Files.Root,"steam","native","native-relay.exe"),offline?(runTest?"--relay-connection-fixture "+Files.Quote(session):"--relay-fixture"):args,relayLog,RelayLine,null);stop.Enabled=true;Note(offline?"Own relay fixture started.":"Steam started. Waiting for lobby and peer.");
            // Capture only this process handle; a later PID is never used for relay cleanup.
            Process currentRelay=relay;
            while(!peerReady&&!currentRelay.HasExited&&!cancelRequested)await Task.Delay(100);
            if(cancelRequested)throw new OperationCanceledException("Session cancelled");if(!peerReady)throw new Rejected("Relay exited before a validated peer connected; see private session log");
            var env=Files.GameEnvironment(runTest);env["COOP_PROBE_NET_MODE"]=hosting?"host":"join";
            launcherLog=new StreamWriter(new FileStream(Path.Combine(session,"launcher.log"),FileMode.CreateNew,FileAccess.Write,FileShare.Read));
            if(offline&&!peerReady)throw new Rejected("Offline ordering check: game before PEER_READY");
            launcher=StartOwned(offline?ownExe:Path.Combine(Files.Root,"probe","native-launcher.exe"),offline?(runTest?"--launcher-connection-fixture "+Files.Quote(session):"--launcher-fixture"):Files.NativeArguments(Path.Combine(test,"Game.exe"),Path.Combine(session,"sandbox"),runTest),launcherLog,GameLine,env);gameStarted=true;Note(runTest?"Peer ready. Automated connection test is running; scripted controls will stop after 3600 frames (five-minute limit).":"Peer ready. Starting the Tokyo research scenario.");
            if(offline&&!runTest){var timer=new System.Windows.Forms.Timer{Interval=1500};timer.Tick+=async(s,e)=>{timer.Stop();timer.Dispose();await StopSession();};timer.Start();}
            Process currentLauncher=launcher;while(!cancelRequested&&!currentLauncher.HasExited){if(currentRelay.HasExited&&!runTest){Note("Relay exited; closing the owned game session.");break;}await Task.Delay(250);}
            if(!cancelRequested&&currentLauncher.HasExited)Note("Game launcher exited "+currentLauncher.ExitCode+". Session logs are private.");
            if(runTest&&!cancelRequested&&currentLauncher.HasExited){
                await Task.Run(()=>currentLauncher.WaitForExit());testLauncherExit=currentLauncher.ExitCode;
                lock(gate){if(game!=null&&game.HasExited){testPid=game.Id;testGameExit=game.ExitCode;}}
                DateTime end=DateTime.UtcNow.AddSeconds(15);while(!cancelRequested&&!currentRelay.HasExited&&DateTime.UtcNow<end)await Task.Delay(100);
                if(!cancelRequested&&currentRelay.HasExited){await Task.Run(()=>currentRelay.WaitForExit());testRelayExit=currentRelay.ExitCode;}
            }
        }catch(Exception ex){Note("REJECTED: "+ex.Message);}
        testCancelled=cancelRequested;try{while(stopping)await Task.Delay(100);await StopSession();}catch(Exception){cleanupForced=true;Note("Owned session cleanup did not complete normally.");}
        if(runTest){
            try {
                string probePath=session==null?null:Path.Combine(session,"sandbox","probe.log"),launchPath=session==null?null:Path.Combine(session,"launcher.log"),relayPath=session==null?null:Path.Combine(session,"relay.log");
                var result=ConnectionEvidence.Evaluate(ReadEvidence(probePath),ReadEvidence(launchPath),ReadEvidence(relayPath),hosting?1:2,testPid,testLauncherExit,testGameExit,testRelayExit,testCancelled,cleanupForced,relay==null&&launcher==null&&game==null&&job==null);
                if(session!=null)Files.NewJson(Path.Combine(session,"connection-test.json"),result);
                connectionPassed=result.status=="pass";
                Note(result.status=="pass"?"AUTOMATED CONNECTION TEST PASS — 3600 scripted frames, native peer completion, normal stream/game exit and owned cleanup. Physical controls and campaign remain unverified.":"AUTOMATED CONNECTION TEST FAIL — "+String.Join("; ",result.errors.ToArray())+". Private logs were retained.");
            }catch(Exception){Note("AUTOMATED CONNECTION TEST FAIL — evidence could not be read or saved. Private logs were retained.");}
        }
        ControlsBusy(false);
    }
    static string ReadEvidence(string path){if(path==null||!File.Exists(path))return "";Files.NoReparse(path);if(new FileInfo(path).Length>64*1024*1024)throw new Rejected("Evidence exceeds bounded log size");return File.ReadAllText(path);}
    void RelayLine(string line) {
        try{if(!line.StartsWith("{"))return;var data=(Dictionary<string,object>)new JavaScriptSerializer().DeserializeObject(line);string ev=(string)data["event"];
            if(ev=="LOBBY_READY"){string code=Convert.ToString(data["lobby"],CultureInfo.InvariantCulture);BeginInvoke(new Action(()=>{lobby.Text=code;Note("Lobby ready. Copy the code to your friend manually.");}));}
            if(ev=="PEER_READY")peerReady=true;
            if(ev=="RELAY_FAIL")Note("Relay failed; inspect private session log.");
        }catch(Exception){Note("Relay emitted an unreadable status line.");}
    }
    void GameLine(string line) {
        var match=Regex.Match(line,@"^HOOK_READY pid=(\d+) ");if(!match.Success||cancelRequested)return;
        try {var candidate=Process.GetProcessById(Int32.Parse(match.Groups[1].Value,CultureInfo.InvariantCulture));var handle=candidate.Handle;
            // Trusted owned launcher reports its child immediately; retain its process handle to avoid PID reuse.
            string expected=offline?Path.GetFileNameWithoutExtension(System.Reflection.Assembly.GetExecutingAssembly().Location).ToLowerInvariant():"game";
            if(candidate.StartTime.ToUniversalTime()<launcherEarliest||candidate.ProcessName.ToLowerInvariant()!=expected||!Ownership.HasParent(candidate,ownedLauncherPid)){candidate.Dispose();return;}
            lock(gate){if(game==null)game=candidate;else candidate.Dispose();}
        }catch(Exception){Note("Could not capture owned game handle; launcher job remains the cleanup boundary.");}
    }
    async Task StopSession() {
        cancelRequested=true;if(stopping)return;stopping=true;stop.Enabled=false;
        try {
            Process ownedGame;lock(gate){ownedGame=game;game=null;}
            if(ownedGame!=null){try{if(!ownedGame.HasExited){Ownership.RequestClose(ownedGame);await Task.Run(()=>ownedGame.WaitForExit(5000));}}catch(InvalidOperationException){}finally{ownedGame.Dispose();}}
            if(launcher!=null){Process owned=launcher;launcher=null;try{if(!owned.HasExited)await Task.Run(()=>owned.WaitForExit(2000));if(!owned.HasExited){cleanupForced=true;owned.Kill();await Task.Run(()=>owned.WaitForExit(3000));}}finally{owned.Dispose();}}
            if(relay!=null){Process owned=relay;relay=null;try{if(!owned.HasExited){try{owned.StandardInput.WriteLine("STOP");owned.StandardInput.Flush();}catch(IOException){}await Task.Run(()=>owned.WaitForExit(5000));if(!owned.HasExited){cleanupForced=true;owned.Kill();await Task.Run(()=>owned.WaitForExit(3000));}}}finally{owned.Dispose();}}
            lock(gate){if(relayLog!=null){relayLog.Dispose();relayLog=null;}if(launcherLog!=null){launcherLog.Dispose();launcherLog=null;}}
            if(gameStarted)Note("Session stopped. This prototype does not provide campaign saving.");
        }finally{if(job!=null){job.Dispose();job=null;}stopping=false;}
    }
}
static class Program {
    static void Assert(bool value,string name){if(!value)throw new Exception("Selftest failed: "+name);}
    static void Reject(Action action,string name){try{action();}catch(Rejected){return;}throw new Exception("Expected rejection: "+name);}
    static int ConnectionSelftest() {
        string probe=ConnectionEvidence.OwnFixtureProbe(1),launch="HOOK_READY pid=123 fixture=1\nPROCESS_EXIT pid=123 code=0\n",relay=ConnectionEvidence.OwnFixtureRelay;
        Func<string,string,string,int,int,int,int,bool,bool,bool,ConnectionEvidence> evaluate=(p,l,r,pid,le,ge,re,cancel,forced,clean)=>ConnectionEvidence.Evaluate(p,l,r,1,pid,le,ge,re,cancel,forced,clean);
        Assert(evaluate(probe,launch,relay,123,0,0,0,false,false,true).status=="pass","connection success fixture");int negative=0;
        foreach(string bad in new[]{probe.Replace("NET READY role=1","NET READY role=2"),probe.Replace("peer completion confirmed","peer completion missing"),probe+"NET SCRIPT_COMPLETE frames=3600; peer completion confirmed; no co-op QA awarded\n",probe.Replace("NET clean WM_CLOSE posted to owned game window", ""),probe.Replace("NET state tick=120 ","NET state tick=121 "),probe.Replace("pos0=00000000,00000000,00000000", "pos0=7fc00000,00000000,00000000"),probe.Replace("slots=0,1", "slots=0,0"),probe.Replace(" progress=12345678", ""),probe+"NET FAIL fixture\n",probe+"EXCEPTION fixture\n",probe+"NET state tick=3600 malformed\n"}){Assert(evaluate(bad,launch,relay,123,0,0,0,false,false,true).status=="fail","negative game evidence");negative++;}
        foreach(string bad in new[]{launch.Replace("code=0","code=124"),launch+"TIMEOUT: fixture\n",launch+"PROBE_SCRIPT_FINISHED fixture\n",launch+"PROCESS_EXIT pid=123 code=0\n"}){Assert(evaluate(probe,bad,relay,123,0,0,0,false,false,true).status=="fail","negative launcher evidence");negative++;}
        foreach(string bad in new[]{relay.Replace("\"discarded_after_game_reset\":0","\"discarded_after_game_reset\":1"),relay.Replace("\"sent\":461008","\"sent\":0"),relay.Replace("STREAM_COMPLETE","RELAY_FAIL"),relay+"{\"event\":\"PEER_READY\",\"peer\":333}\n",relay+"{\"event\":\"RELAY_CANCELLED\"}\n","{not readable}"}){Assert(evaluate(probe,launch,bad,123,0,0,0,false,false,true).status=="fail","negative relay evidence");negative++;}
        foreach(int i in Enumerable.Range(0,7)){Assert(evaluate(probe,launch,relay,i==0?999:123,i==1?5:0,i==2?1:0,i==3?130:0,i==4,i==5,i!=6).status=="fail","negative exits/cancel/cleanup");negative++;}
        var play=Files.GameEnvironment(false);var test=Files.GameEnvironment(true);
        Assert(play["COOP_PROBE_HUMAN_FIXTURE"]=="0"&&play["COOP_PROBE_NET_FRAMES"]=="0"&&!play.ContainsKey("COOP_PROBE_DIAGNOSTICS"),"play remains default");
        Assert(test["COOP_PROBE_HUMAN_FIXTURE"]=="1"&&test["COOP_PROBE_NET_FRAMES"]=="3600"&&test["COOP_PROBE_CLEAN_EXIT"]=="1"&&test["COOP_PROBE_DIAGNOSTICS"]=="1"&&test["COOP_PROBE_CONTENT_SHA256"]==Files.Critical,"finite scripted test environment");
        Assert(Files.NativeArguments("game","sandbox",false)=="--play \"game\" \"sandbox\""&&Files.NativeArguments("game","sandbox",true)=="\"game\" \"sandbox\" 300","finite native arguments");
        Console.WriteLine("OWN_CONNECTION_SELFTEST_PASS positive=1 negative="+negative+" environment/arguments=3 game/account_used=0");return 0;
    }
    static int Selftest() {
        Assert(Files.Normalize("Art\\A.bin")=="art/a.bin","path normalization");foreach(string p in new[]{"../x","a//b","/x","a:b","a/./b","a\nx"})Reject(()=>Files.Normalize(p),"bad path");
        var expected=new HashSet<string>{"game.exe","binkw32.dll"};Files.CheckExtras(new[]{"game.exe","gamelog/output.txt","unins000.exe"},expected);Reject(()=>Files.CheckExtras(new[]{"inject.dll"},expected),"unknown DLL");Reject(()=>Files.CheckExtras(new[]{"save.dat"},expected),"unknown package file");
        var sample=new Dictionary<string,Record>{{"a",new Record(2,new string('0',64))}};Assert(Files.Digest(sample,"critical")=="14f29cb9bfc75e703fd86db6a16b7e9cab690e41d0493c1ef9a6a9a8f278ae86","canonical manifest digest");
        Assert(Files.Quote("a b")=="\"a b\""&&Files.Quote("x\\")=="\"x\\\\\""&&Files.Quote("a\"b")=="\"a\\\"b\"","Windows argument quoting");
        string previous=Environment.GetEnvironmentVariable("COOP_PROBE_UNTRUSTED");Environment.SetEnvironmentVariable("COOP_PROBE_UNTRUSTED","fixture");var env=Files.GameEnvironment();Environment.SetEnvironmentVariable("COOP_PROBE_UNTRUSTED",previous);Assert(!env.ContainsKey("COOP_PROBE_UNTRUSTED")&&env["COOP_PROBE_HUMAN"]=="1"&&env["COOP_PROBE_NET_FRAMES"]=="0","sanitized play environment");
        var manifest=Manifest.Load();Assert(manifest.Expected.Count==1291,"pinned manifest");
        using(var owned=new Process{StartInfo=new ProcessStartInfo(System.Reflection.Assembly.GetExecutingAssembly().Location,"--owned-fixture"){UseShellExecute=false,CreateNoWindow=true}}) {
            using(var ownedJob=new OwnedJob()){owned.Start();ownedJob.Add(owned);}
            Assert(owned.WaitForExit(3000),"owned job kill on close");
        }
        string fixtures=Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"own-fixtures-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(fixtures);
        string a=Path.Combine(fixtures,"game.exe"),b=Path.Combine(fixtures,"one.loc"),c=Path.Combine(fixtures,"two.loc");File.WriteAllText(a,"own-game");File.WriteAllText(b,"english-one");File.WriteAllText(c,"english-two");
        Record core=Files.HashFile(a),en1=Files.HashFile(b),en2=Files.HashFile(c);File.WriteAllText(b,"russian-one");File.WriteAllText(c,"russian-two");Record ru1=Files.HashFile(b),ru2=Files.HashFile(c);
        var mini=new Manifest{Core=new Dictionary<string,Record>{{"game.exe",core}},Locales=new Dictionary<string,Dictionary<string,Record>>{{"English",new Dictionary<string,Record>{{"one.loc",en1},{"two.loc",en2}}},{"Russian",new Dictionary<string,Record>{{"one.loc",ru1},{"two.loc",ru2}}}},Expected=new HashSet<string>{"game.exe","one.loc","two.loc"}};
        Assert(mini.Verify(fixtures,null).language=="Russian","coherent fixture");File.WriteAllText(b,"english-one");Reject(()=>mini.Verify(fixtures,null),"mixed locale");File.WriteAllText(c,"english-two");Assert(mini.Verify(fixtures,null).language=="English","English fixture");
        File.WriteAllText(a,"changed");Reject(()=>mini.Verify(fixtures,null),"changed core");File.WriteAllText(a,"own-game");File.WriteAllText(Path.Combine(fixtures,"bad.dll"),"own");Reject(()=>mini.Verify(fixtures,null),"unknown real-tree DLL");
        Reject(()=>Files.Child(Files.Root,Path.Combine(Files.Root,"test"),true),"outside test root");Reject(()=>Files.Child(fixtures,AppDomain.CurrentDomain.BaseDirectory,false),"existing output");
        Console.WriteLine("OWN_LAUNCHER_SELFTEST_PASS path/allowlist/digest/quote/environment/manifest/coherent/mixed/changed/unknownDLL/confinement/new-output/owned-job");return 0;
    }
    [STAThread] static int Main(string[] args) {
        try {
            if(args.Length==1&&args[0]=="--owned-fixture"){Thread.Sleep(30000);return 0;}
            if(args.Length==1&&args[0]=="--game-fixture"){Application.Run(new FixtureWindow());return 0;}
            if(args.Length==1&&args[0]=="--game-connection-fixture"){Thread.Sleep(1000);return 0;}
            if(args.Length==2&&(args[0]=="--relay-connection-fixture"||args[0]=="--launcher-connection-fixture")) {
                string directory=Files.Child(args[1],Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"offline-sessions"),true);
                string done=Path.Combine(directory,"connection-fixture.done");
                if(args[0]=="--relay-connection-fixture"){
                    Console.WriteLine("{\"event\":\"LOBBY_READY\",\"lobby\":222,\"host\":true,\"transport\":\"own-offline-fixture\"}");Console.Out.Flush();Thread.Sleep(350);
                    Console.WriteLine("{\"event\":\"PEER_READY\",\"peer\":333}");Console.Out.Flush();
                    DateTime end=DateTime.UtcNow.AddSeconds(15);while(!File.Exists(done)&&DateTime.UtcNow<end)Thread.Sleep(50);if(!File.Exists(done))return 1;
                    Console.WriteLine("{\"event\":\"STREAM_COMPLETE\",\"sent\":461008,\"received\":461008,\"discarded_after_game_reset\":0}");return 0;
                }
                Assert(Environment.GetEnvironmentVariable("COOP_PROBE_HUMAN_FIXTURE")=="1"&&Environment.GetEnvironmentVariable("COOP_PROBE_NET_FRAMES")=="3600"&&Environment.GetEnvironmentVariable("COOP_PROBE_DIAGNOSTICS")=="1","offline finite environment routing");
                string sandbox=Files.Child(Path.Combine(directory,"sandbox"),directory,false);Directory.CreateDirectory(sandbox);File.WriteAllText(Path.Combine(sandbox,"probe.log"),ConnectionEvidence.OwnFixtureProbe(1));
                using(var child=new Process{StartInfo=new ProcessStartInfo(System.Reflection.Assembly.GetExecutingAssembly().Location,"--game-connection-fixture"){UseShellExecute=false,CreateNoWindow=true}}){child.Start();Console.WriteLine("HOOK_READY pid="+child.Id+" fixture=1");Console.Out.Flush();if(!child.WaitForExit(10000)){child.Kill();return 1;}Console.WriteLine("PROCESS_EXIT pid="+child.Id+" code="+child.ExitCode);Console.Out.Flush();using(File.Create(done)){}return child.ExitCode;}
            }
            if(args.Length==1&&args[0]=="--relay-fixture") {
                Console.WriteLine("{\"event\":\"LOBBY_READY\",\"lobby\":222,\"host\":true,\"transport\":\"own-offline-fixture\"}");Console.Out.Flush();Thread.Sleep(350);
                Console.WriteLine("{\"event\":\"PEER_READY\",\"peer\":333}");Console.Out.Flush();
                if(Console.ReadLine()=="STOP"){Console.WriteLine("FIXTURE_STOP");return 130;}return 1;
            }
            if(args.Length==1&&args[0]=="--launcher-fixture") {
                using(var child=new Process{StartInfo=new ProcessStartInfo(System.Reflection.Assembly.GetExecutingAssembly().Location,"--game-fixture"){UseShellExecute=false,CreateNoWindow=true}}){child.Start();Console.WriteLine("HOOK_READY pid="+child.Id+" fixture=1");Console.Out.Flush();if(!child.WaitForExit(15000)){child.Kill();return 1;}Console.WriteLine("FIXTURE_GAME_EXIT "+child.ExitCode);return child.ExitCode;}
            }
            if(args.Length==1&&(args[0]=="--offline-session-selftest"||args[0]=="--offline-connection-selftest")) {
                bool connection=args[0]=="--offline-connection-selftest";Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);using(var form=new LauncherForm(true)){form.Opacity=0;form.ShowInTaskbar=false;form.Shown+=async(s,e)=>{await form.RunOffline(connection);form.Close();};Application.Run(form);Assert(form.OfflinePassed,"offline ordering -> owned game -> evidence/cleanup -> no remaining owned handles");Console.WriteLine((connection?"OWN_OFFLINE_CONNECTION_PASS ":"OWN_OFFLINE_SESSION_PASS ")+form.OfflineDirectory);return 0;}
            }
            if(args.Length==1&&args[0]=="--connection-selftest")return ConnectionSelftest();
            if(args.Length==1&&args[0]=="--selftest")return Selftest();
            if(args.Length==2&&args[0]=="--verify"){string folder=Files.Child(args[1],Path.Combine(Files.Root,"test"),true);var report=Manifest.Load().Verify(folder,null);Console.WriteLine(Files.Json.Serialize(report));return 0;}
            if(args.Length!=0)throw new Rejected("Usage: fof-private-launcher.exe [--selftest | --connection-selftest | --offline-session-selftest | --offline-connection-selftest | --verify <test folder>]");
            Files.NoReparse(Files.Root);Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);Application.Run(new LauncherForm());return 0;
        }catch(Exception ex){Console.Error.WriteLine("REJECTED: "+ex.Message);return 1;}
    }
}
}
