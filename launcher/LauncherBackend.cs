using System;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace FofPrivate {
public sealed class BackendState {
    public int FriendJoinedSequence;
    public bool HostEnabled,JoinEnabled,StopEnabled,SetupEnabled,GameReady,SteamReady,Active;
    public string Lobby,Activity,Log,SteamPath,Resolution;
    public int GameLanguage,DisplayMode;
    public string[] Resolutions;
}
// Keep launch verification, process ownership, transport, import and profile semantics in one place.
// The WPF shell never starts a native game or relay directly.
sealed partial class LauncherForm {
    internal void CheckFriendJoinedSignal(){
        bool oldHosting=hostingSession,oldPeer=peerReady,oldCancel=cancelRequested;int oldSequence=friendJoinedSequence;
        try{
            hostingSession=true;peerReady=false;cancelRequested=false;
            RelayLine("{\"event\":\"PEER_READY\"}");RelayLine("{\"event\":\"PEER_READY\"}");
            if(friendJoinedSequence!=oldSequence+1)throw new Exception("Host joined notice missing or duplicated");
            hostingSession=false;peerReady=false;RelayLine("{\"event\":\"PEER_READY\"}");
            if(friendJoinedSequence!=oldSequence+1)throw new Exception("Joiner received host notice");
            hostingSession=true;peerReady=false;cancelRequested=true;RelayLine("{\"event\":\"PEER_READY\"}");
            if(friendJoinedSequence!=oldSequence+1)throw new Exception("Cancelled session received notice");
            cancelRequested=false;peerReady=false;RelayLine("{\"event\":\"PEER_READY\"}");
            if(friendJoinedSequence!=oldSequence+2)throw new Exception("Reconnect notice missing");
        }finally{hostingSession=oldHosting;peerReady=oldPeer;cancelRequested=oldCancel;friendJoinedSequence=oldSequence;}
    }
    public BackendState Snapshot(){return new BackendState{
        FriendJoinedSequence=System.Threading.Volatile.Read(ref friendJoinedSequence),
        HostEnabled=host.Enabled,JoinEnabled=join.Enabled,StopEnabled=stop.Enabled,SetupEnabled=import.Enabled,
        GameReady=Directory.Exists(TestDirectory()),SteamReady=dll.Text.Length>0,Active=busy||relay!=null||launcher!=null,
        Lobby=lobby.Text,Activity=activity==null?"":activity.Text,Log=log.Text,SteamPath=dll.Text,
        Resolution=resolution.SelectedItem as string,GameLanguage=language.SelectedIndex,DisplayMode=displayMode.SelectedIndex,
        Resolutions=resolution.Items.Cast<object>().Select(x=>x.ToString()).ToArray()};}
    public async Task UiHost(){if(!offline)await StartSession(true);}
    public async Task UiJoin(string code){if(offline)return;lobby.Text=code;await StartSession(false);}
    public async Task UiStop(){await StopSession();}
    public void UiImport(){if(!offline)InvokeOnClick(import,EventArgs.Empty);}
    public async Task UiVerify(){if(offline)return;await Work(async()=>{var m=Manifest.Load();string test=TestDirectory();var result=await Task.Run(()=>m.Verify(test,Note));Note(UiText.Format("verified",result.language));RefreshStatus();});}
    public void UiBrowseSteam(){if(!offline)InvokeOnClick(browse,EventArgs.Empty);}
    public void UiSetLanguage(string code){foreach(LanguageItem item in uiLanguage.Items)if(item.Code==code){uiLanguage.SelectedItem=item;break;}}
    public void UiSetGameLanguage(int index){if(index>=0&&index<language.Items.Count)language.SelectedIndex=index;}
    public void UiSetDisplay(int index){if(index>=0&&index<displayMode.Items.Count)displayMode.SelectedIndex=index;}
    public void UiSetResolution(string value){if(resolution.Items.Contains(value))resolution.SelectedItem=value;}
    public async Task<bool> UiClose(){cancelRequested=true;await StopSession();return !busy&&relay==null&&launcher==null;}
    public async void StartBackgroundDiscovery(){
        if(offline)return;
        // Create the hidden backend's marshaling handle without showing its obsolete form.
        if(Handle==IntPtr.Zero)return;
        try{
            string saved=await Task.Run(()=>SteamDll.LoadSaved(SteamDll.SettingsFile));if(IsDisposed)return;
            if(saved!=null){dll.Text=saved;Note(UiText.Get("steamRemembered"));return;}
            Note(UiText.Get("steamLooking"));var found=await Task.Run(()=>SteamDll.Find(null));if(IsDisposed)return;
            if(found!=null){dll.Text=found.Item1;RememberSteam(found.Item1);Note(UiText.Format("steamFound",found.Item2));}
            else Note(UiText.Get("steamNone"));
        }catch(Exception ex){if(!IsDisposed)Note("REJECTED: "+ex.Message);}
    }
}
}
