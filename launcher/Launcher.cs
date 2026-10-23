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
using Microsoft.Win32;

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
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr window);
    // Hands the foreground back to the owned game after the launcher (in front) has linked it.
    public static void Focus(Process process){if(process.HasExited)return;uint target=(uint)process.Id;IntPtr found=IntPtr.Zero;EnumWindows((window,context)=>{uint pid;GetWindowThreadProcessId(window,out pid);if(pid==target&&IsWindowVisible(window)){found=window;return false;}return true;},IntPtr.Zero);if(found!=IntPtr.Zero)SetForegroundWindow(found);}
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
    // native-launcher puts its d3d9 proxy (and a copy of the system d3d9 it forwards to) beside Game.exe while the
    // game runs and replaces an older build of it before the game starts. Accept this add-on's proxy (current, or an
    // older build: every build carries the UTF-16 name COOP_PROXY_PROBE) and the chain copy next to it.
    // Any other d3d9.dll is still an unknown DLL; native-launcher never loads or overwrites it either.
    public static bool HasProxyMarker(byte[] data){byte[] m=Encoding.Unicode.GetBytes("COOP_PROXY_PROBE");for(int i=0;i+m.Length<=data.Length;i++){int j=0;while(j<m.Length&&data[i+j]==m[j])j++;if(j==m.Length)return true;}return false;}
    public static bool OwnProxyFile(string key,Dictionary<string,string> found) {
        try{
            if(key=="d3d9_chain.dll")return found.ContainsKey("d3d9.dll")&&OwnProxyFile("d3d9.dll",found);
            if(key!="d3d9.dll")return false;string path=found[key];if(new FileInfo(path).Length>8*1024*1024)return false;
            string current=Path.Combine(Root,"probe","d3d9.dll");if(File.Exists(current)&&Same(HashFile(path),HashFile(current)))return true;
            return HasProxyMarker(File.ReadAllBytes(path));
        }catch(Exception){return false;}
    }
    public static void CheckExtras(IEnumerable<string> found,HashSet<string> expected){CheckExtras(found,expected,null);}
    public static void CheckExtras(IEnumerable<string> found,HashSet<string> expected,Func<string,bool> ownProxy) {
        foreach(string path in found){if((path=="d3d9.dll"||path=="d3d9_chain.dll")&&ownProxy!=null&&ownProxy(path))continue;if(path.EndsWith(".dll",StringComparison.Ordinal)&&path!="binkw32.dll")throw new Rejected("Unknown DLL: "+path);if(!expected.Contains(path)&&!AllowedExtra(path))throw new Rejected("Unexpected runtime/package file: "+path);}
    }
    public static Dictionary<string,string> GameEnvironment(){return GameEnvironment(false);}
    public static Dictionary<string,string> GameEnvironment(bool automated) {
        var d=new Dictionary<string,string>();
        foreach(DictionaryEntry entry in Environment.GetEnvironmentVariables())if(!((string)entry.Key).StartsWith("COOP_PROBE_",StringComparison.OrdinalIgnoreCase))d[(string)entry.Key]=(string)entry.Value;
        string[] pairs={"AUTOTEST=ExecutionPath","INPUTBOT=0","PLAYBOT=0","ACTIONBOT=0","HUMAN=1","HUMAN_FIXTURE=0","PATHSYNC=1","AMBIENT_SPLIT=1","CLEAN_EXIT=1","NET_FRAMES=0","NET_ADDRESS=127.0.0.1","NET_PORT=49375","AREA=Tokyo/1_Level/1_Area","CONTENT_SHA256="+Critical};
        foreach(string pair in pairs){int i=pair.IndexOf('=');d["COOP_PROBE_"+pair.Substring(0,i)]=pair.Substring(i+1);}
        // The game shows this while it waits for the partner (e.g. still in a cutscene), in the launcher's language.
        d["COOP_PROBE_WAIT_TEXT"]=UiText.Get("partnerWait");
        if(automated){d["COOP_PROBE_HUMAN_FIXTURE"]="1";d["COOP_PROBE_NET_FRAMES"]=ConnectionEvidence.Frames.ToString(CultureInfo.InvariantCulture);d["COOP_PROBE_DIAGNOSTICS"]="1";}
        else{
            // Play: retail New Game -> hub -> missions. The host's controls drive menus and the hub;
            // the automated connection check keeps the bounded research scenario.
            d.Remove("COOP_PROBE_AUTOTEST");d.Remove("COOP_PROBE_AREA");d["COOP_PROBE_CAMPAIGN"]="1";d["COOP_PROBE_CAMPAIGN_FIXTURE"]="0";
        }
        return d;
    }
    public static string NativeArguments(string game,string sandbox,bool automated){return (automated?"":"--play ")+Quote(game)+" "+Quote(sandbox)+(automated?" 300":"");}
    public static string Quote(string s) {
        // CommandLineToArgvW quoting, including quotes and trailing backslashes.
        return "\""+Regex.Replace(s,@"(\\*)\""",m=>new string('\\',m.Groups[1].Length*2+1)+"\"").TrimEnd('\\')+new string('\\',s.Reverse().TakeWhile(c=>c=='\\').Count()*2)+"\"";
    }
    public static void NewJson(string path,object data){using(var stream=new FileStream(path,FileMode.CreateNew,FileAccess.Write,FileShare.Read))using(var w=new StreamWriter(stream,new UTF8Encoding(false)))w.WriteLine(Json.Serialize(data));}
}
// Players should not have to know what steam_api64.dll is. Accept any copy that Valve
// signed (intact signature), is x64 and exports every flat function the native relay
// loads; the relay re-checks the same SHA-256 when it loads the file.
static class SteamDll {
    static readonly string[] Required={"Shutdown","GetHSteamPipe","ManualDispatch_Init","ManualDispatch_RunFrame","ManualDispatch_GetNextCallback","ManualDispatch_FreeLastCallback","ManualDispatch_GetAPICallResult","SteamUser_v023","SteamMatchmaking_v009","SteamNetworkingMessages_SteamAPI_v002","ISteamUser_GetSteamID","ISteamUser_BLoggedOn","ISteamMatchmaking_CreateLobby","ISteamMatchmaking_JoinLobby","ISteamMatchmaking_LeaveLobby","ISteamMatchmaking_GetLobbyOwner","ISteamMatchmaking_GetNumLobbyMembers","ISteamMatchmaking_GetLobbyMemberByIndex","ISteamMatchmaking_SetLobbyData","ISteamMatchmaking_GetLobbyData","SteamNetworkingIdentity_SetSteamID64","SteamNetworkingIdentity_GetSteamID64","ISteamNetworkingMessages_SendMessageToUser","ISteamNetworkingMessages_ReceiveMessagesOnChannel","ISteamNetworkingMessages_AcceptSessionWithUser","ISteamNetworkingMessages_CloseSessionWithUser","SteamNetworkingMessage_t_Release"};
    // Returns null when the x64 export table has everything the relay needs, else the reason.
    public static string ExportProblem(byte[] b){
        try{
            if(BitConverter.ToUInt16(b,0)!=0x5a4d)return "not a Windows DLL";int pe=BitConverter.ToInt32(b,0x3c);
            if(BitConverter.ToUInt32(b,pe)!=0x4550||BitConverter.ToUInt16(b,pe+4)!=0x8664)return "not 64-bit";
            int sections=BitConverter.ToUInt16(b,pe+6),opt=pe+24,table=opt+BitConverter.ToUInt16(b,pe+20);if(BitConverter.ToUInt16(b,opt)!=0x20b)return "not 64-bit";
            int exportRva=BitConverter.ToInt32(b,opt+112);if(exportRva==0)return "no exported functions";
            Func<int,int> offset=rva=>{for(int i=0;i<sections;i++){int h=table+i*40,size=Math.Max(BitConverter.ToInt32(b,h+8),BitConverter.ToInt32(b,h+16)),va=BitConverter.ToInt32(b,h+12);if(rva>=va&&rva<va+size)return rva-va+BitConverter.ToInt32(b,h+20);}throw new Rejected("bad RVA");};
            int e=offset(exportRva),count=BitConverter.ToInt32(b,e+24),names=offset(BitConverter.ToInt32(b,e+32));if(count<0||count>100000)return "bad export table";
            var found=new HashSet<string>(StringComparer.Ordinal);
            for(int i=0;i<count;i++){int n=offset(BitConverter.ToInt32(b,names+i*4)),end=Array.IndexOf(b,(byte)0,n);if(end<0)return "bad export name";found.Add(Encoding.ASCII.GetString(b,n,end-n));}
            if(!found.Contains("SteamAPI_InitFlat")&&!found.Contains("SteamAPI_Init"))return "too old (no Steam start function)";
            foreach(string name in Required)if(!found.Contains("SteamAPI_"+name))return "too old (missing "+name+")";
            return null;
        }catch(Exception){return "unreadable DLL";}
    }
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct FileInfo{public int cbStruct;public string path;public IntPtr file;public IntPtr known;}
    [StructLayout(LayoutKind.Sequential)] struct TrustData{public int cbStruct;public IntPtr policy,sip;public int ui,revocation,choice;public IntPtr file;public int action;public IntPtr state;public IntPtr url;public int provider,context;public IntPtr signature;}
    [DllImport("wintrust.dll",CharSet=CharSet.Unicode)] static extern int WinVerifyTrust(IntPtr window,ref Guid action,ref TrustData data);
    // WinVerifyTrust checks the embedded signature against the file bytes; a modified file fails.
    public static bool ValveSigned(string path){
        var info=new FileInfo{cbStruct=Marshal.SizeOf(typeof(FileInfo)),path=path};IntPtr pinfo=Marshal.AllocHGlobal(info.cbStruct);
        try{
            Marshal.StructureToPtr(info,pinfo,false);var guid=new Guid("00AAC56B-CD44-11d0-8CC2-00C04FC295EE");
            var data=new TrustData{cbStruct=Marshal.SizeOf(typeof(TrustData)),ui=2,revocation=0,choice=1,file=pinfo,action=1,provider=0x10};
            int verdict=WinVerifyTrust(new IntPtr(-1),ref guid,ref data);data.action=2;WinVerifyTrust(new IntPtr(-1),ref guid,ref data);
            if(verdict!=0)return false;
            var subject=System.Security.Cryptography.X509Certificates.X509Certificate.CreateFromSignedFile(path).Subject;
            return Regex.IsMatch(subject,@"(^|,\s*)(CN|O)=""?Valve( Corp\.?)?""?(,|$)");
        }catch(Exception){return false;}finally{Marshal.DestroyStructure(pinfo,typeof(FileInfo));Marshal.FreeHGlobal(pinfo);}
    }
    public static string Problem(string path){
        if(!File.Exists(path))return "file not found";if(!string.Equals(Path.GetFileName(path),"steam_api64.dll",StringComparison.OrdinalIgnoreCase))return "must be named steam_api64.dll";
        string problem=ExportProblem(File.ReadAllBytes(path));if(problem!=null)return problem;
        return ValveSigned(path)?null:"not signed by Valve, or the file was modified";
    }
    public static IEnumerable<string> Libraries(){
        var roots=new List<string>();
        foreach(var hive in new[]{Registry.CurrentUser,Registry.LocalMachine})foreach(string key in new[]{@"Software\Valve\Steam",@"Software\WOW6432Node\Valve\Steam"}){
            try{using(var k=hive.OpenSubKey(key)){object v=k==null?null:(k.GetValue("SteamPath")??k.GetValue("InstallPath"));if(v is string&&((string)v).Length>0)roots.Add(((string)v).Replace('/','\\'));}}catch(Exception){}
        }
        var libraries=new List<string>();
        foreach(string root in roots){
            libraries.Add(root);string vdf=Path.Combine(root,"steamapps","libraryfolders.vdf");
            try{if(File.Exists(vdf))foreach(Match m in Regex.Matches(File.ReadAllText(vdf),"\"path\"\\s+\"([^\"]+)\""))libraries.Add(m.Groups[1].Value.Replace("\\\\","\\"));}catch(Exception){}
        }
        return libraries.Select(l=>{try{return Path.GetFullPath(l);}catch(Exception){return null;}}).Where(l=>l!=null).Distinct(StringComparer.OrdinalIgnoreCase);
    }
    public static string SettingsFile{get{return Path.Combine(Files.Root,"steam-component.json");}}
    // Returns the remembered path only while it still passes every check.
    public static string LoadSaved(string file){
        try{if(!File.Exists(file))return null;var data=new JavaScriptSerializer().Deserialize<Dictionary<string,object>>(File.ReadAllText(file));
            object value;string path=data!=null&&data.TryGetValue("steam_component",out value)?value as string:null;return path!=null&&Problem(path)==null?path:null;}catch(Exception){return null;}
    }
    public static void Save(string file,string path){
        string temporary=file+".tmp";File.WriteAllText(temporary,new JavaScriptSerializer().Serialize(new Dictionary<string,object>{{"steam_component",path}}),new UTF8Encoding(false));
        if(File.Exists(file))File.Replace(temporary,file,null);else File.Move(temporary,file);
    }
    // Searches installed Steam games only (read-only); returns the first acceptable copy and its game folder.
    public static Tuple<string,string> Find(Action<string> note){
        var seen=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach(string library in Libraries()){
            string common=Path.Combine(library,"steamapps","common");if(!Directory.Exists(common))continue;
            IEnumerable<string> games;try{games=Directory.GetDirectories(common);}catch(Exception){continue;}
            foreach(string game in games){
                string[] files;try{files=Directory.GetFiles(game,"steam_api64.dll",SearchOption.AllDirectories);}catch(Exception){continue;}
                foreach(string file in files){
                    string sha;try{sha=Files.HashFile(file).Sha;}catch(Exception){continue;}if(!seen.Add(sha))continue;
                    string problem=Problem(file);if(problem==null)return Tuple.Create(file,Path.GetFileName(game));
                    if(note!=null)note("Skipped Steam component in "+Path.GetFileName(game)+": "+problem);
                }
            }
        }
        return null;
    }
}
// Each player chooses their own display; the probe applies it (any resolution, borderless or window).
static class Display {
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct DevMode{
        [MarshalAs(UnmanagedType.ByValTStr,SizeConst=32)]public string name;public short specVersion,driverVersion,size,driverExtra;public int fields;
        public int positionX,positionY,orientation,fixedOutput;public short color,duplex,yResolution,ttOption,collate;
        [MarshalAs(UnmanagedType.ByValTStr,SizeConst=32)]public string formName;public short logPixels;public int bitsPerPel,width,height,flags,frequency;
        public int icmMethod,icmIntent,mediaType,ditherType,reserved1,reserved2,panningWidth,panningHeight;}
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern bool EnumDisplaySettings(string device,int mode,ref DevMode devMode);
    public const string Borderless="Borderless window (Alt+Tab friendly)",Window="Window",Original="Game's own settings (original fullscreen)";
    public static string SettingsFile{get{return Path.Combine(Files.Root,"display.json");}}
    public static List<string> Resolutions(){
        var found=new HashSet<string>();var list=new List<Tuple<int,int>>();var mode=new DevMode{size=(short)Marshal.SizeOf(typeof(DevMode))};
        for(int i=0;EnumDisplaySettings(null,i,ref mode);i++)if(mode.width>=640&&mode.height>=480&&mode.width<=7680&&mode.height<=4320&&found.Add(mode.width+"x"+mode.height))list.Add(Tuple.Create(mode.width,mode.height));
        string desktop=Desktop;if(found.Add(desktop)){var parts=desktop.Split('x');list.Add(Tuple.Create(int.Parse(parts[0],CultureInfo.InvariantCulture),int.Parse(parts[1],CultureInfo.InvariantCulture)));}
        return list.OrderByDescending(r=>r.Item1).ThenByDescending(r=>r.Item2).Select(r=>r.Item1+"x"+r.Item2).ToList();
    }
    // Real pixels of the current mode; Screen.Bounds is scaled down by Windows display scaling (e.g. 125%).
    public static string Desktop{get{var mode=new DevMode{size=(short)Marshal.SizeOf(typeof(DevMode))};if(EnumDisplaySettings(null,-1,ref mode)&&mode.width>0)return mode.width+"x"+mode.height;var d=Screen.PrimaryScreen.Bounds;return d.Width+"x"+d.Height;}}
    public static bool Valid(string resolution){var m=Regex.Match(resolution??"",@"^([0-9]{3,4})x([0-9]{3,4})$");if(!m.Success)return false;int w=int.Parse(m.Groups[1].Value,CultureInfo.InvariantCulture),h=int.Parse(m.Groups[2].Value,CultureInfo.InvariantCulture);return w>=640&&w<=7680&&h>=480&&h<=4320;}
    public static Dictionary<string,string> Load(string file){
        try{var data=new JavaScriptSerializer().Deserialize<Dictionary<string,object>>(File.ReadAllText(file));var result=new Dictionary<string,string>();
            object mode,resolution;if(data!=null&&data.TryGetValue("mode",out mode)&&(mode as string==Borderless||mode as string==Window||mode as string==Original))result["mode"]=(string)mode;
            if(data!=null&&data.TryGetValue("resolution",out resolution)&&Valid(resolution as string))result["resolution"]=(string)resolution;return result;}catch(Exception){return new Dictionary<string,string>();}
    }
    public static void Save(string file,string mode,string resolution){
        string temporary=file+".tmp";File.WriteAllText(temporary,new JavaScriptSerializer().Serialize(new Dictionary<string,object>{{"mode",mode},{"resolution",resolution}}),new UTF8Encoding(false));
        if(File.Exists(file))File.Replace(temporary,file,null);else File.Move(temporary,file);
    }
    public static void Apply(Dictionary<string,string> env,string mode,string resolution){
        // Original: the game's own fullscreen and the resolution saved in its options (read only by the add-on).
        if(mode==Original){env["COOP_PROBE_DISPLAY_MODE"]="original";env.Remove("COOP_PROBE_RESOLUTION");return;}
        if(!Valid(resolution))throw new Rejected("Choose a valid resolution");
        env["COOP_PROBE_DISPLAY_MODE"]=mode==Window?"window":"borderless";env["COOP_PROBE_RESOLUTION"]=resolution;
    }
}
// Remembers the last game folder imported for each language, so the folder dialog opens there.
static class ImportMemory {
    public static string SettingsFile{get{return Path.Combine(Files.Root,"import.json");}}
    public static Dictionary<string,string> Load(string file){
        var result=new Dictionary<string,string>();
        try{var data=new JavaScriptSerializer().Deserialize<Dictionary<string,object>>(File.ReadAllText(file));if(data!=null)foreach(var pair in data){string path=pair.Value as string;if((pair.Key=="English"||pair.Key=="Russian")&&path!=null&&path.Length<32768)result[pair.Key]=path;}}catch(Exception){}
        return result;
    }
    public static string Folder(string file,string language){string path;return Load(file).TryGetValue(language,out path)&&Directory.Exists(path)?path:null;}
    public static void Save(string file,string language,string path){
        var all=Load(file);all[language]=path;string temporary=file+".tmp";
        File.WriteAllText(temporary,new JavaScriptSerializer().Serialize(all.ToDictionary(pair=>pair.Key,pair=>(object)pair.Value)),new UTF8Encoding(false));
        if(File.Exists(file))File.Replace(temporary,file,null);else File.Move(temporary,file);
    }
}
// Launcher window texts. The game's own language (English/Russian files) is chosen separately.
static class UiText {
    public static readonly string[][] Languages={new[]{"en","English"},new[]{"ru","Русский"},new[]{"tr","Türkçe"},new[]{"uk","Українська"},new[]{"de","Deutsch"},new[]{"fr","Français"},new[]{"es","Español"},new[]{"pt","Português (Brasil)"},new[]{"it","Italiano"},new[]{"pl","Polski"},new[]{"zh","简体中文"}};
    public static string Current="en";
    static readonly Dictionary<string,Dictionary<string,string>> Texts=new Dictionary<string,Dictionary<string,string>>{
        {"en",new Dictionary<string,string>{{"title","Friend or Foe — online co-op (private research)"},{"warning","PRIVATE RESEARCH • Campaign: choose New Game and the first save slot. The host controls menus and the hub.\nSaving/continuing and remote play over Steam are not yet verified.\nHost / Join starts Steam Spacewar with your signed-in account. No invitations are sent."},{"uiLanguage","Launcher language"},{"gameLanguage","Game language"},{"files","Game files"},{"steam","Steam component"},{"display","Display"},{"lobby","Lobby code"},{"import","Import installed folder"},{"verify","Verify copy"},{"choose","Choose manually..."},{"open","Play Game"},{"copy","Copy code"},{"host","Host"},{"join","Join"},{"stop","Stop session"},{"hint","Host: click “Play Game” and play alone (New Game or Continue). To invite a friend, press Alt+Tab, click Host and send the code; your friend appears when you are in the Helicarrier.\nFriend: enter the code and click Join. Your game opens, starts by itself and catches up with the host; just wait."},{"automated","Automated connection test (both players enable it; scripted controls, ends automatically)"},{"together","Start a new campaign together (both tick this; you both see the opening)"},{"togetherStarted","Partner connected; both games are starting. Skip the logo videos. The host picks New Game and an empty save slot (used slots can't be chosen), then you both watch the opening."},{"togetherClose","Close the open game first: starting together opens a fresh game for both of you."},{"partnerWait","Waiting for your partner…\nThey may still be watching a cutscene. The game continues as soon as they catch up."},{"steamNotRunning","Couldn't reach Steam. Open Steam and sign in (if it's already open, restart it), then click Host or Join again."},{"relayLost","The online connection stopped. Check that Steam is open and your internet works, then click Host or Join again."},{"statusReady","Copy available; verified before each launch"},{"statusImport","Import required"},{"modeBorderless","Borderless window (Alt+Tab friendly)"},{"modeWindow","Window"},{"modeOriginal","Game's own settings (original fullscreen)"},{"steamLooking","Looking for the Steam component in your installed Steam games..."},{"steamFound","Steam component found (from {0}) - OK"},{"steamRemembered","Steam component remembered from last time - OK"},{"steamNone","No compatible Steam component found. Install any recent free Steam game (for example Counter-Strike 2), then reopen this window, or use \"Choose manually...\"."},{"steamSelected","Steam component selected - OK"},{"steamBad","This Steam component cannot be used: {0}"},{"gameOpen","Game opened ({0}). It waits at the main menu until you click Host or Join."},{"gameClosed","Game closed."},{"gameStopped","The add-on stopped the game because a safety check failed. Please send this file so it can be fixed: {0}"},{"sessionStopped","Session ended. Your saves stay in this package's profile folder."},{"partnerLinkedHost","Friend connected. They join you in the Helicarrier hub; if you are on a mission, finish it or return there."},{"partnerLinkedJoin","Connected. Your game catches up with the host by itself (New Game, then the Helicarrier); please wait without pressing anything."},{"partnerLeft","Your friend left. You keep playing; click Host again to invite them back (they rejoin you in the Helicarrier)."},{"steamStarted","Steam started. Waiting for the lobby and your friend."},{"lobbyReady","Lobby ready. Send the code to your friend."},{"imported","Imported and verified the {0} game copy."},{"verified","OK: {0} game copy verified (1291 files)."},{"importDialog","Choose your installed game folder. It is only read; verified game files are copied."},{"gameEnglish","English"},{"gameRussian","Russian"}}},
        {"ru",new Dictionary<string,string>{{"title","Friend or Foe — онлайн-кооператив (тестовая версия)"},{"warning","ТЕСТОВАЯ ВЕРСИЯ • Кампания: выберите «Новая игра» и первый слот сохранения. Меню и базой управляет хост.\nСохранение/продолжение и игра по сети через Steam ещё не проверены.\n«Создать» / «Подключиться» запускает Steam Spacewar с вашим аккаунтом. Приглашения не отправляются."},{"uiLanguage","Язык программы"},{"gameLanguage","Язык игры"},{"files","Файлы игры"},{"steam","Компонент Steam"},{"display","Экран"},{"lobby","Код лобби"},{"import","Импорт папки с игрой"},{"verify","Проверить копию"},{"choose","Выбрать вручную..."},{"open","Играть"},{"copy","Копировать код"},{"host","Создать"},{"join","Подключиться"},{"stop","Остановить сессию"},{"hint","Хост: нажмите «Играть» и играйте один («Новая игра» или «Продолжить»). Чтобы позвать друга, нажмите Alt+Tab, затем «Создать» и отправьте код; друг появится, когда вы будете в Геликарриере.\nДруг: введите код и нажмите «Подключиться». Ваша игра откроется, запустится сама и догонит хоста; просто подождите."},{"automated","Автоматическая проверка соединения (включают оба игрока; управление по сценарию, завершается сама)"},{"together","Начать новую кампанию вместе (отметьте оба; вы оба увидите вступление)"},{"togetherStarted","Партнёр подключён, обе игры запускаются. Пропустите заставки. Хост выбирает «Новая игра» и пустой слот (занятые выбрать нельзя), затем вы оба смотрите вступление."},{"togetherClose","Сначала закройте открытую игру: совместный старт открывает новую игру у вас обоих."},{"partnerWait","Ждём напарника…\nВозможно, он ещё смотрит заставку. Игра продолжится, как только он догонит."},{"steamNotRunning","Не удалось подключиться к Steam. Откройте Steam и войдите в аккаунт (если он уже открыт — перезапустите его), затем снова нажмите «Создать» или «Подключиться»."},{"relayLost","Онлайн-соединение прервалось. Проверьте, что Steam открыт и интернет работает, затем снова нажмите «Создать» или «Подключиться»."},{"statusReady","Копия есть; проверяется перед каждым запуском"},{"statusImport","Нужен импорт"},{"modeBorderless","Окно без рамки (удобно для Alt+Tab)"},{"modeWindow","Окно"},{"modeOriginal","Настройки самой игры (оригинальный полный экран)"},{"steamLooking","Ищем компонент Steam в установленных играх Steam..."},{"steamFound","Компонент Steam найден (из {0}) - OK"},{"steamRemembered","Компонент Steam запомнен с прошлого раза - OK"},{"steamNone","Подходящий компонент Steam не найден. Установите любую недавнюю бесплатную игру из Steam (например, Counter-Strike 2) и откройте это окно снова или нажмите «Выбрать вручную...»."},{"steamSelected","Компонент Steam выбран - OK"},{"steamBad","Этот компонент Steam не подходит: {0}"},{"gameOpen","Игра открыта ({0}). Она ждёт в главном меню, пока вы не нажмёте «Создать» или «Подключиться»."},{"gameClosed","Игра закрыта."},{"gameStopped","Дополнение остановило игру, потому что не прошла проверка безопасности. Пришлите этот файл, чтобы это исправить: {0}"},{"sessionStopped","Сеанс завершён. Ваши сохранения остаются в папке profile этого пакета."},{"partnerLinkedHost","Друг подключился. Он присоединится к вам в Геликарриере; если вы на задании, завершите его или вернитесь туда."},{"partnerLinkedJoin","Подключено. Ваша игра сама догоняет хоста («Новая игра», затем Геликарриер); подождите, ничего не нажимая."},{"partnerLeft","Друг вышел. Вы продолжаете играть; нажмите «Создать» снова, чтобы позвать его обратно (он вернётся к вам в Геликарриере)."},{"steamStarted","Steam запущен. Ждём лобби и вашего друга."},{"lobbyReady","Лобби готово. Отправьте код другу."},{"imported","Копия игры ({0}) импортирована и проверена."},{"verified","OK: копия игры ({0}) проверена (1291 файл)."},{"importDialog","Выберите папку с установленной игрой. Она только читается; копируются проверенные файлы игры."},{"gameEnglish","Английский"},{"gameRussian","Русский"}}},
        {"tr",new Dictionary<string,string>{{"title","Friend or Foe — çevrimiçi co-op (özel test sürümü)"},{"warning","ÖZEL TEST SÜRÜMÜ • Kampanya: Yeni Oyun'u ve ilk kayıt yuvasını seçin. Menüleri ve üssü host yönetir.\nKaydetme/devam etme ve Steam üzerinden uzaktan oyun henüz doğrulanmadı.\nKur / Katıl, Steam Spacewar'ı oturum açmış hesabınızla başlatır. Davet gönderilmez."},{"uiLanguage","Başlatıcı dili"},{"gameLanguage","Oyun dili"},{"files","Oyun dosyaları"},{"steam","Steam bileşeni"},{"display","Ekran"},{"lobby","Lobi kodu"},{"import","Kurulu klasörü içe aktar"},{"verify","Kopyayı doğrula"},{"choose","Elle seç..."},{"open","Oyna"},{"copy","Kodu kopyala"},{"host","Kur (host)"},{"join","Katıl"},{"stop","Oturumu durdur"},{"hint","Ev sahibi: «Oyna» düğmesine basın ve tek başınıza oynayın (Yeni Oyun ya da Devam). Arkadaş çağırmak için Alt+Tab yapın, «Kur (host)» düğmesine basın ve kodu gönderin; Helicarrier'dayken arkadaşınız gelir.\nArkadaş: kodu girin ve «Katıl» düğmesine basın. Oyununuz açılır, kendi kendine başlar ve ev sahibine yetişir; sadece bekleyin."},{"automated","Otomatik bağlantı testi (iki oyuncu da açar; senaryolu kontroller, kendiliğinden biter)"},{"together","Yeni bir kampanyaya birlikte başla (ikiniz de işaretleyin; açılışı ikiniz de görürsünüz)"},{"togetherStarted","Ortak bağlandı; iki oyun da başlıyor. Logo videolarını geçin. Host Yeni Oyun'u ve boş bir kayıt yuvasını seçer (dolu yuvalar seçilemez), sonra açılışı birlikte izlersiniz."},{"togetherClose","Önce açık oyunu kapatın: birlikte başlamak ikiniz için de yeni bir oyun açar."},{"partnerWait","Ortağınız bekleniyor…\nHâlâ bir ara sahne izliyor olabilir. Size yetiştiği anda oyun devam eder."},{"steamNotRunning","Steam'e ulaşılamadı. Steam'i açıp giriş yapın (zaten açıksa yeniden başlatın), sonra tekrar Kur veya Katıl'a basın."},{"relayLost","Çevrimiçi bağlantı kesildi. Steam'in açık olduğunu ve internetin çalıştığını kontrol edin, sonra tekrar Kur veya Katıl'a basın."},{"statusReady","Kopya hazır; her başlatmadan önce doğrulanır"},{"statusImport","İçe aktarma gerekli"},{"modeBorderless","Kenarlıksız pencere (Alt+Tab dostu)"},{"modeWindow","Pencere"},{"modeOriginal","Oyunun kendi ayarları (orijinal tam ekran)"},{"steamLooking","Kurulu Steam oyunlarınızda Steam bileşeni aranıyor..."},{"steamFound","Steam bileşeni bulundu ({0} oyunundan) - Tamam"},{"steamRemembered","Steam bileşeni geçen seferden hatırlandı - Tamam"},{"steamNone","Uyumlu bir Steam bileşeni bulunamadı. Yakın tarihli herhangi bir ücretsiz Steam oyunu kurun (örneğin Counter-Strike 2), sonra bu pencereyi yeniden açın ya da \"Elle seç...\" düğmesini kullanın."},{"steamSelected","Steam bileşeni seçildi - Tamam"},{"steamBad","Bu Steam bileşeni kullanılamaz: {0}"},{"gameOpen","Oyun açıldı ({0}). Siz «Kur (host)» ya da «Katıl» düğmesine basana kadar ana menüde bekler."},{"gameClosed","Oyun kapatıldı."},{"gameStopped","Bir güvenlik denetimi başarısız olduğu için eklenti oyunu durdurdu. Düzeltilmesi için lütfen bu dosyayı gönderin: {0}"},{"sessionStopped","Oturum sona erdi. Kayıtlarınız bu paketin profile klasöründe kalır."},{"partnerLinkedHost","Arkadaşınız bağlandı. Size Helicarrier'da katılacak; görevdeyseniz görevi bitirin ya da oraya dönün."},{"partnerLinkedJoin","Bağlandı. Oyununuz ev sahibine kendi kendine yetişiyor (Yeni Oyun, sonra Helicarrier); hiçbir şeye basmadan bekleyin."},{"partnerLeft","Arkadaşınız ayrıldı. Oynamaya devam edersiniz; onu geri çağırmak için yeniden «Kur (host)» düğmesine basın (Helicarrier'da size yeniden katılır)."},{"steamStarted","Steam başlatıldı. Lobi ve arkadaşınız bekleniyor."},{"lobbyReady","Lobi hazır. Kodu arkadaşınıza gönderin."},{"imported","{0} oyun kopyası içe aktarıldı ve doğrulandı."},{"verified","Tamam: {0} oyun kopyası doğrulandı (1291 dosya)."},{"importDialog","Kurulu oyun klasörünüzü seçin. Yalnızca okunur; doğrulanmış oyun dosyaları kopyalanır."},{"gameEnglish","İngilizce"},{"gameRussian","Rusça"}}},
        {"uk",new Dictionary<string,string>{{"title","Friend or Foe — онлайн-кооператив (тестова версія)"},{"warning","ТЕСТОВА ВЕРСІЯ • Кампанія: оберіть «Нова гра» і перший слот збереження. Меню та базою керує хост.\nЗбереження/продовження та гра через Steam ще не перевірені.\n«Створити» / «Приєднатися» запускає Steam Spacewar з вашим акаунтом. Запрошення не надсилаються."},{"uiLanguage","Мова програми"},{"gameLanguage","Мова гри"},{"files","Файли гри"},{"steam","Компонент Steam"},{"display","Екран"},{"lobby","Код лобі"},{"import","Імпорт теки з грою"},{"verify","Перевірити копію"},{"choose","Обрати вручну..."},{"open","Грати"},{"copy","Копіювати код"},{"host","Створити"},{"join","Приєднатися"},{"stop","Зупинити сесію"},{"hint","Хост: натисніть «Грати» і грайте сам («Нова гра» або «Продовжити»). Щоб покликати друга, натисніть Alt+Tab, потім «Створити» і надішліть код; друг з'явиться, коли ви будете в Гелікерієрі.\nДруг: введіть код і натисніть «Приєднатися». Ваша гра відкриється, запуститься сама й наздожене хоста; просто зачекайте."},{"automated","Автоматична перевірка з'єднання (вмикають обидва гравці; керування за сценарієм, завершується сама)"},{"together","Почати нову кампанію разом (позначте обидва; ви обидва побачите вступ)"},{"togetherStarted","Партнер підключився, обидві гри запускаються. Пропустіть заставки. Хост обирає «Нова гра» і порожній слот (зайняті обрати не можна), потім ви обидва дивитеся вступ."},{"togetherClose","Спершу закрийте відкриту гру: спільний старт відкриває нову гру у вас обох."},{"partnerWait","Чекаємо на напарника…\nМожливо, він ще дивиться заставку. Гра продовжиться, щойно він наздожене."},{"steamNotRunning","Не вдалося підключитися до Steam. Відкрийте Steam і увійдіть (якщо він уже відкритий — перезапустіть його), потім знову натисніть «Створити» або «Приєднатися»."},{"relayLost","Онлайн-з'єднання перервалося. Перевірте, що Steam відкритий і інтернет працює, потім знову натисніть «Створити» або «Приєднатися»."},{"statusReady","Копія є; перевіряється перед кожним запуском"},{"statusImport","Потрібен імпорт"},{"modeBorderless","Вікно без рамки (зручно для Alt+Tab)"},{"modeWindow","Вікно"},{"modeOriginal","Налаштування самої гри (оригінальний повний екран)"},{"steamLooking","Шукаємо компонент Steam у встановлених іграх Steam..."},{"steamFound","Компонент Steam знайдено (з {0}) - OK"},{"steamRemembered","Компонент Steam запам'ятовано з минулого разу - OK"},{"steamNone","Відповідний компонент Steam не знайдено. Встановіть будь-яку нещодавню безкоштовну гру зі Steam (наприклад, Counter-Strike 2) і знову відкрийте це вікно або натисніть «Обрати вручну...»."},{"steamSelected","Компонент Steam обрано - OK"},{"steamBad","Цей компонент Steam не підходить: {0}"},{"gameOpen","Гру відкрито ({0}). Вона чекає в головному меню, доки ви не натиснете «Створити» або «Приєднатися»."},{"gameClosed","Гру закрито."},{"gameStopped","Доповнення зупинило гру, бо не пройшла перевірка безпеки. Надішліть цей файл, щоб це виправити: {0}"},{"sessionStopped","Сеанс завершено. Ваші збереження лишаються в папці profile цього пакета."},{"partnerLinkedHost","Друг підключився. Він приєднається до вас у Гелікерієрі; якщо ви на завданні, завершіть його або поверніться туди."},{"partnerLinkedJoin","Підключено. Ваша гра сама наздоганяє хоста («Нова гра», потім Гелікерієр); зачекайте, нічого не натискаючи."},{"partnerLeft","Друг вийшов. Ви граєте далі; натисніть «Створити» знову, щоб покликати його назад (він повернеться до вас у Гелікерієрі)."},{"steamStarted","Steam запущено. Чекаємо на лобі та вашого друга."},{"lobbyReady","Лобі готове. Надішліть код другові."},{"imported","Копію гри ({0}) імпортовано та перевірено."},{"verified","OK: копію гри ({0}) перевірено (1291 файл)."},{"importDialog","Оберіть теку зі встановленою грою. Вона лише читається; копіюються перевірені файли гри."},{"gameEnglish","Англійська"},{"gameRussian","Російська"}}},
        {"de",new Dictionary<string,string>{{"title","Friend or Foe — Online-Koop (private Testversion)"},{"warning","PRIVATE TESTVERSION • Kampagne: Neues Spiel und den ersten Speicherplatz wählen. Der Host steuert Menüs und den Hub.\nSpeichern/Fortsetzen und Online-Spiel über Steam sind noch nicht geprüft.\nHosten / Beitreten startet Steam Spacewar mit deinem angemeldeten Konto. Es werden keine Einladungen gesendet."},{"uiLanguage","Sprache des Launchers"},{"gameLanguage","Spielsprache"},{"files","Spieldateien"},{"steam","Steam-Komponente"},{"display","Anzeige"},{"lobby","Lobby-Code"},{"import","Installierten Ordner importieren"},{"verify","Kopie prüfen"},{"choose","Manuell wählen..."},{"open","Spielen"},{"copy","Code kopieren"},{"host","Hosten"},{"join","Beitreten"},{"stop","Sitzung beenden"},{"hint","Host: „Spielen“ wählen und allein spielen (Neues Spiel oder Fortsetzen). Um einen Freund einzuladen, Alt+Tab drücken, „Hosten“ wählen und den Code schicken; der Freund kommt dazu, wenn Sie im Helicarrier sind.\nFreund: Code eingeben und „Beitreten“ wählen. Das Spiel öffnet sich, startet von selbst und holt den Host ein; einfach warten."},{"automated","Automatischer Verbindungstest (beide Spieler aktivieren ihn; gescriptete Steuerung, endet automatisch)"},{"together","Neue Kampagne gemeinsam starten (beide ankreuzen; ihr seht beide den Anfang)"},{"togetherStarted","Partner verbunden; beide Spiele starten. Logo-Videos überspringen. Der Host wählt Neues Spiel und einen freien Speicherplatz (belegte sind gesperrt), dann seht ihr beide den Anfang."},{"togetherClose","Schließe zuerst das offene Spiel: Gemeinsam starten öffnet für euch beide ein neues Spiel."},{"partnerWait","Warte auf deinen Mitspieler…\nEr schaut vielleicht noch eine Zwischensequenz. Das Spiel geht weiter, sobald er aufholt."},{"steamNotRunning","Steam ist nicht erreichbar. Öffne Steam und melde dich an (läuft es schon, starte es neu), dann klicke erneut auf Hosten oder Beitreten."},{"relayLost","Die Online-Verbindung wurde unterbrochen. Prüfe, ob Steam läuft und das Internet funktioniert, dann klicke erneut auf Hosten oder Beitreten."},{"statusReady","Kopie vorhanden; wird vor jedem Start geprüft"},{"statusImport","Import erforderlich"},{"modeBorderless","Randloses Fenster (Alt+Tab-freundlich)"},{"modeWindow","Fenster"},{"modeOriginal","Eigene Einstellungen des Spiels (originaler Vollbildmodus)"},{"steamLooking","Suche die Steam-Komponente in deinen installierten Steam-Spielen..."},{"steamFound","Steam-Komponente gefunden (aus {0}) - OK"},{"steamRemembered","Steam-Komponente vom letzten Mal gemerkt - OK"},{"steamNone","Keine passende Steam-Komponente gefunden. Installiere ein beliebiges aktuelles kostenloses Steam-Spiel (zum Beispiel Counter-Strike 2) und öffne dieses Fenster erneut, oder nutze „Manuell wählen...“."},{"steamSelected","Steam-Komponente gewählt - OK"},{"steamBad","Diese Steam-Komponente kann nicht verwendet werden: {0}"},{"gameOpen","Spiel geöffnet ({0}). Es wartet im Hauptmenü, bis Sie „Hosten“ oder „Beitreten“ wählen."},{"gameClosed","Spiel geschlossen."},{"gameStopped","Das Add-on hat das Spiel gestoppt, weil eine Sicherheitsprüfung fehlschlug. Bitte senden Sie diese Datei, damit es behoben werden kann: {0}"},{"sessionStopped","Sitzung beendet. Ihre Spielstände bleiben im Ordner profile dieses Pakets."},{"partnerLinkedHost","Freund verbunden. Er stößt im Helicarrier zu Ihnen; sind Sie auf einer Mission, beenden Sie sie oder kehren Sie dorthin zurück."},{"partnerLinkedJoin","Verbunden. Ihr Spiel holt den Host von selbst ein (Neues Spiel, dann der Helicarrier); bitte warten, ohne etwas zu drücken."},{"partnerLeft","Ihr Freund ist gegangen. Sie spielen weiter; wählen Sie erneut „Hosten“, um ihn zurückzuholen (er stößt im Helicarrier wieder dazu)."},{"steamStarted","Steam gestartet. Warte auf die Lobby und deinen Freund."},{"lobbyReady","Lobby bereit. Schick deinem Freund den Code."},{"imported","Spielkopie ({0}) importiert und geprüft."},{"verified","OK: Spielkopie ({0}) geprüft (1291 Dateien)."},{"importDialog","Wähle deinen installierten Spielordner. Er wird nur gelesen; geprüfte Spieldateien werden kopiert."},{"gameEnglish","Englisch"},{"gameRussian","Russisch"}}},
        {"fr",new Dictionary<string,string>{{"title","Friend or Foe — coop en ligne (version de test privée)"},{"warning","VERSION DE TEST PRIVÉE • Campagne : choisissez Nouvelle partie et le premier emplacement de sauvegarde. L'hôte contrôle les menus et la base.\nLa sauvegarde/reprise et le jeu en ligne via Steam ne sont pas encore vérifiés.\nHéberger / Rejoindre lance Steam Spacewar avec votre compte connecté. Aucune invitation n'est envoyée."},{"uiLanguage","Langue du lanceur"},{"gameLanguage","Langue du jeu"},{"files","Fichiers du jeu"},{"steam","Composant Steam"},{"display","Affichage"},{"lobby","Code du salon"},{"import","Importer le dossier installé"},{"verify","Vérifier la copie"},{"choose","Choisir manuellement..."},{"open","Jouer"},{"copy","Copier le code"},{"host","Héberger"},{"join","Rejoindre"},{"stop","Arrêter la session"},{"hint","Hôte : cliquez sur « Jouer » et jouez seul (Nouvelle partie ou Continuer). Pour inviter un ami, faites Alt+Tab, cliquez sur « Héberger » et envoyez le code ; votre ami arrive quand vous êtes dans l'Héliporteur.\nAmi : saisissez le code et cliquez sur « Rejoindre ». Votre jeu s'ouvre, démarre tout seul et rattrape l'hôte ; patientez."},{"automated","Test de connexion automatique (les deux joueurs l'activent ; commandes scriptées, se termine seul)"},{"together","Commencer une nouvelle campagne ensemble (cochez tous les deux ; vous voyez tous deux l'introduction)"},{"togetherStarted","Partenaire connecté ; les deux jeux démarrent. Passez les logos. L'hôte choisit Nouvelle partie et un emplacement vide (les emplacements occupés sont bloqués), puis vous regardez l'introduction ensemble."},{"togetherClose","Fermez d'abord le jeu ouvert : commencer ensemble ouvre un nouveau jeu pour vous deux."},{"partnerWait","En attente de votre partenaire…\nIl regarde peut-être encore une cinématique. La partie reprend dès qu'il vous rattrape."},{"steamNotRunning","Impossible de joindre Steam. Ouvrez Steam et connectez-vous (s'il est déjà ouvert, redémarrez-le), puis cliquez à nouveau sur Héberger ou Rejoindre."},{"relayLost","La connexion en ligne s'est arrêtée. Vérifiez que Steam est ouvert et qu'Internet fonctionne, puis cliquez à nouveau sur Héberger ou Rejoindre."},{"statusReady","Copie disponible ; vérifiée avant chaque lancement"},{"statusImport","Importation nécessaire"},{"modeBorderless","Fenêtre sans bordure (compatible Alt+Tab)"},{"modeWindow","Fenêtre"},{"modeOriginal","Réglages du jeu (plein écran d'origine)"},{"steamLooking","Recherche du composant Steam dans vos jeux Steam installés..."},{"steamFound","Composant Steam trouvé (dans {0}) - OK"},{"steamRemembered","Composant Steam mémorisé depuis la dernière fois - OK"},{"steamNone","Aucun composant Steam compatible trouvé. Installez n'importe quel jeu Steam gratuit récent (par exemple Counter-Strike 2), puis rouvrez cette fenêtre, ou utilisez « Choisir manuellement... »."},{"steamSelected","Composant Steam sélectionné - OK"},{"steamBad","Ce composant Steam ne peut pas être utilisé : {0}"},{"gameOpen","Jeu ouvert ({0}). Il attend au menu principal jusqu'à ce que vous cliquiez sur « Héberger » ou « Rejoindre »."},{"gameClosed","Jeu fermé."},{"gameStopped","Le module a arrêté le jeu car une vérification de sécurité a échoué. Envoyez ce fichier pour que ce soit corrigé : {0}"},{"sessionStopped","Session terminée. Vos sauvegardes restent dans le dossier profile de ce paquet."},{"partnerLinkedHost","Ami connecté. Il vous rejoint dans l'Héliporteur ; si vous êtes en mission, terminez-la ou retournez-y."},{"partnerLinkedJoin","Connecté. Votre jeu rattrape l'hôte tout seul (Nouvelle partie, puis l'Héliporteur) ; patientez sans rien appuyer."},{"partnerLeft","Votre ami est parti. Vous continuez à jouer ; cliquez de nouveau sur « Héberger » pour le réinviter (il vous rejoint dans l'Héliporteur)."},{"steamStarted","Steam lancé. En attente du salon et de votre ami."},{"lobbyReady","Salon prêt. Envoyez le code à votre ami."},{"imported","Copie du jeu ({0}) importée et vérifiée."},{"verified","OK : copie du jeu ({0}) vérifiée (1291 fichiers)."},{"importDialog","Choisissez le dossier du jeu installé. Il est seulement lu ; les fichiers du jeu vérifiés sont copiés."},{"gameEnglish","Anglais"},{"gameRussian","Russe"}}},
        {"es",new Dictionary<string,string>{{"title","Friend or Foe — cooperativo en línea (versión de prueba privada)"},{"warning","VERSIÓN DE PRUEBA PRIVADA • Campaña: elige Nueva partida y la primera ranura de guardado. El anfitrión controla los menús y la base.\nGuardar/continuar y el juego en línea por Steam aún no están verificados.\nCrear / Unirse inicia Steam Spacewar con tu cuenta. No se envían invitaciones."},{"uiLanguage","Idioma del lanzador"},{"gameLanguage","Idioma del juego"},{"files","Archivos del juego"},{"steam","Componente de Steam"},{"display","Pantalla"},{"lobby","Código de sala"},{"import","Importar carpeta instalada"},{"verify","Verificar copia"},{"choose","Elegir manualmente..."},{"open","Jugar"},{"copy","Copiar código"},{"host","Crear"},{"join","Unirse"},{"stop","Detener sesión"},{"hint","Anfitrión: pulsa «Jugar» y juega solo (Nueva partida o Continuar). Para invitar a un amigo, pulsa Alt+Tab, luego «Crear» y envía el código; tu amigo aparece cuando estés en el Helitransporte.\nAmigo: introduce el código y pulsa «Unirse». Tu juego se abre, empieza solo y alcanza al anfitrión; solo espera."},{"automated","Prueba de conexión automática (la activan ambos jugadores; controles programados, termina sola)"},{"together","Empezar una campaña nueva juntos (marcadlo los dos; ambos veis la introducción)"},{"togetherStarted","Compañero conectado; los dos juegos se inician. Salta los vídeos de logos. El anfitrión elige Nueva partida y una ranura vacía (las ocupadas no se pueden elegir) y luego veis la introducción juntos."},{"togetherClose","Cierra primero el juego abierto: empezar juntos abre un juego nuevo para los dos."},{"partnerWait","Esperando a tu compañero…\nPuede que aún esté viendo una cinemática. El juego sigue en cuanto te alcance."},{"steamNotRunning","No se pudo conectar con Steam. Abre Steam e inicia sesión (si ya está abierto, reinícialo) y vuelve a pulsar Crear o Unirse."},{"relayLost","Se cortó la conexión en línea. Comprueba que Steam está abierto y que tienes internet, y vuelve a pulsar Crear o Unirse."},{"statusReady","Copia disponible; se verifica antes de cada inicio"},{"statusImport","Hace falta importar"},{"modeBorderless","Ventana sin bordes (compatible con Alt+Tab)"},{"modeWindow","Ventana"},{"modeOriginal","Ajustes propios del juego (pantalla completa original)"},{"steamLooking","Buscando el componente de Steam en tus juegos de Steam instalados..."},{"steamFound","Componente de Steam encontrado (de {0}) - OK"},{"steamRemembered","Componente de Steam recordado de la última vez - OK"},{"steamNone","No se encontró un componente de Steam compatible. Instala cualquier juego gratuito reciente de Steam (por ejemplo, Counter-Strike 2) y vuelve a abrir esta ventana, o usa «Elegir manualmente...»."},{"steamSelected","Componente de Steam elegido - OK"},{"steamBad","Este componente de Steam no se puede usar: {0}"},{"gameOpen","Juego abierto ({0}). Espera en el menú principal hasta que pulses «Crear» o «Unirse»."},{"gameClosed","Juego cerrado."},{"gameStopped","El complemento detuvo el juego porque falló una comprobación de seguridad. Envía este archivo para que se pueda corregir: {0}"},{"sessionStopped","Sesión terminada. Tus partidas guardadas quedan en la carpeta profile de este paquete."},{"partnerLinkedHost","Amigo conectado. Se une a ti en el Helitransporte; si estás en una misión, termínala o vuelve allí."},{"partnerLinkedJoin","Conectado. Tu juego alcanza al anfitrión por sí solo (Nueva partida y luego el Helitransporte); espera sin pulsar nada."},{"partnerLeft","Tu amigo se fue. Sigues jugando; pulsa «Crear» otra vez para invitarlo de nuevo (vuelve contigo en el Helitransporte)."},{"steamStarted","Steam iniciado. Esperando la sala y a tu amigo."},{"lobbyReady","Sala lista. Envía el código a tu amigo."},{"imported","Copia del juego ({0}) importada y verificada."},{"verified","OK: copia del juego ({0}) verificada (1291 archivos)."},{"importDialog","Elige la carpeta del juego instalado. Solo se lee; se copian los archivos del juego verificados."},{"gameEnglish","Inglés"},{"gameRussian","Ruso"}}},
        {"pt",new Dictionary<string,string>{{"title","Friend or Foe — cooperativo online (versão de teste privada)"},{"warning","VERSÃO DE TESTE PRIVADA • Campanha: escolha Novo Jogo e o primeiro espaço de salvamento. O anfitrião controla os menus e a base.\nSalvar/continuar e o jogo online pela Steam ainda não foram verificados.\nCriar / Entrar inicia o Steam Spacewar com sua conta conectada. Nenhum convite é enviado."},{"uiLanguage","Idioma do iniciador"},{"gameLanguage","Idioma do jogo"},{"files","Arquivos do jogo"},{"steam","Componente da Steam"},{"display","Tela"},{"lobby","Código da sala"},{"import","Importar pasta instalada"},{"verify","Verificar cópia"},{"choose","Escolher manualmente..."},{"open","Jogar"},{"copy","Copiar código"},{"host","Criar"},{"join","Entrar"},{"stop","Parar sessão"},{"hint","Anfitrião: clique em “Jogar” e jogue sozinho (Novo Jogo ou Continuar). Para convidar um amigo, pressione Alt+Tab, clique em “Criar” e envie o código; seu amigo aparece quando você estiver no Aeroporta-aviões.\nAmigo: digite o código e clique em “Entrar”. Seu jogo abre, começa sozinho e alcança o anfitrião; é só esperar."},{"automated","Teste de conexão automático (os dois jogadores ativam; controles roteirizados, termina sozinho)"},{"together","Começar uma nova campanha juntos (os dois marcam; ambos veem a abertura)"},{"togetherStarted","Parceiro conectado; os dois jogos estão iniciando. Pule os vídeos de logotipo. O anfitrião escolhe Novo Jogo e um espaço vazio (os ocupados não podem ser escolhidos) e então vocês dois assistem à abertura."},{"togetherClose","Feche primeiro o jogo aberto: começar juntos abre um jogo novo para os dois."},{"partnerWait","Aguardando seu parceiro…\nEle pode ainda estar assistindo a uma cena. O jogo continua assim que ele alcançar você."},{"steamNotRunning","Não foi possível conectar ao Steam. Abra o Steam e entre na sua conta (se já estiver aberto, reinicie-o) e clique de novo em Criar ou Entrar."},{"relayLost","A conexão online caiu. Verifique se o Steam está aberto e se a internet funciona e clique de novo em Criar ou Entrar."},{"statusReady","Cópia disponível; verificada antes de cada início"},{"statusImport","Importação necessária"},{"modeBorderless","Janela sem bordas (bom para Alt+Tab)"},{"modeWindow","Janela"},{"modeOriginal","Configurações do próprio jogo (tela cheia original)"},{"steamLooking","Procurando o componente da Steam nos seus jogos instalados..."},{"steamFound","Componente da Steam encontrado (de {0}) - OK"},{"steamRemembered","Componente da Steam lembrado da última vez - OK"},{"steamNone","Nenhum componente da Steam compatível encontrado. Instale qualquer jogo gratuito recente da Steam (por exemplo, Counter-Strike 2) e abra esta janela de novo, ou use \"Escolher manualmente...\"."},{"steamSelected","Componente da Steam escolhido - OK"},{"steamBad","Este componente da Steam não pode ser usado: {0}"},{"gameOpen","Jogo aberto ({0}). Ele espera no menu principal até você clicar em “Criar” ou “Entrar”."},{"gameClosed","Jogo fechado."},{"gameStopped","O complemento parou o jogo porque uma verificação de segurança falhou. Envie este arquivo para que isso seja corrigido: {0}"},{"sessionStopped","Sessão encerrada. Seus jogos salvos ficam na pasta profile deste pacote."},{"partnerLinkedHost","Amigo conectado. Ele se junta a você no Aeroporta-aviões; se estiver em uma missão, termine-a ou volte para lá."},{"partnerLinkedJoin","Conectado. Seu jogo alcança o anfitrião sozinho (Novo Jogo e depois o Aeroporta-aviões); aguarde sem apertar nada."},{"partnerLeft","Seu amigo saiu. Você continua jogando; clique em “Criar” de novo para chamá-lo de volta (ele volta para você no Aeroporta-aviões)."},{"steamStarted","Steam iniciada. Aguardando a sala e seu amigo."},{"lobbyReady","Sala pronta. Envie o código para seu amigo."},{"imported","Cópia do jogo ({0}) importada e verificada."},{"verified","OK: cópia do jogo ({0}) verificada (1291 arquivos)."},{"importDialog","Escolha a pasta do jogo instalado. Ela é apenas lida; os arquivos do jogo verificados são copiados."},{"gameEnglish","Inglês"},{"gameRussian","Russo"}}},
        {"it",new Dictionary<string,string>{{"title","Friend or Foe — cooperativa online (versione di prova privata)"},{"warning","VERSIONE DI PROVA PRIVATA • Campagna: scegli Nuova partita e il primo slot di salvataggio. L'host controlla i menu e la base.\nSalvataggio/continua e il gioco online tramite Steam non sono ancora verificati.\nOspita / Unisciti avvia Steam Spacewar con il tuo account. Non vengono inviati inviti."},{"uiLanguage","Lingua del launcher"},{"gameLanguage","Lingua del gioco"},{"files","File del gioco"},{"steam","Componente Steam"},{"display","Schermo"},{"lobby","Codice lobby"},{"import","Importa cartella installata"},{"verify","Verifica copia"},{"choose","Scegli manualmente..."},{"open","Gioca"},{"copy","Copia codice"},{"host","Ospita"},{"join","Unisciti"},{"stop","Ferma sessione"},{"hint","Host: premi «Gioca» e gioca da solo (Nuova partita o Continua). Per invitare un amico premi Alt+Tab, poi «Ospita» e invia il codice; l'amico arriva quando sei nell'Helicarrier.\nAmico: inserisci il codice e premi «Unisciti». Il gioco si apre, parte da solo e raggiunge l'host; aspetta e basta."},{"automated","Test di connessione automatico (lo attivano entrambi i giocatori; comandi scriptati, termina da solo)"},{"together","Inizia insieme una nuova campagna (spuntatelo entrambi; vedete entrambi l'introduzione)"},{"togetherStarted","Partner collegato; i due giochi si avviano. Salta i video dei loghi. L'host sceglie Nuova partita e uno slot vuoto (quelli occupati non si possono scegliere), poi guardate insieme l'introduzione."},{"togetherClose","Chiudi prima il gioco aperto: iniziare insieme apre un nuovo gioco per entrambi."},{"partnerWait","In attesa del tuo compagno…\nPotrebbe stare ancora guardando un filmato. Il gioco riprende appena ti raggiunge."},{"steamNotRunning","Impossibile raggiungere Steam. Apri Steam ed effettua l'accesso (se è già aperto, riavvialo), poi premi di nuovo Ospita o Unisciti."},{"relayLost","La connessione online si è interrotta. Controlla che Steam sia aperto e che internet funzioni, poi premi di nuovo Ospita o Unisciti."},{"statusReady","Copia disponibile; verificata prima di ogni avvio"},{"statusImport","Importazione necessaria"},{"modeBorderless","Finestra senza bordi (adatta ad Alt+Tab)"},{"modeWindow","Finestra"},{"modeOriginal","Impostazioni del gioco (schermo intero originale)"},{"steamLooking","Cerco il componente Steam nei tuoi giochi Steam installati..."},{"steamFound","Componente Steam trovato (da {0}) - OK"},{"steamRemembered","Componente Steam ricordato dall'ultima volta - OK"},{"steamNone","Nessun componente Steam compatibile trovato. Installa un qualsiasi gioco Steam gratuito recente (per esempio Counter-Strike 2) e riapri questa finestra, oppure usa «Scegli manualmente...»."},{"steamSelected","Componente Steam scelto - OK"},{"steamBad","Questo componente Steam non può essere usato: {0}"},{"gameOpen","Gioco aperto ({0}). Attende nel menu principale finché non premi «Ospita» o «Unisciti»."},{"gameClosed","Gioco chiuso."},{"gameStopped","Il componente ha fermato il gioco perché un controllo di sicurezza non è riuscito. Invia questo file per farlo correggere: {0}"},{"sessionStopped","Sessione terminata. I tuoi salvataggi restano nella cartella profile di questo pacchetto."},{"partnerLinkedHost","Amico connesso. Ti raggiunge nell'Helicarrier; se sei in missione, finiscila o torna lì."},{"partnerLinkedJoin","Connesso. Il gioco raggiunge l'host da solo (Nuova partita, poi l'Helicarrier); aspetta senza premere nulla."},{"partnerLeft","Il tuo amico è uscito. Continui a giocare; premi di nuovo «Ospita» per richiamarlo (ti raggiunge di nuovo nell'Helicarrier)."},{"steamStarted","Steam avviato. In attesa della lobby e del tuo amico."},{"lobbyReady","Lobby pronta. Invia il codice al tuo amico."},{"imported","Copia del gioco ({0}) importata e verificata."},{"verified","OK: copia del gioco ({0}) verificata (1291 file)."},{"importDialog","Scegli la cartella del gioco installato. Viene solo letta; vengono copiati i file del gioco verificati."},{"gameEnglish","Inglese"},{"gameRussian","Russo"}}},
        {"pl",new Dictionary<string,string>{{"title","Friend or Foe — kooperacja online (prywatna wersja testowa)"},{"warning","PRYWATNA WERSJA TESTOWA • Kampania: wybierz Nową grę i pierwszy slot zapisu. Menu i bazą steruje host.\nZapis/kontynuacja i gra przez Steam nie są jeszcze sprawdzone.\nHostuj / Dołącz uruchamia Steam Spacewar na twoim zalogowanym koncie. Zaproszenia nie są wysyłane."},{"uiLanguage","Język launchera"},{"gameLanguage","Język gry"},{"files","Pliki gry"},{"steam","Komponent Steam"},{"display","Ekran"},{"lobby","Kod lobby"},{"import","Importuj zainstalowany folder"},{"verify","Sprawdź kopię"},{"choose","Wybierz ręcznie..."},{"open","Graj"},{"copy","Kopiuj kod"},{"host","Hostuj"},{"join","Dołącz"},{"stop","Zatrzymaj sesję"},{"hint","Host: kliknij „Graj” i graj sam (Nowa gra lub Kontynuuj). Aby zaprosić znajomego, naciśnij Alt+Tab, kliknij „Hostuj” i wyślij kod; znajomy pojawi się, gdy będziesz w Helicarrierze.\nZnajomy: wpisz kod i kliknij „Dołącz”. Gra otworzy się, uruchomi sama i dogoni hosta; po prostu poczekaj."},{"automated","Automatyczny test połączenia (włączają obaj gracze; sterowanie skryptowe, kończy się samo)"},{"together","Zacznijcie razem nową kampanię (zaznaczcie oboje; oboje zobaczycie początek)"},{"togetherStarted","Partner połączony; obie gry startują. Pomińcie filmy z logo. Host wybiera Nową grę i pusty slot (zajętych nie da się wybrać), potem oboje oglądacie początek."},{"togetherClose","Najpierw zamknij otwartą grę: wspólny start otwiera nową grę dla was obojga."},{"partnerWait","Czekanie na partnera…\nMoże jeszcze oglądać przerywnik. Gra będzie kontynuowana, gdy tylko cię dogoni."},{"steamNotRunning","Nie udało się połączyć ze Steam. Otwórz Steam i zaloguj się (jeśli już działa, uruchom go ponownie), a potem znów kliknij Hostuj lub Dołącz."},{"relayLost","Połączenie online zostało przerwane. Sprawdź, czy Steam działa i masz internet, a potem znów kliknij Hostuj lub Dołącz."},{"statusReady","Kopia dostępna; sprawdzana przed każdym uruchomieniem"},{"statusImport","Wymagany import"},{"modeBorderless","Okno bez ramki (przyjazne dla Alt+Tab)"},{"modeWindow","Okno"},{"modeOriginal","Ustawienia samej gry (oryginalny pełny ekran)"},{"steamLooking","Szukam komponentu Steam w zainstalowanych grach Steam..."},{"steamFound","Znaleziono komponent Steam (z {0}) - OK"},{"steamRemembered","Komponent Steam zapamiętany z poprzedniego razu - OK"},{"steamNone","Nie znaleziono zgodnego komponentu Steam. Zainstaluj dowolną nowszą darmową grę ze Steam (np. Counter-Strike 2), a potem otwórz to okno ponownie lub użyj „Wybierz ręcznie...”."},{"steamSelected","Wybrano komponent Steam - OK"},{"steamBad","Tego komponentu Steam nie można użyć: {0}"},{"gameOpen","Gra otwarta ({0}). Czeka w menu głównym, aż klikniesz „Hostuj” lub „Dołącz”."},{"gameClosed","Gra zamknięta."},{"gameStopped","Dodatek zatrzymał grę, bo nie powiodła się kontrola bezpieczeństwa. Wyślij ten plik, aby można było to naprawić: {0}"},{"sessionStopped","Sesja zakończona. Twoje zapisy zostają w folderze profile tego pakietu."},{"partnerLinkedHost","Znajomy połączony. Dołączy do ciebie w Helicarrierze; jeśli jesteś na misji, ukończ ją lub tam wróć."},{"partnerLinkedJoin","Połączono. Gra sama dogania hosta (Nowa gra, potem Helicarrier); poczekaj, niczego nie naciskając."},{"partnerLeft","Znajomy wyszedł. Grasz dalej; kliknij ponownie „Hostuj”, aby zaprosić go z powrotem (dołączy do ciebie w Helicarrierze)."},{"steamStarted","Steam uruchomiony. Czekam na lobby i twojego kolegę."},{"lobbyReady","Lobby gotowe. Wyślij kod koledze."},{"imported","Kopia gry ({0}) zaimportowana i sprawdzona."},{"verified","OK: kopia gry ({0}) sprawdzona (1291 plików)."},{"importDialog","Wybierz folder zainstalowanej gry. Jest tylko odczytywany; kopiowane są sprawdzone pliki gry."},{"gameEnglish","Angielski"},{"gameRussian","Rosyjski"}}},
        {"zh",new Dictionary<string,string>{{"title","Friend or Foe — 在线合作（私人测试版）"},{"warning","私人测试版 • 战役：请选择“新游戏”和第一个存档位。菜单和基地由主机控制。\n存档/继续以及通过 Steam 的联网游戏尚未验证。\n“创建”/“加入”会用你已登录的账号启动 Steam Spacewar。不会发送任何邀请。"},{"uiLanguage","启动器语言"},{"gameLanguage","游戏语言"},{"files","游戏文件"},{"steam","Steam 组件"},{"display","显示"},{"lobby","大厅代码"},{"import","导入已安装的文件夹"},{"verify","校验副本"},{"choose","手动选择..."},{"open","开始游戏"},{"copy","复制代码"},{"host","创建"},{"join","加入"},{"stop","停止会话"},{"hint","主机：点击“开始游戏”，先独自游玩（新游戏或继续）。想邀请朋友时，按 Alt+Tab，点击“创建”并发送代码；当你在天空母舰中时，朋友就会加入。\n朋友：输入代码后点击“加入”。你的游戏会自动打开、自行开始并追上主机进度；请耐心等待。"},{"automated","自动连接测试（双方都要勾选；脚本控制，自动结束）"},{"together","一起开始新战役（双方都勾选；你们都能看到开场）"},{"togetherStarted","伙伴已连接，两边游戏正在启动。请跳过标志视频。主机选择“新游戏”和一个空存档位（已用的存档位无法选择），然后你们一起观看开场。"},{"togetherClose","请先关闭已打开的游戏：一起开始会为你们双方各打开一个新游戏。"},{"partnerWait","正在等待你的伙伴……\n对方可能还在观看过场动画。对方赶上后游戏会立即继续。"},{"steamNotRunning","无法连接 Steam。请打开 Steam 并登录（如果已经打开，请重启 Steam），然后再次点击“创建”或“加入”。"},{"relayLost","联机连接已中断。请确认 Steam 已打开且网络正常，然后再次点击“创建”或“加入”。"},{"statusReady","已有副本；每次启动前都会校验"},{"statusImport","需要导入"},{"modeBorderless","无边框窗口（适合 Alt+Tab）"},{"modeWindow","窗口"},{"modeOriginal","游戏自身设置（原始全屏）"},{"steamLooking","正在已安装的 Steam 游戏中查找 Steam 组件..."},{"steamFound","已找到 Steam 组件（来自 {0}）- OK"},{"steamRemembered","已记住上次的 Steam 组件 - OK"},{"steamNone","未找到兼容的 Steam 组件。请安装任意一款较新的免费 Steam 游戏（例如 Counter-Strike 2），然后重新打开此窗口，或使用“手动选择...”。"},{"steamSelected","已选择 Steam 组件 - OK"},{"steamBad","无法使用此 Steam 组件：{0}"},{"gameOpen","游戏已打开（{0}）。它会停在主菜单，直到你点击“创建”或“加入”。"},{"gameClosed","游戏已关闭。"},{"gameStopped","由于安全检查未通过，附加组件已停止游戏。请发送此文件以便修复：{0}"},{"sessionStopped","会话已结束。你的存档保留在此程序包的 profile 文件夹中。"},{"partnerLinkedHost","朋友已连接。他会在天空母舰中加入你；如果你正在执行任务，请完成任务或返回那里。"},{"partnerLinkedJoin","已连接。你的游戏会自动追上主机（新游戏，然后进入天空母舰）；请耐心等待，不要按任何键。"},{"partnerLeft","朋友已离开。你可以继续游戏；再次点击“创建”即可重新邀请他（他会在天空母舰中重新加入你）。"},{"steamStarted","Steam 已启动。正在等待大厅和你的朋友。"},{"lobbyReady","大厅已就绪。把代码发给你的朋友。"},{"imported","游戏副本（{0}）已导入并校验。"},{"verified","OK：游戏副本（{0}）已校验（1291 个文件）。"},{"importDialog","请选择已安装的游戏文件夹。只会读取；会复制经过校验的游戏文件。"},{"gameEnglish","英语"},{"gameRussian","俄语"}}}
    };
    public static string Get(string key){Dictionary<string,string> table;string value;if(Texts.TryGetValue(Current,out table)&&table.TryGetValue(key,out value))return value;return Texts["en"][key];}
    public static string Format(string key,params object[] values){return string.Format(CultureInfo.InvariantCulture,Get(key),values);}
    public static bool Has(string code){return Texts.ContainsKey(code??"");}
    public static IEnumerable<string> Keys{get{return Texts["en"].Keys;}}
    public static string Raw(string code,string key){return Texts[code][key];}
    public static string SettingsFile{get{return Path.Combine(Files.Root,"ui.json");}}
    public static string Saved(string file){try{var data=new JavaScriptSerializer().Deserialize<Dictionary<string,object>>(File.ReadAllText(file));object code;if(data!=null&&data.TryGetValue("language",out code)&&Has(code as string))return (string)code;}catch(Exception){}return null;}
    public static string Default(string file){string saved=Saved(file);if(saved!=null)return saved;string windows=CultureInfo.CurrentUICulture.TwoLetterISOLanguageName;return Has(windows)?windows:"en";}
    public static void Save(string file,string code){string temporary=file+".tmp";File.WriteAllText(temporary,new JavaScriptSerializer().Serialize(new Dictionary<string,object>{{"language",code}}),new UTF8Encoding(false));if(File.Exists(file))File.Replace(temporary,file,null);else File.Move(temporary,file);}
}
// A dropdown entry whose stored value stays fixed while its shown text follows the launcher language.
sealed class LanguageItem{public readonly string Code,Name;public LanguageItem(string code,string name){Code=code;Name=name;}public override string ToString(){return Name;}}
sealed class Choice{public readonly string Id,Key;public Choice(string id,string key){Id=id;Key=key;}public override string ToString(){return Key==null?Id:UiText.Get(Key);}}
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
        var found=Files.Scan(root);Files.CheckExtras(found.Keys,Expected,p=>Files.OwnProxyFile(p,found));
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
        // Each file is a hard link to the player's install when both are on one NTFS drive (no second copy on disk;
        // the probe refuses writes to linked files), else a plain copy.
        int linked=0,copied=0;
        foreach(string path in Expected.OrderBy(p=>p,StringComparer.Ordinal)) {
            string target=Path.Combine(staging,path.Replace('/','\\'));Directory.CreateDirectory(Path.GetDirectoryName(target));Files.NoReparse(Path.GetDirectoryName(target));Files.NoReparse(found[path]);
            if(HardLink.Create(target,found[path])){linked++;continue;}
            using(var input=new FileStream(found[path],FileMode.Open,FileAccess.Read,FileShare.Read))using(var output=new FileStream(target,FileMode.CreateNew,FileAccess.Write,FileShare.None))input.CopyTo(output);copied++;
        }
        if(progress!=null)progress(copied==0?"Game files linked to your installed game: no extra disk space used.":linked==0?"Game files copied (your game is on another drive, so they could not be linked).":"Game files: "+linked+" linked, "+copied+" copied.");
        Report after=Verify(staging,progress);if(after.language!=language)throw new Rejected("Copied language mismatch");
        Directory.CreateDirectory(testRoot);Files.Child(destination,testRoot,false);Directory.Move(staging,destination);return after;
    }
}
// Look: dark navy window, red/blue accents and spider webs (painted, no image files).
static class Theme {
    public static readonly System.Drawing.Color Back=System.Drawing.Color.FromArgb(13,19,38),Panel=System.Drawing.Color.FromArgb(24,32,60),Field=System.Drawing.Color.FromArgb(18,25,48),
        Red=System.Drawing.Color.FromArgb(206,32,48),Blue=System.Drawing.Color.FromArgb(36,87,214),Text=System.Drawing.Color.FromArgb(234,238,250),Muted=System.Drawing.Color.FromArgb(160,172,206);
    static int Clamp(int v){return Math.Max(0,Math.Min(255,v));}
    public static System.Drawing.Color Shade(System.Drawing.Color c,int by){return System.Drawing.Color.FromArgb(Clamp(c.R+by),Clamp(c.G+by),Clamp(c.B+by));}
    public static void Button(Button b,System.Drawing.Color back){b.FlatStyle=FlatStyle.Flat;b.FlatAppearance.BorderSize=0;b.BackColor=back;b.ForeColor=Text;b.UseVisualStyleBackColor=false;
        b.FlatAppearance.MouseOverBackColor=Shade(back,26);b.FlatAppearance.MouseDownBackColor=Shade(back,-24);b.Padding=new Padding(10,3,10,3);b.Cursor=Cursors.Hand;}
    public static void Outline(Button b){Button(b,Panel);b.FlatAppearance.BorderSize=1;b.FlatAppearance.BorderColor=Blue;}
    // A corner web: spokes from the centre and sagging threads between them.
    public static void Web(System.Drawing.Graphics g,System.Drawing.PointF centre,float radius,float startDeg,float sweepDeg,int spokes,int rings,System.Drawing.Color color){
        using(var pen=new System.Drawing.Pen(color,1.2f)){
            var angles=Enumerable.Range(0,spokes).Select(i=>(startDeg+sweepDeg*i/(spokes-1))*Math.PI/180).ToArray();
            foreach(var a in angles)g.DrawLine(pen,centre,new System.Drawing.PointF(centre.X+(float)Math.Cos(a)*radius,centre.Y+(float)Math.Sin(a)*radius));
            for(int r=1;r<=rings;r++){float d=radius*r/(rings+0.6f);
                for(int i=0;i+1<angles.Length;i++){var p1=new System.Drawing.PointF(centre.X+(float)Math.Cos(angles[i])*d,centre.Y+(float)Math.Sin(angles[i])*d);
                    var p2=new System.Drawing.PointF(centre.X+(float)Math.Cos(angles[i+1])*d,centre.Y+(float)Math.Sin(angles[i+1])*d);
                    double mid=(angles[i]+angles[i+1])/2;float sag=d*0.88f;var c=new System.Drawing.PointF(centre.X+(float)Math.Cos(mid)*sag,centre.Y+(float)Math.Sin(mid)*sag);
                    g.DrawBezier(pen,p1,c,c,p2);}}}
    }
}
sealed class WebBanner : Panel {
    public string Title="FRIEND OR FOE",Subtitle="";
    public WebBanner(){DoubleBuffered=true;ResizeRedraw=true;Height=104;Dock=DockStyle.Top;}
    protected override void OnPaint(PaintEventArgs e){var g=e.Graphics;g.SmoothingMode=System.Drawing.Drawing2D.SmoothingMode.AntiAlias;var r=ClientRectangle;if(r.Width<2||r.Height<2)return;
        using(var fill=new System.Drawing.Drawing2D.LinearGradientBrush(r,System.Drawing.Color.FromArgb(150,16,34),System.Drawing.Color.FromArgb(16,34,96),0f))g.FillRectangle(fill,r);
        Theme.Web(g,new System.Drawing.PointF(r.Right,r.Top),340,90,90,8,7,System.Drawing.Color.FromArgb(105,255,255,255));
        Theme.Web(g,new System.Drawing.PointF(r.Left,r.Bottom),170,-90,90,5,4,System.Drawing.Color.FromArgb(60,255,255,255));
        using(var stripe=new System.Drawing.SolidBrush(Theme.Red))g.FillRectangle(stripe,0,r.Bottom-4,r.Width/2,4);
        using(var stripe=new System.Drawing.SolidBrush(Theme.Blue))g.FillRectangle(stripe,r.Width/2,r.Bottom-4,r.Width-r.Width/2,4);
        g.TextRenderingHint=System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
        using(var big=new System.Drawing.Font("Segoe UI",26,System.Drawing.FontStyle.Bold))using(var small=new System.Drawing.Font(Font.FontFamily,10.5f))
        using(var shadow=new System.Drawing.SolidBrush(System.Drawing.Color.FromArgb(120,0,0,0)))using(var soft=new System.Drawing.SolidBrush(System.Drawing.Color.FromArgb(225,232,255))){
            g.DrawString(Title,big,shadow,20,14);g.DrawString(Title,big,System.Drawing.Brushes.White,18,12);g.DrawString(Subtitle,small,soft,22,62);}
    }
}
static class HardLink{[DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern bool CreateHardLinkW(string link,string existing,IntPtr reserved);
    public static bool Create(string link,string existing){try{return CreateHardLinkW(link,existing,IntPtr.Zero);}catch(Exception){return false;}}}
static class DarkTitle{[DllImport("dwmapi.dll")]static extern int DwmSetWindowAttribute(IntPtr hwnd,int attribute,ref int value,int size);
    public static void Apply(IntPtr handle){try{int on=1;if(DwmSetWindowAttribute(handle,20,ref on,4)!=0)DwmSetWindowAttribute(handle,19,ref on,4);}catch(Exception){}}}
sealed class LauncherForm : Form {
    readonly WebBanner banner=new WebBanner();
    protected override void OnHandleCreated(EventArgs e){base.OnHandleCreated(e);DarkTitle.Apply(Handle);}
    protected override bool ShowWithoutActivation{get{return offline;}}
    readonly ComboBox language=new ComboBox{DropDownStyle=ComboBoxStyle.DropDownList},displayMode=new ComboBox{DropDownStyle=ComboBoxStyle.DropDownList,Width=240},resolution=new ComboBox{DropDownStyle=ComboBoxStyle.DropDownList,Width=140};
    readonly CheckBox together=new CheckBox{AutoSize=true,Margin=new Padding(12,6,3,3)};
    readonly CheckBox automated=new CheckBox{Text="Automated connection test (both players enable; scripted controls, ends automatically)",AutoSize=true};
    readonly TextBox dll=new TextBox(), lobby=new TextBox(), log=new TextBox{Multiline=true,ReadOnly=true,ScrollBars=ScrollBars.Vertical};
    readonly Label status=new Label{AutoSize=true}; readonly Button openGame=new Button{AutoSize=true},host=new Button{AutoSize=true},join=new Button{AutoSize=true},stop=new Button{AutoSize=true},import=new Button{AutoSize=true},verify=new Button{AutoSize=true},browse=new Button{AutoSize=true},copy=new Button{AutoSize=true};
    readonly Label lblWarning=new Label{AutoSize=true},lblGame=new Label{AutoSize=true},lblFiles=new Label{AutoSize=true},lblSteam=new Label{AutoSize=true},lblDisplay=new Label{AutoSize=true},lblLobby=new Label{AutoSize=true},lblHint=new Label{AutoSize=true},lblUi=new Label{AutoSize=true,Margin=new Padding(3,7,3,3)};
    readonly ComboBox uiLanguage=new ComboBox{DropDownStyle=ComboBoxStyle.DropDownList,Width=180};
    Process relay,launcher,game; OwnedJob job; StreamWriter relayLog,launcherLog; string session; int ownedLauncherPid;DateTime launcherEarliest; bool busy,stopping,gameStarted,cleanupForced,connectionPassed; volatile bool peerReady,cancelRequested; readonly object gate=new object(); readonly bool offline;public bool OfflinePassed;public string OfflineDirectory;
    public LauncherForm():this(false){}
    public LauncherForm(bool offlineMode) {
        offline=offlineMode;
        Width=960;Height=800;MinimumSize=new System.Drawing.Size(900,700);BackColor=Theme.Back;ForeColor=Theme.Text;DoubleBuffered=true;
        var grid=new TableLayoutPanel{Dock=DockStyle.Fill,ColumnCount=3,RowCount=11,Padding=new Padding(16,12,16,16),BackColor=Theme.Back};grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));Controls.Add(grid);
        var warning=lblWarning;grid.Controls.Add(warning,0,0);grid.SetColumnSpan(warning,3);
        language.Items.AddRange(new object[]{new Choice("English","gameEnglish"),new Choice("Russian","gameRussian")});language.SelectedIndex=0;
        grid.Controls.Add(lblGame,0,1);grid.Controls.Add(language,1,1);grid.Controls.Add(import,2,1);
        grid.Controls.Add(lblFiles,0,2);grid.Controls.Add(status,1,2);grid.Controls.Add(verify,2,2);
        grid.Controls.Add(lblSteam,0,3);grid.Controls.Add(dll,1,3);dll.Dock=DockStyle.Fill;grid.Controls.Add(browse,2,3);
        var displayRow=new FlowLayoutPanel{AutoSize=true,WrapContents=false};displayRow.Controls.AddRange(new Control[]{displayMode,resolution});grid.Controls.Add(lblDisplay,0,4);grid.Controls.Add(displayRow,1,4);grid.SetColumnSpan(displayRow,2);
        // The main action: one big red button on its own row.
        openGame.AutoSize=false;openGame.Height=60;openGame.Dock=DockStyle.Fill;openGame.Margin=new Padding(3,10,3,10);grid.Controls.Add(openGame,0,5);grid.SetColumnSpan(openGame,3);
        grid.Controls.Add(lblLobby,0,6);grid.Controls.Add(lobby,1,6);lobby.Dock=DockStyle.Fill;grid.Controls.Add(copy,2,6);
        var buttons=new FlowLayoutPanel{AutoSize=true};buttons.Controls.AddRange(new Control[]{host,join,stop,together});grid.Controls.Add(buttons,0,7);grid.SetColumnSpan(buttons,3);
        grid.Controls.Add(lblHint,0,8);grid.SetColumnSpan(lblHint,3);
        grid.Controls.Add(automated,0,9);grid.SetColumnSpan(automated,3);
        grid.Controls.Add(log,0,10);grid.SetColumnSpan(log,3);log.Dock=DockStyle.Fill;grid.RowStyles.Clear();for(int i=0;i<10;i++)grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));grid.RowStyles.Add(new RowStyle(SizeType.Percent,100));
        // Launcher language sits in the banner's top right.
        lblUi.ForeColor=System.Drawing.Color.White;lblUi.BackColor=System.Drawing.Color.Transparent;banner.Controls.Add(lblUi);banner.Controls.Add(uiLanguage);
        banner.Layout+=(s,e)=>{uiLanguage.Location=new System.Drawing.Point(banner.ClientSize.Width-uiLanguage.Width-14,12);lblUi.AutoSize=false;lblUi.Size=new System.Drawing.Size(lblUi.PreferredWidth,lblUi.PreferredHeight);lblUi.Location=new System.Drawing.Point(uiLanguage.Left-lblUi.Width-6,15);uiLanguage.BringToFront();};
        Controls.Add(banner);grid.BringToFront();
        Stylize(grid);
        UiText.Current=offline?"en":UiText.Default(UiText.SettingsFile);foreach(var entry in UiText.Languages)uiLanguage.Items.Add(new LanguageItem(entry[0],entry[1]));foreach(LanguageItem item in uiLanguage.Items)if(item.Code==UiText.Current)uiLanguage.SelectedItem=item;
        uiLanguage.SelectedIndexChanged+=(s,e)=>{var item=uiLanguage.SelectedItem as LanguageItem;if(item==null||item.Code==UiText.Current)return;UiText.Current=item.Code;if(!offline)try{UiText.Save(UiText.SettingsFile,item.Code);}catch(Exception){}ApplyTexts();};
        stop.Enabled=false; language.SelectedIndexChanged+=(s,e)=>RefreshStatus();
        displayMode.Items.AddRange(new object[]{new Choice(Display.Borderless,"modeBorderless"),new Choice(Display.Window,"modeWindow"),new Choice(Display.Original,"modeOriginal")});resolution.Items.AddRange(Display.Resolutions().ToArray());
        var savedDisplay=offline?new Dictionary<string,string>():Display.Load(Display.SettingsFile);string savedValue;
        string wantedMode=savedDisplay.TryGetValue("mode",out savedValue)?savedValue:Display.Borderless;foreach(Choice item in displayMode.Items)if(item.Id==wantedMode)displayMode.SelectedItem=item;
        string wanted=savedDisplay.TryGetValue("resolution",out savedValue)?savedValue:Display.Desktop;if(!resolution.Items.Contains(wanted))resolution.Items.Insert(0,wanted);resolution.SelectedItem=wanted;
        EventHandler rememberDisplay=(s,e)=>{if(offline||displayMode.SelectedItem==null||resolution.SelectedItem==null)return;try{Display.Save(Display.SettingsFile,DisplayId(),(string)resolution.SelectedItem);}catch(Exception x){Note("Could not remember the display choice: "+x.Message);}};
        displayMode.SelectedIndexChanged+=rememberDisplay;resolution.SelectedIndexChanged+=rememberDisplay;
        EventHandler originalResolution=(s,e)=>{resolution.Visible=DisplayId()!=Display.Original;};displayMode.SelectedIndexChanged+=originalResolution;originalResolution(null,null);
        ApplyTexts();
        if(!offline)Shown+=async(s,e)=>{if(dll.Text.Length>0)return;
            string saved=await Task.Run(()=>SteamDll.LoadSaved(SteamDll.SettingsFile));if(IsDisposed||dll.Text.Length>0)return;
            if(saved!=null){dll.Text=saved;Note(UiText.Get("steamRemembered"));return;}
            Note(UiText.Get("steamLooking"));
            var found=await Task.Run(()=>SteamDll.Find(null));if(IsDisposed||dll.Text.Length>0)return;
            if(found!=null){dll.Text=found.Item1;RememberSteam(found.Item1);Note(UiText.Format("steamFound",found.Item2));}
            else Note(UiText.Get("steamNone"));};
        browse.Click+=(s,e)=>{using(var d=new OpenFileDialog{Filter="Steam API (steam_api64.dll)|steam_api64.dll",CheckFileExists=true})if(d.ShowDialog()==DialogResult.OK){
            string problem=SteamDll.Problem(d.FileName);if(problem!=null){Note(UiText.Format("steamBad",problem));return;}dll.Text=d.FileName;RememberSteam(d.FileName);Note(UiText.Get("steamSelected"));}};
        copy.Click+=(s,e)=>{if(Regex.IsMatch(lobby.Text,@"^\d{1,20}$"))Clipboard.SetText(lobby.Text);};
        verify.Click+=async(s,e)=>await Work(async()=>{var m=Manifest.Load();string test=TestDirectory();var r=await Task.Run(()=>m.Verify(test,Note));Note(UiText.Format("verified",r.language));});
        import.Click+=async(s,e)=>{string lang=GameLanguage(),remembered=offline?null:ImportMemory.Folder(ImportMemory.SettingsFile,lang);
            using(var d=new FolderBrowserDialog{Description=UiText.Get("importDialog")}){if(remembered!=null)d.SelectedPath=remembered;
                if(d.ShowDialog()==DialogResult.OK){string source=d.SelectedPath;
                    if(!offline)try{ImportMemory.Save(ImportMemory.SettingsFile,lang,source);}catch(Exception x){Note("Could not remember the game folder: "+x.Message);}
                    await Work(async()=>{var m=Manifest.Load();await Task.Run(()=>m.Import(source,lang,Note));Note(UiText.Format("imported",lang));RefreshStatus();});}}};
        openGame.Click+=async(s,e)=>await OpenGame();
        host.Click+=async(s,e)=>await StartSession(true);join.Click+=async(s,e)=>await StartSession(false);stop.Click+=async(s,e)=>await StopSession();
        FormClosing+=async(s,e)=>{if(busy||relay!=null||launcher!=null){e.Cancel=true;if(!stopping){await StopSession();Note("Wait for the current verification/session cleanup to finish, then close this window.");}}};
    }
    public async Task RunOffline(bool connection) {
        Note("Own-code offline session test; no Steam or game library is loaded.");
        automated.Checked=connection;
        if(!connection){
            await StartSession(true);string hostSession=session,hostConnect=Path.Combine(hostSession,"sandbox","connect.json");
            bool hosted=File.Exists(hostConnect)&&File.ReadAllText(hostConnect)=="{\"role\":\"host\"}"&&launcher!=null&&relay!=null;
            await StopSession();if(gameMonitor!=null)await gameMonitor;
            await OpenGame();string openedSession=session;lobby.Text="222";
            DateTime captured=DateTime.UtcNow.AddSeconds(10);while(DateTime.UtcNow<captured){lock(gate)if(game!=null)break;await Task.Delay(50);}
            await StartSession(false);string joinSession=session,joinConnect=Path.Combine(joinSession,"sandbox","connect.json");
            bool joined=joinSession!=openedSession&&File.Exists(joinConnect)&&File.ReadAllText(joinConnect)=="{\"role\":\"join\"}"&&launcher!=null&&relay!=null&&!File.Exists(Path.Combine(openedSession,"sandbox","connect.json"));
            await StopSession();if(gameMonitor!=null)await gameMonitor;
            Func<string,string,string> read=(dir,name)=>File.Exists(Path.Combine(dir,name))?File.ReadAllText(Path.Combine(dir,name)):"";
            OfflinePassed=hosted&&joined&&relay==null&&launcher==null&&game==null&&job==null&&read(hostSession,"relay.log").Contains("FIXTURE_STOP")&&read(joinSession,"relay.log").Contains("FIXTURE_STOP")
                &&read(hostSession,"launcher.log").Contains("FIXTURE_GAME_EXIT 0")&&read(joinSession,"launcher.log").Contains("FIXTURE_GAME_EXIT 0");
            // The game replaced by Join only has to be gone (its owned job closes it if the handle was not captured yet).
            if(!OfflinePassed)Console.Error.WriteLine("OFFLINE_DETAIL hosted="+hosted+" joined="+joined+" relay="+(relay==null)+" launcher="+(launcher==null)+" game="+(game==null)+" job="+(job==null)+" host_relay_stop="+read(hostSession,"relay.log").Contains("FIXTURE_STOP")+" join_relay_stop="+read(joinSession,"relay.log").Contains("FIXTURE_STOP")+" host_exit="+read(hostSession,"launcher.log").Contains("FIXTURE_GAME_EXIT 0")+" opened_exit="+read(openedSession,"launcher.log").Contains("FIXTURE_GAME_EXIT 0")+" join_exit="+read(joinSession,"launcher.log").Contains("FIXTURE_GAME_EXIT 0"));
            OfflineDirectory=joinSession;return;
        }
        await StartSession(true);
        string relayText=File.Exists(Path.Combine(session,"relay.log"))?File.ReadAllText(Path.Combine(session,"relay.log")):"";
        string gameText=File.Exists(Path.Combine(session,"launcher.log"))?File.ReadAllText(Path.Combine(session,"launcher.log")):"";
        OfflinePassed=peerReady&&gameStarted&&relay==null&&launcher==null&&game==null&&job==null&&(connection?connectionPassed:relayText.Contains("FIXTURE_STOP")&&gameText.Contains("FIXTURE_GAME_EXIT 0"));OfflineDirectory=session;
    }
    string TestDirectory(){return Path.Combine(Files.Root,"test",(GameLanguage()).ToLowerInvariant());}
    void RefreshStatus(){status.Text=UiText.Get(Directory.Exists(TestDirectory())?"statusReady":"statusImport");}
    string GameLanguage(){var c=language.SelectedItem as Choice;return c==null?"English":c.Id;}
    string DisplayId(){var c=displayMode.SelectedItem as Choice;return c==null?Display.Borderless:c.Id;}
    static void Retext(ComboBox box){int index=box.SelectedIndex;var items=box.Items.Cast<object>().ToArray();box.BeginUpdate();box.Items.Clear();box.Items.AddRange(items);box.SelectedIndex=index;box.EndUpdate();}
    void Stylize(Control root){
        foreach(Control c in root.Controls){
            if(c is Button)Theme.Outline((Button)c);
            else if(c is ComboBox){var box=(ComboBox)c;box.FlatStyle=FlatStyle.Flat;box.BackColor=Theme.Field;box.ForeColor=Theme.Text;}
            else if(c is TextBox){var t=(TextBox)c;t.BorderStyle=BorderStyle.FixedSingle;t.BackColor=Theme.Field;t.ForeColor=Theme.Text;}
            else if(c is CheckBox){var k=(CheckBox)c;k.ForeColor=Theme.Text;k.FlatStyle=FlatStyle.Flat;}
            else if(c is Label)c.ForeColor=Theme.Muted;
            if(c.HasChildren&&!(c is ComboBox))Stylize(c);
        }
        uiLanguage.FlatStyle=FlatStyle.Flat;uiLanguage.BackColor=Theme.Field;uiLanguage.ForeColor=Theme.Text;
        Theme.Button(openGame,Theme.Red);Theme.Button(host,Theme.Blue);Theme.Button(join,Theme.Blue);Theme.Button(stop,Theme.Panel);
        foreach(var b in new[]{host,join,stop}){b.AutoSize=true;b.MinimumSize=new System.Drawing.Size(124,38);}
        lblWarning.ForeColor=System.Drawing.Color.FromArgb(255,196,120);lblHint.ForeColor=Theme.Text;status.ForeColor=Theme.Text;
        foreach(var b in new[]{import,verify,browse,copy})b.Dock=DockStyle.Fill;
        foreach(var l in new Control[]{lblGame,lblFiles,lblSteam,lblDisplay,lblLobby,status})l.Anchor=AnchorStyles.Left;
        lblWarning.Margin=new Padding(3,0,3,10);Shown+=(s,e)=>ActiveControl=openGame;
        log.BackColor=System.Drawing.Color.FromArgb(9,13,28);log.ForeColor=System.Drawing.Color.FromArgb(185,198,232);log.BorderStyle=BorderStyle.None;
    }
    System.Drawing.Font latinFont;
    void ApplyTexts(){
        // Chinese needs a CJK UI font, otherwise WinForms measures with a fallback font and clips text.
        if(latinFont==null)latinFont=Font;Font=UiText.Current=="zh"?new System.Drawing.Font("Microsoft YaHei UI",latinFont.Size):latinFont;
        Text=UiText.Get("title");lblWarning.Text=UiText.Get("warning");lblUi.Text=UiText.Get("uiLanguage");lblGame.Text=UiText.Get("gameLanguage");lblFiles.Text=UiText.Get("files");
        lblSteam.Text=UiText.Get("steam");lblDisplay.Text=UiText.Get("display");lblLobby.Text=UiText.Get("lobby");lblHint.Text=UiText.Get("hint");
        import.Text=UiText.Get("import");verify.Text=UiText.Get("verify");browse.Text=UiText.Get("choose");openGame.Text="▶  "+UiText.Get("open").ToUpper(UiText.Current=="tr"?new CultureInfo("tr-TR"):CultureInfo.InvariantCulture);copy.Text=UiText.Get("copy");
        host.Text=UiText.Get("host");join.Text=UiText.Get("join");stop.Text=UiText.Get("stop");automated.Text=UiText.Get("automated");together.Text=UiText.Get("together");
        openGame.Font=new System.Drawing.Font(Font.FontFamily,16f,System.Drawing.FontStyle.Bold);foreach(var b in new[]{host,join})b.Font=new System.Drawing.Font(Font.FontFamily,Font.Size+1f,System.Drawing.FontStyle.Bold);
        string title=UiText.Get("title");int dash=title.IndexOf('—');banner.Subtitle=dash>=0?title.Substring(dash+1).Trim():title;banner.Font=Font;banner.PerformLayout();banner.Invalidate();
        Retext(language);Retext(displayMode);RefreshStatus();
    }
    void RememberSteam(string path){if(offline)return;try{SteamDll.Save(SteamDll.SettingsFile,path);}catch(Exception x){Note("Could not remember the Steam component location: "+x.Message);}}
    void Note(string text){if(IsDisposed)return;if(InvokeRequired){BeginInvoke(new Action<string>(Note),text);return;}log.AppendText(text+Environment.NewLine);}
    // While a game opened with "open" waits, only Host/Join/Stop stay usable.
    void ControlsBusy(bool value){busy=value;bool idle=!value&&launcher==null&&relay==null;openGame.Enabled=import.Enabled=verify.Enabled=language.Enabled=automated.Enabled=together.Enabled=displayMode.Enabled=resolution.Enabled=idle;host.Enabled=join.Enabled=!value&&relay==null;stop.Enabled=relay!=null||launcher!=null;}
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
    // Starts the verified copy as an ordinary game; Host/Join later hands it its role through sandbox\connect.json.
    // Saves persist in the package profile, except for a joiner's catch-up game, which must never
    // overwrite that player's own campaign. Without the display button the game uses its own settings.
    async Task<bool> LaunchGame(bool persistentSaves,bool displaySettings) {
        if(busy||launcher!=null||relay!=null)return false;ControlsBusy(true);cancelRequested=false;cleanupForced=false;session=null;peerReady=false;gameStarted=false;bool failed=false;
        try {
            string test=TestDirectory();
            if(!offline){var m=Manifest.Load();test=Files.Child(test,Path.Combine(Files.Root,"test"),true);var report=await Task.Run(()=>m.Verify(test,Note));
                if(report.language!=GameLanguage())throw new Rejected("Test folder language mismatch");if(cancelRequested)throw new OperationCanceledException("Cancelled before the game opened");}
            string sessions=offline?Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"offline-sessions"):Path.Combine(Files.Root,"sessions");if(!Directory.Exists(sessions))Directory.CreateDirectory(sessions);Files.NoReparse(sessions);
            session=Path.Combine(sessions,DateTime.UtcNow.ToString("yyyyMMddTHHmmssfff",CultureInfo.InvariantCulture)+"-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(session);Files.NoReparse(session);
            job=new OwnedJob();var env=Files.GameEnvironment(false);env["COOP_PROBE_NET_MODE"]="deferred";
            if(persistentSaves&&!offline){string profile=Path.Combine(Files.Root,"profile");if(!Directory.Exists(profile))Directory.CreateDirectory(profile);Files.NoReparse(profile);env["COOP_PROBE_PROFILE"]=profile;}
            if(displaySettings)Display.Apply(env,DisplayId(),(string)resolution.SelectedItem);else Display.Apply(env,Display.Original,null);
            launcherLog=new StreamWriter(new FileStream(Path.Combine(session,"launcher.log"),FileMode.CreateNew,FileAccess.Write,FileShare.Read));
            launcher=StartOwned(offline?System.Reflection.Assembly.GetExecutingAssembly().Location:Path.Combine(Files.Root,"probe","native-launcher.exe"),offline?"--launcher-fixture":Files.NativeArguments(Path.Combine(test,"Game.exe"),Path.Combine(session,"sandbox"),false),launcherLog,GameLine,env);gameStarted=true;
            gameMonitor=MonitorGame(launcher,session);
            Note(UiText.Format("gameOpen",!displaySettings||DisplayId()==Display.Original?((Choice)displayMode.Items[2]).ToString():resolution.SelectedItem+", "+displayMode.SelectedItem));
        }catch(Exception ex){Note("REJECTED: "+ex.Message);failed=true;}
        if(failed){try{while(stopping)await Task.Delay(100);await StopSession();}catch(Exception){cleanupForced=true;}ControlsBusy(false);return false;}
        ControlsBusy(false);return true;
    }
    Task gameMonitor,relayWatcher;
    // The partner left (their game closed or the connection dropped): Steam stops, the open game keeps
    // running alone (the host continues; a joiner's game closes by itself), and Host/Join work again.
    async Task WatchRelay(Process current) {
        while(relay==current&&!current.HasExited)await Task.Delay(250);
        if(relay!=current||cancelRequested||launcher==null||launcher.HasExited)return;
        try{await StopRelay();}catch(Exception){cleanupForced=true;}
        if(!offline)TopMost=false;Note(UiText.Get("partnerLeft"));ControlsBusy(false);
    }
    async Task MonitorGame(Process current,string owner) {
        while(!cancelRequested&&!current.HasExited)await Task.Delay(250);
        // A newer session (Join restarting the game) owns cleanup from here.
        if(session!=owner)return;
        if(!offline)TopMost=false;
        // native-launcher exits nonzero when the add-on stopped the game itself (a safety check failed).
        if(!cancelRequested){int code=0;try{code=current.ExitCode;}catch(InvalidOperationException){}Note(code==0?UiText.Get("gameClosed"):UiText.Format("gameStopped",session==null?"":Path.Combine(session,"sandbox","probe.log")));}
        try{while(stopping)await Task.Delay(100);await StopSession();}catch(Exception){cleanupForced=true;Note("Owned session cleanup did not complete normally.");}
        ControlsBusy(false);
    }
    async Task OpenGame(){await LaunchGame(true,true);}
    // Host/Join for a game opened with "open": start Steam, wait for a validated partner, then tell the game its role.
    async Task LinkSession(bool hosting) {
        if(busy||relay!=null)return;Process currentGame=launcher;if(currentGame==null||currentGame.HasExited)return;ControlsBusy(true);peerReady=false;bool failed=false;
        // Visible above a borderless game while Steam connects; the game gets the foreground back once linked.
        if(!offline){TopMost=true;Activate();}
        try {
            string steam="",steamSha="";
            if(!offline){
                if(dll.Text.Length==0)throw new Rejected("No Steam component selected yet. Wait for the automatic search, or use Choose manually.");
                steam=Path.GetFullPath(dll.Text);string steamProblem=SteamDll.Problem(steam);if(steamProblem!=null)throw new Rejected("This Steam component cannot be used: "+steamProblem);steamSha=Files.HashFile(steam).Sha;
            }
            ulong code=0;if(!hosting&&(!UInt64.TryParse(lobby.Text,NumberStyles.None,CultureInfo.InvariantCulture,out code)||code==0))throw new Rejected("Enter the host's decimal lobby code");
            string relayFile=Path.Combine(session,"relay.log");if(File.Exists(relayFile))relayFile=Path.Combine(session,"relay-"+DateTime.UtcNow.ToString("HHmmssfff",CultureInfo.InvariantCulture)+".log");
            relayLog=new StreamWriter(new FileStream(relayFile,FileMode.CreateNew,FileAccess.Write,FileShare.Read));
            string args=(hosting?"host":"join")+" --steam-live "+Files.Quote(steam)+" --steam-api-sha256 "+steamSha+" --local-port 49375 --seconds 0 --ready-file "+Files.Quote(Path.Combine(session,"lobby.json"))+(hosting?"":" --lobby "+code.ToString(CultureInfo.InvariantCulture));
            relay=StartOwned(offline?System.Reflection.Assembly.GetExecutingAssembly().Location:Path.Combine(Files.Root,"steam","native","native-relay.exe"),offline?"--relay-fixture":args,relayLog,RelayLine,null);ControlsBusy(true);Note(offline?"Own relay fixture started.":UiText.Get("steamStarted"));
            Process currentRelay=relay;
            while(!peerReady&&!currentRelay.HasExited&&!currentGame.HasExited&&!cancelRequested)await Task.Delay(100);
            if(cancelRequested||currentGame.HasExited)throw new OperationCanceledException("Session cancelled");if(!peerReady)throw new Rejected("Relay exited before a validated peer connected; see private session log");
            string sandbox=Path.Combine(session,"sandbox");if(offline)Directory.CreateDirectory(sandbox);Files.NoReparse(sandbox);
            string connect=Path.Combine(sandbox,"connect.json"),temporary=connect+".tmp";
            File.WriteAllText(temporary,Files.Json.Serialize(new Dictionary<string,object>{{"role",hosting?"host":"join"}}),new UTF8Encoding(false));File.Move(temporary,connect);
            Note(UiText.Get(hosting?"partnerLinkedHost":"partnerLinkedJoin"));
            relayWatcher=WatchRelay(relay);
            if(!offline){TopMost=false;Process linkedGame;lock(gate)linkedGame=game;if(linkedGame!=null)Ownership.Focus(linkedGame);}
        }catch(Exception ex){Note("REJECTED: "+ex.Message);failed=true;}
        // The open game keeps waiting at its menu; only Steam stops, so Host/Join can be tried again.
        if(failed){if(!offline)TopMost=false;try{await StopRelay();}catch(Exception){cleanupForced=true;}}
        ControlsBusy(false);
    }
    async Task StartSession(bool hosting) {
        if(busy)return;
        // "Start together": both games start once the partner connects and the host's menu choices drive both,
        // from the main menu on, so both players see the opening. Otherwise the friend joins the host at the hub.
        if(!automated.Checked&&!together.Checked){
            // Host links the game already being played (or opens one). A joiner always gets a fresh catch-up game,
            // kept in the display mode it chose with the display button.
            bool display=false;
            if(!hosting&&launcher!=null){display=true;await StopSession();if(gameMonitor!=null)await gameMonitor;}
            if(launcher==null&&!await LaunchGame(hosting,display))return;
            await LinkSession(hosting);return;
        }
        if(launcher!=null){Note(automated.Checked?"Close the open game before running the automated connection test.":UiText.Get("togetherClose"));return;}bool runTest=automated.Checked;ControlsBusy(true);peerReady=false;gameStarted=false;cancelRequested=false;cleanupForced=false;connectionPassed=false;session=null;
        int testPid=0,testLauncherExit=-1,testGameExit=-1,testRelayExit=-1;bool testCancelled=false;
        try {
            string test=TestDirectory(),selected=GameLanguage(),steam="",steamSha="";Report report;
            if(offline)report=new Report{schema="own-offline-fixture",status="fixture",language="English",critical_digest="not-checked-in-own-fixture"};
            else {
                var m=Manifest.Load();test=Files.Child(test,Path.Combine(Files.Root,"test"),true);report=await Task.Run(()=>m.Verify(test,Note));if(cancelRequested)throw new OperationCanceledException("Session cancelled before account use");if(report.language!=selected)throw new Rejected("Test folder language mismatch");
                if(dll.Text.Length==0)throw new Rejected("No Steam component selected yet. Wait for the automatic search, or use Choose manually.");
                steam=Path.GetFullPath(dll.Text);string steamProblem=SteamDll.Problem(steam);if(steamProblem!=null)throw new Rejected("This Steam component cannot be used: "+steamProblem);steamSha=Files.HashFile(steam).Sha;
                using(var fs=File.OpenRead(steam))using(var b=new BinaryReader(fs)){if(b.ReadUInt16()!=0x5a4d)throw new Rejected("Steam DLL is not PE");fs.Position=0x3c;int offset=b.ReadInt32();fs.Position=offset;if(b.ReadUInt32()!=0x4550||b.ReadUInt16()!=0x8664)throw new Rejected("Steam DLL must be x64");}
            }
            ulong code=0;if(!hosting&&(!UInt64.TryParse(lobby.Text,NumberStyles.None,CultureInfo.InvariantCulture,out code)||code==0))throw new Rejected("Enter the host's decimal lobby code");
            string sessions=offline?Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"offline-sessions"):Path.Combine(Files.Root,"sessions");if(!Directory.Exists(sessions))Directory.CreateDirectory(sessions);Files.NoReparse(sessions);
            session=Path.Combine(sessions,DateTime.UtcNow.ToString("yyyyMMddTHHmmssfff",CultureInfo.InvariantCulture)+"-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(session);Files.NoReparse(session);Files.NewJson(Path.Combine(session,"preflight.json"),report);
            job=new OwnedJob();relayLog=new StreamWriter(new FileStream(Path.Combine(session,"relay.log"),FileMode.CreateNew,FileAccess.Write,FileShare.Read));
            string args=(hosting?"host":"join")+" --steam-live "+Files.Quote(steam)+" --steam-api-sha256 "+steamSha+" --local-port 49375 --seconds 0 --ready-file "+Files.Quote(Path.Combine(session,"lobby.json"))+(hosting?"":" --lobby "+code.ToString(CultureInfo.InvariantCulture));
            string ownExe=System.Reflection.Assembly.GetExecutingAssembly().Location;
            relay=StartOwned(offline?ownExe:Path.Combine(Files.Root,"steam","native","native-relay.exe"),offline?(runTest?"--relay-connection-fixture "+Files.Quote(session):"--relay-fixture"):args,relayLog,RelayLine,null);stop.Enabled=true;Note(offline?"Own relay fixture started.":UiText.Get("steamStarted"));
            // Capture only this process handle; a later PID is never used for relay cleanup.
            Process currentRelay=relay;
            while(!peerReady&&!currentRelay.HasExited&&!cancelRequested)await Task.Delay(100);
            if(cancelRequested)throw new OperationCanceledException("Session cancelled");if(!peerReady)throw new Rejected("Relay exited before a validated peer connected; see private session log");
            var env=Files.GameEnvironment(runTest);env["COOP_PROBE_NET_MODE"]=hosting?"host":"join";
            if(!runTest){
                Display.Apply(env,DisplayId(),(string)resolution.SelectedItem);
                // The host keeps the campaign started together (Continue it later and invite with Host); the friend's
                // game keeps nothing, so that player's own campaign is never touched.
                if(hosting&&!offline){string profile=Path.Combine(Files.Root,"profile");if(!Directory.Exists(profile))Directory.CreateDirectory(profile);Files.NoReparse(profile);env["COOP_PROBE_PROFILE"]=profile;}
            }
            launcherLog=new StreamWriter(new FileStream(Path.Combine(session,"launcher.log"),FileMode.CreateNew,FileAccess.Write,FileShare.Read));
            if(offline&&!peerReady)throw new Rejected("Offline ordering check: game before PEER_READY");
            launcher=StartOwned(offline?ownExe:Path.Combine(Files.Root,"probe","native-launcher.exe"),offline?(runTest?"--launcher-connection-fixture "+Files.Quote(session):"--launcher-fixture"):Files.NativeArguments(Path.Combine(test,"Game.exe"),Path.Combine(session,"sandbox"),runTest),launcherLog,GameLine,env);gameStarted=true;Note(runTest?"Peer ready. Automated connection test is running; scripted controls will stop after 3600 frames (five-minute limit).":UiText.Get("togetherStarted"));
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
            if(ev=="LOBBY_READY"){string code=Convert.ToString(data["lobby"],CultureInfo.InvariantCulture);BeginInvoke(new Action(()=>{lobby.Text=code;Note(UiText.Get("lobbyReady"));}));}
            if(ev=="PEER_READY")peerReady=true;
            // Plain words for players; the technical reason stays in the session's relay.log.
            if(ev=="RELAY_FAIL"){object why;string reason=data.TryGetValue("reason",out why)?Convert.ToString(why,CultureInfo.InvariantCulture):"";
                Note(UiText.Get(reason.Contains("Steam initialization")||reason.Contains("not logged in")||reason.Contains("interfaces unavailable")?"steamNotRunning":"relayLost"));}
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
    async Task StopRelay() {
        if(relay!=null){Process owned=relay;relay=null;try{if(!owned.HasExited){try{owned.StandardInput.WriteLine("STOP");owned.StandardInput.Flush();}catch(IOException){}await Task.Run(()=>owned.WaitForExit(5000));if(!owned.HasExited){cleanupForced=true;owned.Kill();await Task.Run(()=>owned.WaitForExit(3000));}}}finally{owned.Dispose();}}
        lock(gate){if(relayLog!=null){relayLog.Dispose();relayLog=null;}}
    }
    async Task StopSession() {
        cancelRequested=true;if(stopping)return;stopping=true;stop.Enabled=false;
        try {
            Process ownedGame;lock(gate){ownedGame=game;game=null;}
            if(ownedGame!=null){try{if(!ownedGame.HasExited){Ownership.RequestClose(ownedGame);await Task.Run(()=>ownedGame.WaitForExit(5000));}}catch(InvalidOperationException){}finally{ownedGame.Dispose();}}
            if(launcher!=null){Process owned=launcher;launcher=null;try{if(!owned.HasExited)await Task.Run(()=>owned.WaitForExit(2000));if(!owned.HasExited){cleanupForced=true;owned.Kill();await Task.Run(()=>owned.WaitForExit(3000));}
                // Its last output lines are still being read asynchronously; an untimed WaitForExit would hang on
                // pipe handles inherited by the game, so allow a short bounded drain before the log closes.
                if(owned.HasExited)await Task.Delay(300);}finally{owned.Dispose();}}
            await StopRelay();
            lock(gate){if(launcherLog!=null){launcherLog.Dispose();launcherLog=null;}}
            if(gameStarted)Note(UiText.Get("sessionStopped"));
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
        {string saved=UiText.Current;UiText.Current="ru";string ru=Files.GameEnvironment(false)["COOP_PROBE_WAIT_TEXT"];UiText.Current=saved;
            Assert(play["COOP_PROBE_WAIT_TEXT"]==UiText.Get("partnerWait")&&ru.Contains("напарник")&&ru.Length<512,"partner wait notice follows the launcher language");}
        Assert(play["COOP_PROBE_HUMAN_FIXTURE"]=="0"&&play["COOP_PROBE_NET_FRAMES"]=="0"&&!play.ContainsKey("COOP_PROBE_DIAGNOSTICS"),"play remains default");
        Assert(play["COOP_PROBE_CAMPAIGN"]=="1"&&play["COOP_PROBE_CAMPAIGN_FIXTURE"]=="0"&&!play.ContainsKey("COOP_PROBE_AUTOTEST")&&!play.ContainsKey("COOP_PROBE_AREA"),"play uses the retail campaign with human menus");
        Assert(test.ContainsKey("COOP_PROBE_AUTOTEST")&&!test.ContainsKey("COOP_PROBE_CAMPAIGN"),"connection check keeps the research scenario");
        Assert(test["COOP_PROBE_HUMAN_FIXTURE"]=="1"&&test["COOP_PROBE_NET_FRAMES"]=="3600"&&test["COOP_PROBE_CLEAN_EXIT"]=="1"&&test["COOP_PROBE_DIAGNOSTICS"]=="1"&&test["COOP_PROBE_CONTENT_SHA256"]==Files.Critical,"finite scripted test environment");
        Assert(Files.NativeArguments("game","sandbox",false)=="--play \"game\" \"sandbox\""&&Files.NativeArguments("game","sandbox",true)=="\"game\" \"sandbox\" 300","finite native arguments");
        Console.WriteLine("OWN_CONNECTION_SELFTEST_PASS positive=1 negative="+negative+" environment/arguments=3 game/account_used=0");return 0;
    }
    static int Selftest() {
        Assert(Files.Normalize("Art\\A.bin")=="art/a.bin","path normalization");foreach(string p in new[]{"../x","a//b","/x","a:b","a/./b","a\nx"})Reject(()=>Files.Normalize(p),"bad path");
        var expected=new HashSet<string>{"game.exe","binkw32.dll"};Files.CheckExtras(new[]{"game.exe","gamelog/output.txt","unins000.exe"},expected);Reject(()=>Files.CheckExtras(new[]{"inject.dll"},expected),"unknown DLL");
        Files.CheckExtras(new[]{"game.exe","d3d9.dll","d3d9_chain.dll"},expected,p=>true);Reject(()=>Files.CheckExtras(new[]{"d3d9.dll"},expected,p=>false),"foreign d3d9");Reject(()=>Files.CheckExtras(new[]{"dinput8.dll"},expected,p=>true),"own-proxy pass is only for d3d9 names");
        Assert(Files.HasProxyMarker(Encoding.Unicode.GetBytes("xxCOOP_PROXY_PROBEyy")),"older own proxy build recognized");Assert(!Files.HasProxyMarker(Encoding.ASCII.GetBytes("COOP_PROXY_PROBE")),"foreign d3d9 without the marker rejected");Reject(()=>Files.CheckExtras(new[]{"save.dat"},expected),"unknown package file");
        var sample=new Dictionary<string,Record>{{"a",new Record(2,new string('0',64))}};Assert(Files.Digest(sample,"critical")=="14f29cb9bfc75e703fd86db6a16b7e9cab690e41d0493c1ef9a6a9a8f278ae86","canonical manifest digest");
        string mock=Path.Combine(Files.Root,"steam","api_abi_mock.dll");
        if(File.Exists(mock)){Assert(SteamDll.ExportProblem(File.ReadAllBytes(mock))==null,"own ABI mock exports every relay function");Assert(SteamDll.Problem(mock)!=null,"unsigned own mock is rejected as a Steam component");}
        string settings=Path.Combine(Path.GetTempPath(),"fof-steam-component-"+Guid.NewGuid().ToString("N")+".json");
        try{SteamDll.Save(settings,@"C:\missing\steam_api64.dll");Assert(File.Exists(settings)&&SteamDll.LoadSaved(settings)==null,"a remembered path that no longer passes is ignored");
            File.WriteAllText(settings,"{not json");Assert(SteamDll.LoadSaved(settings)==null,"a damaged settings file is ignored");}finally{File.Delete(settings);}
        Assert(Display.Valid("1920x1200")&&Display.Valid("640x480")&&!Display.Valid("639x480")&&!Display.Valid("1920x1200x")&&!Display.Valid(null),"display resolution validation");
        string displayFile=Path.Combine(Path.GetTempPath(),"fof-display-"+Guid.NewGuid().ToString("N")+".json");
        try{Display.Save(displayFile,Display.Window,"1280x720");var loaded=Display.Load(displayFile);Assert(loaded["mode"]==Display.Window&&loaded["resolution"]=="1280x720","display settings round trip");
            File.WriteAllText(displayFile,"{\"mode\":\"bad\",\"resolution\":\"1x1\"}");Assert(Display.Load(displayFile).Count==0,"invalid display settings ignored");}finally{File.Delete(displayFile);}
        var originalEnv=new Dictionary<string,string>{{"COOP_PROBE_RESOLUTION","1x1"}};Display.Apply(originalEnv,Display.Original,null);Assert(originalEnv["COOP_PROBE_DISPLAY_MODE"]=="original"&&!originalEnv.ContainsKey("COOP_PROBE_RESOLUTION"),"original display environment");
        var displayEnv=new Dictionary<string,string>();Display.Apply(displayEnv,Display.Borderless,"2560x1080");Assert(displayEnv["COOP_PROBE_DISPLAY_MODE"]=="borderless"&&displayEnv["COOP_PROBE_RESOLUTION"]=="2560x1080","display environment");
        string importFile=Path.Combine(Path.GetTempPath(),"fof-import-"+Guid.NewGuid().ToString("N")+".json");
        try{ImportMemory.Save(importFile,"English",Path.GetTempPath());ImportMemory.Save(importFile,"Russian",@"C:\missing-folder-fixture");
            Assert(ImportMemory.Folder(importFile,"English")==Path.GetTempPath()&&ImportMemory.Folder(importFile,"Russian")==null,"import folder remembered per language; missing folder ignored");
            File.WriteAllText(importFile,"{broken");Assert(ImportMemory.Folder(importFile,"English")==null,"damaged import memory ignored");}finally{File.Delete(importFile);}
        foreach(var entry in UiText.Languages)foreach(string key in UiText.Keys){string english=UiText.Raw("en",key),local=UiText.Raw(entry[0],key);
            Assert(local.Length>0&&Regex.Matches(english,@"\{\d\}").Count==Regex.Matches(local,@"\{\d\}").Count,"translation "+entry[0]+"/"+key);}
        Assert(UiText.Has("tr")&&UiText.Has("ru")&&!UiText.Has("xx"),"launcher languages include Turkish and Russian");
        Assert(SteamDll.ExportProblem(new byte[64])!=null&&SteamDll.ExportProblem(File.ReadAllBytes(System.Reflection.Assembly.GetExecutingAssembly().Location))!=null,"non-Steam binaries are rejected");
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
            if(args.Length==1&&args[0]=="--list-display"){Console.WriteLine("DISPLAY desktop="+Display.Desktop+" modes="+string.Join(",",Display.Resolutions()));return 0;}
            if(args.Length==1&&args[0]=="--find-steam-component"){
                int accepted=0;foreach(string library in SteamDll.Libraries()){string common=Path.Combine(library,"steamapps","common");if(!Directory.Exists(common))continue;
                    foreach(string file in Directory.GetFiles(common,"steam_api64.dll",SearchOption.AllDirectories)){string problem=SteamDll.Problem(file);if(problem==null)accepted++;Console.WriteLine((problem==null?"OK   ":"SKIP ")+Files.HashFile(file).Sha.Substring(0,12)+" "+file+(problem==null?"":" ("+problem+")"));}}
                var pick=SteamDll.Find(null);bool remembered=false;
                if(pick!=null){string settings=Path.Combine(Path.GetTempPath(),"fof-steam-component-"+Guid.NewGuid().ToString("N")+".json");try{SteamDll.Save(settings,pick.Item1);remembered=SteamDll.LoadSaved(settings)==pick.Item1;}finally{File.Delete(settings);}}
                Console.WriteLine("FIND_STEAM_COMPONENT accepted="+accepted+" pick="+(pick==null?"none":pick.Item2)+" remembered_roundtrip="+(remembered?1:0));return pick!=null&&remembered?0:1;
            }
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
            if(args.Length==3&&args[0]=="--import"&&(args[2]=="English"||args[2]=="Russian")){var report=Manifest.Load().Import(Path.GetFullPath(args[1]),args[2],Console.WriteLine);Console.WriteLine(Files.Json.Serialize(report));return 0;}
            if(args.Length==2&&args[0]=="--verify"){string folder=Files.Child(args[1],Path.Combine(Files.Root,"test"),true);var report=Manifest.Load().Verify(folder,null);Console.WriteLine(Files.Json.Serialize(report));return 0;}
            if(args.Length!=0)throw new Rejected("Usage: fof-private-launcher.exe [--selftest | --connection-selftest | --offline-session-selftest | --offline-connection-selftest | --verify <test folder> | --import <installed folder> English|Russian]");
            Files.NoReparse(Files.Root);Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);Application.Run(new LauncherForm());return 0;
        }catch(Exception ex){Console.Error.WriteLine("REJECTED: "+ex.Message);return 1;}
    }
}
}
