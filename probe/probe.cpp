#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <d3d9.h>
#include <Xinput.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <algorithm>
#include <cmath>
#include "input_wire.hpp"
#include "rng_calls.inc"
#include "device_calls.inc"
#include "progress_sample.hpp"
#include "mode_sample.hpp"
#include "input_key_names.hpp"
#include "campaign_phase.hpp"
#include "mission_menu_policy.hpp"

static bool tutorialUiTest=false;
static unsigned tutorialFixtureStart=~0u,tutorialOpen=~0u,tutorialIgnored=0;
static bool tutorialIgnoredLogged=false,tutorialAccepted=false,tutorialClosed=false;
static coop::MenuPointer localMenuPointer={},appliedMenuPointer={};
static void applyMissionPointer();

// Own research code. Hooks the guarded executable only; this is not an OS sandbox.
static wchar_t sandbox[32768], gameRoot[32768];
static HANDLE logHandle=INVALID_HANDLE_VALUE;
static DWORD logLock=0;
static unsigned inputCalls=0,timeCalls=0,presentCalls=0,sceneCalls=0;
static volatile unsigned gameActivatedAt=0,gameActivations=0;
static bool selfTest=false;
static bool autoTest=false;
static char autoTestName[32]="InGame";
static bool inputBot=false;
static ULONGLONG inputBotStarted=0;
static bool playBot=false;
static bool actionBot=false;
static ULONGLONG playBotStarted=0;
static bool deferredGameplayRequest=false;
static float autoGameSeconds=10.0f;
static unsigned networkRole=0,networkTick=0;
// Deferred play: the game opens before Host/Join and runs as an ordinary solo game; the role arrives
// later through <sandbox>\connect.json. A late partner then joins at the hub: the host sends its
// campaign progress, the joiner starts New Game alone, takes that progress and meets the host in the hub.
static bool networkDeferred=false,campaignJoinPending=false,campaignJoinSent=false,campaignJoinReceived=false,campaignJoined=false;
// Automatic start together: a partner who links while the host still waits in the main menu (no game loaded yet
// this session) watches the opening with the host (the route of "Start a new campaign together"). The host's
// Continue there falls back to joining at the hub. campaignSoloLoaded: this game already loaded a level alone.
static bool campaignAutoTogether=false,campaignTogetherOffered=false,campaignSoloLoaded=false,campaignAutoTogetherEnabled=true;
// The main menu stays free for both players (own options, audio, ...) until the host picks New Game or Continue:
// the host's offer goes out without waiting; that choice then waits for the partner's main menu and is exchanged once
// (first two-PC test, 2026-10-24: a shared main-menu cursor kept the friend out of its own settings).
static bool campaignTogetherDecided=false,campaignTogetherJoinerStart=false;static unsigned campaignOfferSequence=0;
// Research (COOP_PROBE_HOST_WAIT_LINK=1, host bot): stay in the main menu until a partner links, as a person waiting would.
static bool campaignHostWaitLink=false;
// Research (COOP_PROBE_HOST_CONTINUE=1, with HOST_WAIT_LINK): the host bot picks Continue after starting together.
static bool campaignHostContinue=false;
static BYTE campaignJoinProgress[0x1fc];static unsigned campaignJoinSelected=0;
static wchar_t profileRoot[32768];
static bool campaignMode=false,campaignFixture=false;
// Lockstep gameplay only once both seats are linked and joined.
static bool networkLockstep(){return networkRole&&!networkDeferred&&!campaignJoinPending;}
// Campaign hooks drive the game while synchronized, while a joiner catches up, and for the test bot
// playing an opened game alone. A host still away from the hub keeps playing untouched.
static bool campaignLocalDrive(){return (networkDeferred&&campaignFixture)||(campaignJoinPending&&networkRole==1&&!campaignJoinSent&&campaignFixture)||(campaignJoinPending&&networkRole==2&&campaignJoinReceived);}
static bool campaignLive(){return campaignMode&&((!networkDeferred&&!(campaignJoinPending&&networkRole==1&&!campaignJoinSent))||campaignLocalDrive());}
static bool networkReady=false,networkFailed=false,networkComplete=false;
static coop::InputWire* networkWire=0;
static unsigned networkInputHash[2]={0,0};
static std::uint64_t networkState=0;
static bool networkJitterFixture=false;
static bool networkPauseFixture=false;
// Research (COOP_PROBE_MENU_FIXTURE=1, solo Play): walks main menu -> Options -> Audio and back, then closes the game.
static bool menuFixture=false,menuContinueFixture=false,menuNavFixture=false;static DWORD menuNavAction=~0u;
static bool mainMenuAudio=false;
// Research (COOP_PROBE_SWAP_FIXTURE=1, host): holds the character-switch action at ticks 600-649 and 1500-1549.
static bool swapFixture=false;
static bool networkMutateFixture=false,networkCloseRequested=false;
static coop::Digest networkContent={};
static bool networkContentVerified=false;
static char networkArea[128]="Tokyo/1_Level/1_Area";
static bool networkMissionEndFixture=false,networkMissionEndInvoked=false;
static bool networkTransitionFixture=false,networkTransitionWarped=false,networkTransitionChanged=false;
static DWORD networkTransitionStartArea=0,networkTransitionStartSlot=0;
static unsigned networkTransitionTarget=1,networkTransitionCount=0,networkTransitionNextTick=900;
static DWORD networkTransitionUsedCenters[4][3]={};
static bool networkInSimulation=false;
static DWORD networkSimulationThread=0;
static LONG networkRngCalls=0;
static unsigned networkFrameLimit=3600;
static char networkAddress[64]="127.0.0.1";
static unsigned short networkPort=49375;
static bool networkCleanExit=true;
static HWND gameWindow=0;
// displayMode: 0 test window 640x480, 1 borderless, 2 window, 3 original (the game's own fullscreen and saved resolution).
static unsigned displayWidth=640,displayHeight=480,displayMode=0;
static bool networkPathSync=false;
static bool networkAmbientSplit=false;
static DWORD networkAmbientRng=0x84f19a21;
// Co-op character swap state (networkSwapSlot).
// Raw action of the game's character switch (keyboard TAB, scan 0F; run menu-audio-001 binding log).
static const unsigned networkSwapAction=13;
static bool networkSwapped=false,networkSwapLatched=false;static unsigned networkSwapHeld=0;
// Solo: one player, original input, no network; only sandbox and display handling.
static bool humanInputMode=false,humanInputFixture=false,virtualPadSelected=false,soloMode=false;
static bool probeDiagnostics=true;
static bool humanDisconnectFixture=false;
static bool humanKeyboardFixture=false;
static DWORD virtualPadIndex=3;
static bool networkInputClock=false;
static bool campaignPrepared=false;
// Native save slot (0..2) both seats chose for New Game; its default row varies between runs (043, 067-069).
static DWORD campaignSlot=0;
static void campaignOverwriteReset();
static unsigned campaignSlotFixture=~0u;
static bool campaignOverwriteFixtureNo=false;
static bool campaignNewGame=false,campaignProfile=false,campaignHub=false,campaignMission=false,campaignGameplay=false;
// Hub -> mission -> hub cycles: the committed mission start path and cycle count.
static char campaignMissionPath[33]={};static DWORD campaignMissionArea=~0u;static unsigned campaignCycle=0,campaignCycleTarget=1,campaignCycleStartTick=0;static bool campaignHubWalkReset=false;
static unsigned campaignPhaseSequence=0,campaignLoadSequence=0,campaignDelayMs=0;
static coop::Actions campaignPhysicalMenu={};
static coop::Actions networkBotActions(unsigned player,unsigned tick);
static void assignNativeDevice(unsigned slot,DWORD device);
static DWORD WINAPI humanInput(DWORD index,XINPUT_STATE* state);
static DWORD WINAPI humanCaps(DWORD index,DWORD flags,XINPUT_CAPABILITIES* caps);
static LONG exceptionCount=0;
static void log(const char* fmt,...) {
    if(logHandle==INVALID_HANDLE_VALUE || InterlockedExchange((LONG*)&logLock,1)) return;
    char b[4096]; va_list ap; va_start(ap,fmt); int n=_vsnprintf(b,sizeof(b)-3,fmt,ap); va_end(ap);
    if(n<0)n=sizeof(b)-3; b[n++]='\r';b[n++]='\n'; DWORD done; WriteFile(logHandle,b,n,&done,0); FlushFileBuffers(logHandle);
    InterlockedExchange((LONG*)&logLock,0);
}
static bool prefix(const wchar_t* p,const wchar_t* root) {
    size_t n=wcslen(root); return n && !_wcsnicmp(p,root,n) && (p[n]==0||p[n]=='\\'||p[n]=='/');
}
static bool fullpath(const wchar_t* in,wchar_t* out) {
    if(!in)return false;DWORD count=GetFullPathNameW(in,32768,out,0);if(!count||count>=32768)return false;
    if(!_wcsnicmp(out,L"\\\\?\\",4)) memmove(out,out+4,(wcslen(out+4)+1)*sizeof(wchar_t));
    return true;
}
static bool allowed(const wchar_t* p) {
    wchar_t q[32768]; if(!fullpath(p,q) || !(prefix(q,sandbox)||(profileRoot[0]&&prefix(q,profileRoot))||(!selfTest&&prefix(q,gameRoot))))return false;
    // No reparse-point traversal within writable roots.
    for(wchar_t* c=q+3;*c;c++) if(*c=='\\') {wchar_t old=*c;*c=0;DWORD a=GetFileAttributesW(q);*c=old;if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT))return false;}
    DWORD a=GetFileAttributesW(q); if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT))return false;
    // The launcher links the game files to the player's own install (hard links: no second copy on disk), so a
    // game file shared with another name must never be written, truncated or deleted here: it is the player's
    // original. New files beside them stay allowed.
    if(a!=INVALID_FILE_ATTRIBUTES&&!(a&FILE_ATTRIBUTE_DIRECTORY)&&!selfTest&&prefix(q,gameRoot)&&!prefix(q,sandbox)&&!(profileRoot[0]&&prefix(q,profileRoot))){
        HANDLE h=CreateFileW(q,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,0,0);BY_HANDLE_FILE_INFORMATION info={};
        bool shared=h==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(h,&info)||info.nNumberOfLinks>1;if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
        if(shared){static unsigned sharedLogs=0;if(sharedLogs++<16)log("BLOCK write to a linked game file %ls",q);return false;}}
    return true;
}
static bool asciiWide(const char* in,wchar_t* out) {return in&&MultiByteToWideChar(CP_ACP,0,in,-1,out,32768)>0;}
static void mkdirs(const wchar_t* path) {
    wchar_t b[32768]; lstrcpynW(b,path,32768);
    for(wchar_t* p=b+3;*p;p++)if(*p=='\\'){*p=0;CreateDirectoryW(b,0);*p='\\';} CreateDirectoryW(b,0);
}
static HRESULT WINAPI folder(HWND,int id,HANDLE,DWORD,wchar_t* out) {
    const wchar_t* leaf=0; switch(id&0xff){case 5:leaf=L"Documents";break;case 0x1a:leaf=L"AppData\\Roaming";break;case 0x1c:leaf=L"AppData\\Local";break;case 0x23:leaf=L"ProgramData";break;case 0x25:{wchar_t b[MAX_PATH];GetSystemDirectoryW(b,MAX_PATH);lstrcpynW(out,b,MAX_PATH);log("folder system id=%x real system allowed",id);return S_OK;}default:log("BLOCK unknown folder id=%x",id);SetLastError(ERROR_ACCESS_DENIED);return E_ACCESSDENIED;}
    // Saves (Documents) persist in the package profile between sessions; everything else stays per session.
    wchar_t p[32768];_snwprintf(p,32768,L"%s\\%s",(id&0xff)==5&&profileRoot[0]?profileRoot:sandbox,leaf);if(wcslen(p)>=MAX_PATH)return E_FAIL;mkdirs(p);lstrcpyW(out,p);log("folder id=%x redirected",id);return S_OK;
}
// The game also builds paths with ExpandEnvironmentStringsW (%USERPROFILE%...), which bypassed the
// Documents redirect: test and co-op games saw the owner's real saves (slot list opened on the first
// free real slot, 067-079). Anything under the real user profile is mapped into the redirected folders.
static DWORD (WINAPI* originalExpand)(LPCWSTR,LPWSTR,DWORD);static unsigned expandLogs=0;
static DWORD WINAPI expandHook(LPCWSTR source,LPWSTR out,DWORD size){
    wchar_t expanded[32768];DWORD length=originalExpand(source,expanded,32768);if(!length||length>32768)return originalExpand(source,out,size);
    wchar_t profile[MAX_PATH]={0},documents[MAX_PATH]={0},result[32768];lstrcpynW(result,expanded,32768);
    GetEnvironmentVariableW(L"USERPROFILE",profile,MAX_PATH);SHGetFolderPathW(0,CSIDL_PERSONAL,0,SHGFP_TYPE_CURRENT,documents);
    wchar_t mapped[MAX_PATH]={0};
    if(documents[0]&&prefix(expanded,documents)&&SUCCEEDED(folder(0,CSIDL_PERSONAL,0,0,mapped)))_snwprintf(result,32768,L"%s%s",mapped,expanded+wcslen(documents));
    else if(profile[0]&&prefix(expanded,profile))_snwprintf(result,32768,L"%s%s",sandbox,expanded+wcslen(profile));
    if(expandLogs<32&&wcscmp(result,expanded)){++expandLogs;log("ENV expand redirected %ls -> %ls",expanded,result);}
    DWORD needed=(DWORD)wcslen(result)+1;if(out&&size>=needed)lstrcpyW(out,result);return needed;
}

struct Key {wchar_t path[1024];bool used;};
struct Value {wchar_t path[1024],name[256];DWORD type,size;BYTE data[2048];bool used;};
static Key keys[96];static Value values[256];
// Game settings (volumes, controls layout, ...) the game writes to its registry key are kept in the package
// (COOP_PROBE_SETTINGS, set by the launcher for real play) so they survive the next launch; before, every launch
// started with an empty virtual registry. The display size and language stay launcher choices and are not kept.
static wchar_t settingsPath[4096];
static bool settingKept(const wchar_t* name){return _wcsicmp(name,L"ResolutionX")&&_wcsicmp(name,L"ResolutionY")&&_wcsicmp(name,L"Language");}
static void saveSettings(){
    if(!settingsPath[0])return;{wchar_t folder[4096];lstrcpyW(folder,settingsPath);wchar_t* slash=wcsrchr(folder,L'\\');if(slash){*slash=0;CreateDirectoryW(folder,0);}}wchar_t temporary[4200];_snwprintf_s(temporary,_countof(temporary),_TRUNCATE,L"%s.tmp",settingsPath);
    HANDLE file=CreateFileW(temporary,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);if(file==INVALID_HANDLE_VALUE)return;
    bool ok=true;DWORD done=0;auto put=[&](const void* data,DWORD size){if(ok&&(!WriteFile(file,data,size,&done,0)||done!=size))ok=false;};
    const char magic[8]={'F','O','F','S','E','T','1',0};put(magic,8);
    for(auto&v:values)if(v.used&&settingKept(v.name)){WORD pathLength=(WORD)wcslen(v.path),nameLength=(WORD)wcslen(v.name);put(&pathLength,2);put(v.path,pathLength*2);put(&nameLength,2);put(v.name,nameLength*2);put(&v.type,4);put(&v.size,4);put(v.data,v.size);}
    CloseHandle(file);if(!ok||!MoveFileExW(temporary,settingsPath,MOVEFILE_REPLACE_EXISTING))DeleteFileW(temporary);
}
static void loadSettings(){
    if(!settingsPath[0])return;HANDLE file=CreateFileW(settingsPath,GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);if(file==INVALID_HANDLE_VALUE)return;
    static BYTE buffer[256*1024];DWORD size=0;BOOL read=ReadFile(file,buffer,sizeof(buffer),&size,0);CloseHandle(file);if(!read||size<8||memcmp(buffer,"FOFSET1",8))return;
    DWORD at=8;unsigned loaded=0;auto take=[&](void* out,DWORD n){if(at+n>size)return false;memcpy(out,buffer+at,n);at+=n;return true;};
    for(unsigned slot=0;slot<256&&at<size;){Value& v=values[slot];WORD pathLength=0,nameLength=0;
        if(!take(&pathLength,2)||pathLength>=1024||!take(v.path,pathLength*2u)||!take(&nameLength,2)||nameLength>=256||!take(v.name,nameLength*2u)||!take(&v.type,4)||!take(&v.size,4)||v.size>2048||!take(v.data,v.size)){memset(&v,0,sizeof(v));break;}
        v.path[pathLength]=0;v.name[nameLength]=0;if(!settingKept(v.name)){memset(&v,0,sizeof(v));continue;}v.used=true;++loaded;++slot;}
    log("SETTINGS loaded=%u from the package",loaded);
}
// Keyboard players start with the game's Laptop controls layout (owner request): only while the player has never
// saved a layout (ControllerMap1 missing) and no XInput controller is connected when the game reads it.
static bool controllerConnected(){
    HMODULE xinput=GetModuleHandleW(L"xinput1_1.dll");if(!xinput)xinput=LoadLibraryW(L"xinput1_1.dll");if(!xinput)return false;
    auto get=(DWORD(WINAPI*)(DWORD,XINPUT_STATE*))GetProcAddress(xinput,"XInputGetState");if(!get)return false;
    for(DWORD i=0;i<4;i++){XINPUT_STATE state={};if(get(i,&state)==ERROR_SUCCESS)return true;}return false;
}

static bool keyPath(HKEY key,wchar_t* out) {
    ULONG_PTR n=(ULONG_PTR)key;if(n>=0xcafe0100&&n<0xcafe0160){Key&k=keys[n-0xcafe0100];if(k.used){lstrcpyW(out,k.path);return true;}}
    if(key==HKEY_CURRENT_USER){lstrcpyW(out,L"HKCU");return true;}if(key==HKEY_LOCAL_MACHINE){lstrcpyW(out,L"HKLM");return true;}
    return false;
}
static LSTATUS newKey(HKEY parent,LPCWSTR sub,PHKEY out) {
    wchar_t root[1024],path[1024];if(!out||!keyPath(parent,root)||wcslen(root)+(sub?wcslen(sub):0)+2>=1024)return ERROR_ACCESS_DENIED;
    _snwprintf(path,1024,L"%s\\%s",root,sub?sub:L"");
    for(unsigned i=0;i<96;i++)if(keys[i].used&&!_wcsicmp(keys[i].path,path)){*out=(HKEY)(ULONG_PTR)(0xcafe0100+i);return ERROR_SUCCESS;}
    for(unsigned i=0;i<96;i++)if(!keys[i].used){keys[i].used=true;lstrcpyW(keys[i].path,path);*out=(HKEY)(ULONG_PTR)(0xcafe0100+i);log("registry key virtualized index=%u",i);return ERROR_SUCCESS;}
    log("BLOCK registry key capacity");return ERROR_NOT_ENOUGH_MEMORY;
}
static LSTATUS WINAPI regCreate(HKEY a,LPCWSTR b,PHKEY c){return newKey(a,b,c);}
static LSTATUS WINAPI regOpenA(HKEY a,LPCSTR b,DWORD,REGSAM,PHKEY c){wchar_t w[1024];if(!b||!MultiByteToWideChar(CP_ACP,0,b,-1,w,1024))return ERROR_ACCESS_DENIED;return newKey(a,w,c);}
static LSTATUS WINAPI regClose(HKEY a){wchar_t p[1024];return keyPath(a,p)?ERROR_SUCCESS:ERROR_INVALID_HANDLE;}
static LSTATUS WINAPI regSet(HKEY k,LPCWSTR name,DWORD,DWORD type,const BYTE* data,DWORD n) {
    wchar_t path[1024];if(!keyPath(k,path)||n>2048||!name||wcslen(name)>=256||(!data&&n))return ERROR_ACCESS_DENIED;
    Value* v=0;for(auto&x:values)if(x.used&&!_wcsicmp(path,x.path)&&!_wcsicmp(name,x.name)){v=&x;break;}
    if(!v)for(auto&x:values)if(!x.used){v=&x;break;}if(!v)return ERROR_NOT_ENOUGH_MEMORY;
    v->used=true;lstrcpyW(v->path,path);lstrcpyW(v->name,name);v->type=type;v->size=n;if(n)memcpy(v->data,data,n);log("registry set virtualized name=%ls bytes=%lu",name,n);if(settingKept(name))saveSettings();return ERROR_SUCCESS;
}
static LSTATUS WINAPI regQuery(HKEY k,LPCWSTR name,LPDWORD,LPDWORD type,LPBYTE data,LPDWORD n) {
    wchar_t path[1024];if(!n||!name||!keyPath(k,path))return ERROR_ACCESS_DENIED;
    // Known native settings only. No guessed fullscreen or language key values.
    DWORD forced=0;bool known=false;
    if(!_wcsicmp(name,L"ResolutionX")){forced=displayWidth;known=true;}if(!_wcsicmp(name,L"ResolutionY")){forced=displayHeight;known=true;}
    if(known&&displayMode==3){for(auto&v:values)if(v.used&&!_wcsicmp(path,v.path)&&!_wcsicmp(name,v.name))known=false;if(!displayWidth||!displayHeight)known=false;}
    if(known){if(type)*type=REG_DWORD;if(data&&*n<4){*n=4;return ERROR_MORE_DATA;}if(data)memcpy(data,&forced,4);*n=4;log("registry known setting name=%ls value=%lu",name,forced);return ERROR_SUCCESS;}
    for(auto&v:values)if(v.used&&!_wcsicmp(path,v.path)&&!_wcsicmp(name,v.name)){if(type)*type=v.type;DWORD capacity=*n;*n=v.size;if(data&&capacity<v.size)return ERROR_MORE_DATA;if(data)memcpy(data,v.data,v.size);return ERROR_SUCCESS;}
    // FirstRunCompleted only makes the movie screen unskippable on the game's first launch (529480, read once by the
    // movie scene at 4A02A8). Players asked for skippable movies, so real play reports it as already done.
    if(((humanInputMode&&!humanInputFixture)||soloMode)&&!_wcsicmp(name,L"FirstRunCompleted")){
        DWORD value=1;if(type)*type=REG_DWORD;if(data&&*n<4){*n=4;return ERROR_MORE_DATA;}if(data)memcpy(data,&value,4);*n=4;
        log("registry FirstRunCompleted not saved yet: movies skippable from the first launch");return ERROR_SUCCESS;}
    // First launch volumes (owner request): music 50%, speech and effects 80%. The game stores attenuation steps,
    // 0 = full volume, 10 = silent (Audio page slider = 10 - value); later changes are kept in the package.
    if(((humanInputMode&&!humanInputFixture)||soloMode)&&(!_wcsicmp(name,L"VolumeMusic")||!_wcsicmp(name,L"VolumeDialogue")||!_wcsicmp(name,L"VolumeSfx"))){
        DWORD value=!_wcsicmp(name,L"VolumeMusic")?5u:2u;if(type)*type=REG_DWORD;if(data&&*n<4){*n=4;return ERROR_MORE_DATA;}if(data)memcpy(data,&value,4);*n=4;
        log("registry first-launch volume name=%ls value=%lu",name,value);return ERROR_SUCCESS;}
    if(!_wcsicmp(name,L"ControllerMap1")&&((humanInputMode&&!humanInputFixture)||soloMode)&&!controllerConnected()){
        static const wchar_t laptop[]=L"Laptop";if(type)*type=REG_SZ;DWORD capacity=*n;*n=sizeof(laptop);if(data&&capacity<sizeof(laptop))return ERROR_MORE_DATA;if(data)memcpy(data,laptop,sizeof(laptop));
        log("registry ControllerMap1 not saved yet and no controller connected: Laptop layout");return ERROR_SUCCESS;}
    log("registry query missing name=%ls",name);return ERROR_FILE_NOT_FOUND;
}
static LSTATUS WINAPI regQueryA(HKEY k,LPCSTR name,LPDWORD reserved,LPDWORD type,LPBYTE data,LPDWORD n){wchar_t w[256];if(!name||!MultiByteToWideChar(CP_ACP,0,name,-1,w,256))return ERROR_ACCESS_DENIED;return regQuery(k,w,reserved,type,data,n);}

static HANDLE (WINAPI* originalCreateW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
// Save-slot diagnostics: which save paths the game checks and opens (runs 067-069 saw slot row 2).
static DWORD (WINAPI* originalAttributes)(LPCWSTR);static unsigned saveProbeLogs=0;
static DWORD WINAPI attributesHook(LPCWSTR p){DWORD result=originalAttributes(p);if(p&&saveProbeLogs<64&&StrStrIW(p,L"save")){++saveProbeLogs;log("SAVE_PROBE GetFileAttributesW path=%ls result=%08lx",p,result);}return result;}
static HANDLE (WINAPI* originalCreateA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static BOOL (WINAPI* originalWrite)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
static FILE* (__cdecl* originalWfopen)(const wchar_t*,const wchar_t*);
static FILE* (__cdecl* originalFopen)(const char*,const char*);
static int (__cdecl* originalMkdir)(const wchar_t*);
static int (__cdecl* originalRemove)(const wchar_t*);
static int* (__cdecl* originalErrno)();
static unsigned fileReadLogs=0;
static unsigned failedFileLogs=0;
static void fileRead(const wchar_t* path){if(path&&fileReadLogs<160&&(wcsstr(path,L".byte_code")||wcsstr(path,L"user.ini")||wcsstr(path,L"level.ini")||wcsstr(path,L".dict")||wcsstr(path,L".res")||wcsstr(path,L".nlx")||wcsstr(path,L".elf"))){++fileReadLogs;log("file read candidate %ls",path);}}
static bool writing(DWORD access,DWORD disp){return (access&(GENERIC_WRITE|GENERIC_ALL|DELETE|WRITE_DAC|WRITE_OWNER|FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_ATTRIBUTES|FILE_WRITE_EA))||disp!=OPEN_EXISTING;}
static HANDLE WINAPI createW(LPCWSTR p,DWORD a,DWORD share,LPSECURITY_ATTRIBUTES sa,DWORD disp,DWORD flags,HANDLE t){if(p&&saveProbeLogs<64&&StrStrIW(p,L"save")){++saveProbeLogs;log("SAVE_PROBE CreateFileW path=%ls access=%08lx disposition=%lu",p,a,disp);}if((writing(a,disp)||(flags&FILE_FLAG_DELETE_ON_CLOSE))&&!allowed(p)){log("BLOCK CreateFileW write outside sandbox");SetLastError(ERROR_ACCESS_DENIED);return INVALID_HANDLE_VALUE;}if(!writing(a,disp))fileRead(p);HANDLE h=originalCreateW(p,a,share,sa,disp,flags,t);DWORD error=GetLastError();if(h==INVALID_HANDLE_VALUE&&failedFileLogs++<60)log("FILE FAILED W path=%ls error=%lu access=%08lx disposition=%lu",p,error,a,disp);SetLastError(error);return h;}
// Research (hub sidekick): name of the sidekick the hub is loading (see campaignBuildDescriptor).
static char campaignHubLoadName[32]={};
static HANDLE WINAPI createA(LPCSTR p,DWORD a,DWORD share,LPSECURITY_ATTRIBUTES sa,DWORD disp,DWORD flags,HANDLE t){if(p&&saveProbeLogs<64&&StrStrIA(p,"save")){++saveProbeLogs;log("SAVE_PROBE CreateFileA path=%s access=%08lx",p,a);}wchar_t w[32768];if((writing(a,disp)||(flags&FILE_FLAG_DELETE_ON_CLOSE))&&(!asciiWide(p,w)||!allowed(w))){log("BLOCK CreateFileA write outside sandbox");SetLastError(ERROR_ACCESS_DENIED);return INVALID_HANDLE_VALUE;}if(!writing(a,disp)&&asciiWide(p,w))fileRead(w);HANDLE h=originalCreateA(p,a,share,sa,disp,flags,t);DWORD error=GetLastError();if(h==INVALID_HANDLE_VALUE&&failedFileLogs++<60)log("FILE FAILED A path=%s error=%lu access=%08lx disposition=%lu",p,error,a,disp);SetLastError(error);return h;}
static BOOL WINAPI write(HANDLE h,LPCVOID b,DWORD n,LPDWORD done,LPOVERLAPPED o){wchar_t p[32768];DWORD c=GetFinalPathNameByHandleW(h,p,32768,FILE_NAME_NORMALIZED);if(!c||c>=32768||!allowed(p)){log("BLOCK WriteFile unknown/outside handle");if(done)*done=0;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}return originalWrite(h,b,n,done,o);}
static FILE* __cdecl wfopenHook(const wchar_t* p,const wchar_t* mode){if(p&&saveProbeLogs<64&&StrStrIW(p,L"save")){++saveProbeLogs;log("SAVE_PROBE _wfopen path=%ls mode=%ls",p,mode?mode:L"-");}if(!mode)return 0;if(wcspbrk(mode,L"wa+")&&!allowed(p)){log("BLOCK _wfopen write outside sandbox");return 0;}if(!wcspbrk(mode,L"wa+"))fileRead(p);return originalWfopen(p,mode);}
static FILE* __cdecl fopenHook(const char* p,const char* mode){if(p&&saveProbeLogs<64&&StrStrIA(p,"save")){++saveProbeLogs;log("SAVE_PROBE fopen path=%s mode=%s",p,mode?mode:"-");}wchar_t w[32768];if(!mode)return 0;if(strpbrk(mode,"wa+")&&(!asciiWide(p,w)||!allowed(w))){log("BLOCK fopen write outside sandbox");return 0;}if(!strpbrk(mode,"wa+")&&asciiWide(p,w))fileRead(w);return originalFopen(p,mode);}
static int __cdecl mkdirHook(const wchar_t*p){if(!allowed(p)){wchar_t normalized[32768];bool ancestor=false;if(p&&fullpath(p,normalized)){DWORD attrs=GetFileAttributesW(normalized);ancestor=attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_DIRECTORY)&&(prefix(sandbox,normalized)||(!selfTest&&prefix(gameRoot,normalized)));}if(ancestor){if(originalErrno)*originalErrno()=EEXIST;SetLastError(ERROR_ALREADY_EXISTS);log("_wmkdir EXISTING ancestor, no mutation path=%ls",p);return -1;}if(originalErrno)*originalErrno()=EACCES;SetLastError(ERROR_ACCESS_DENIED);log("BLOCK _wmkdir outside sandbox path=%ls",p?p:L"<null>");return -1;}log("_wmkdir allowed path=%ls",p);return originalMkdir(p);}
static int __cdecl removeHook(const wchar_t*p){if(!allowed(p)){log("BLOCK _wremove outside sandbox");return -1;}return originalRemove(p);}
static HINSTANCE WINAPI shell(HWND,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR,INT){log("BLOCK ShellExecuteW");SetLastError(ERROR_ACCESS_DENIED);return (HINSTANCE)SE_ERR_ACCESSDENIED;}
static BOOL WINAPI clip(const RECT*){log("cursor ClipCursor suppressed");return TRUE;}
static int WINAPI cursor(BOOL){return 0;}
static HWND (WINAPI* originalWindow)(DWORD,LPCSTR,LPCSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,LPVOID);
static BOOL (WINAPI* originalShow)(HWND,int);
static WNDPROC originalGameWindowProc=0;
// Set when this player closes the game window during a co-op session (WM_CLOSE -> RequestStop).
static bool campaignLocalStop=false,campaignPeerStop=false;
static bool campaignStopping(){return campaignLocalStop||campaignPeerStop;}
static bool __cdecl campaignStopPending(){return campaignStopping();}
// The game draws its own hand cursor from DirectInput deltas. Its ShowCursor/ClipCursor calls are
// suppressed for bot tests (and it would clip in client coordinates), so in real play the probe hides
// the Windows arrow over the game and keeps the mouse inside the focused game window instead.
// While an opened game is played alone (waiting for Host/Join) the arrow is still hidden over the game,
// but the mouse is not kept inside it, so the launcher on top stays reachable in every display mode.
static bool realPlayCursor(){return (humanInputMode&&!humanInputFixture)||soloMode;}
static bool realPlayClip(){return !networkDeferred&&!campaignJoinPending;}
static void clipToGameWindow(HWND hwnd){if(!realPlayClip()){ClipCursor(NULL);return;}RECT client={};POINT a={0,0},b={0,0};if(!GetClientRect(hwnd,&client))return;b.x=client.right;b.y=client.bottom;ClientToScreen(hwnd,&a);ClientToScreen(hwnd,&b);RECT area={a.x,a.y,b.x,b.y};if(area.right>area.left&&area.bottom>area.top)ClipCursor(&area);}
static LRESULT CALLBACK gameWindowProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam){
    if(hwnd==gameWindow&&realPlayCursor()){
        if(message==WM_SETCURSOR&&LOWORD(lparam)==HTCLIENT){SetCursor(NULL);if(GetForegroundWindow()==hwnd)clipToGameWindow(hwnd);return TRUE;}
        if(message==WM_ACTIVATE){if(LOWORD(wparam)==WA_INACTIVE)ClipCursor(NULL);else clipToGameWindow(hwnd);}
        if((message==WM_SIZE||message==WM_MOVE)&&GetForegroundWindow()==hwnd)clipToGameWindow(hwnd);
        if(message==WM_DESTROY)ClipCursor(NULL);
    }
    if(hwnd==gameWindow&&(message==WM_ACTIVATEAPP||message==WM_ACTIVATE||message==WM_SHOWWINDOW||(message==WM_SIZE&&wparam!=SIZE_RESTORED))){
        log("WINDOW_EVENT msg=%04x wparam=%08lx iconic=%u foreground=%u",message,(unsigned long)wparam,IsIconic(hwnd)?1u:0u,GetForegroundWindow()==hwnd?1u:0u);
        if(message==WM_ACTIVATEAPP&&wparam){gameActivations++;gameActivatedAt=presentCalls;}}
    if(message==WM_CLOSE&&hwnd==gameWindow&&networkRole&&networkReady&&!networkComplete&&!networkFailed&&networkWire){
        bool sent=networkWire->RequestStop();campaignLocalStop=true;log("NET user stop requested sent=%u",sent?1:0);
    }
    return CallWindowProcA(originalGameWindowProc,hwnd,message,wparam,lparam);
}
static HWND WINAPI window(DWORD ex,LPCSTR cl,LPCSTR title,DWORD requestedStyle,int requestedX,int requestedY,int requestedWidth,int requestedHeight,HWND parent,HMENU menu,HINSTANCE instance,LPVOID param){bool interactive=(humanInputMode&&!humanInputFixture)||soloMode;log("window 640x480 interactive=%u",interactive);DWORD style=WS_OVERLAPPEDWINDOW;int x=0,y=0,width=660,height=520;
    if(displayMode==3){style=requestedStyle;x=requestedX;y=requestedY;width=requestedWidth;height=requestedHeight;log("DISPLAY window mode=original (game settings) resolution=%ux%u",displayWidth,displayHeight);}
    else if(!parent&&displayMode){HMONITOR monitor=MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY);MONITORINFO info={sizeof(info)};GetMonitorInfoW(monitor,&info);RECT area=info.rcMonitor;
        if(displayMode==1){style=WS_POPUP;x=area.left;y=area.top;width=area.right-area.left;height=area.bottom-area.top;}
        else{RECT frame={0,0,(LONG)displayWidth,(LONG)displayHeight};AdjustWindowRectEx(&frame,style,FALSE,ex);width=frame.right-frame.left;height=frame.bottom-frame.top;x=area.left+((area.right-area.left)-width)/2;y=area.top+((area.bottom-area.top)-height)/2;if(x<area.left)x=area.left;if(y<area.top)y=area.top;}
        log("DISPLAY window mode=%s resolution=%ux%u rect=%d,%d,%d,%d",displayMode==1?"borderless":"window",displayWidth,displayHeight,x,y,width,height);}
    HWND result=originalWindow(interactive?ex:ex|WS_EX_NOACTIVATE,cl,title,style,x,y,width,height,parent,menu,instance,param);if(result&&!parent){gameWindow=result;
        if((networkRole||networkDeferred)&&!selfTest){WNDPROC previous=(WNDPROC)GetWindowLongPtrA(result,GWLP_WNDPROC);
            if(previous!=(WNDPROC)0x402d5c){log("FAIL game window procedure guard original=%p",previous);DestroyWindow(result);return 0;}
            originalGameWindowProc=previous;SetLastError(0);
            if((WNDPROC)SetWindowLongPtrA(result,GWLP_WNDPROC,(LONG_PTR)gameWindowProc)!=previous){log("FAIL game window procedure installation error=%lu",GetLastError());DestroyWindow(result);return 0;}
            log("WINDOW owned close hook installed original=%p",previous);
        }
    }return result;}
static BOOL WINAPI show(HWND h,int){return originalShow(h,(humanInputMode&&!humanInputFixture)||soloMode?SW_SHOWNORMAL:SW_SHOWNOACTIVATE);}

typedef HRESULT (WINAPI* CreateDeviceFn)(IDirect3D9*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice9**);
typedef HRESULT (WINAPI* ResetFn)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
typedef HRESULT (WINAPI* PresentFn)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
typedef HRESULT (WINAPI* EndSceneFn)(IDirect3DDevice9*);
static IDirect3D9* (WINAPI* originalD3D)(UINT);static CreateDeviceFn originalDevice=0;static ResetFn originalReset=0;static PresentFn originalPresent=0;static EndSceneFn originalEndScene=0;
static bool patchComSlot(void** table,unsigned index,void* replacement,void** original){
    if(!table||!replacement||!original){log("FAIL null slot-hook argument");return false;}
    void** slot=table+index;void* current=*slot;
    if(current==replacement)return *original!=0;
    if(*original&&*original!=current){log("FAIL differing COM implementation slot=%u",index);return false;}
    DWORD old=0;if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old)){log("FAIL COM slot protection index=%u",index);return false;}
    *original=current;InterlockedExchangePointer((PVOID volatile*)slot,replacement);DWORD ignored=0;
    bool restored=VirtualProtect(slot,sizeof(void*),old,&ignored)!=0;
    log("COM slot patched index=%u original=%p protection_restored=%u",index,current,restored?1:0);return restored;
}
static void logParams(const char* event,D3DPRESENT_PARAMETERS* p){log("%s width=%u height=%u format=%u count=%u multisample=%u quality=%lu swap=%u windowed=%u refresh=%u interval=%u autodepth=%u depthformat=%u",event,p->BackBufferWidth,p->BackBufferHeight,p->BackBufferFormat,p->BackBufferCount,p->MultiSampleType,p->MultiSampleQuality,p->SwapEffect,p->Windowed,p->FullScreen_RefreshRateInHz,p->PresentationInterval,p->EnableAutoDepthStencil,p->AutoDepthStencilFormat);}
// Always a windowed device: borderless covers the monitor without exclusive mode, so Alt+Tab never loses the device.
static void windowParams(D3DPRESENT_PARAMETERS* p){if(displayMode==3)return;p->Windowed=TRUE;p->FullScreen_RefreshRateInHz=0;p->BackBufferWidth=displayWidth;p->BackBufferHeight=displayHeight;}
static HRESULT WINAPI reset(IDirect3DDevice9* p,D3DPRESENT_PARAMETERS* params){logParams("D3D Reset ORIGINAL",params);windowParams(params);log("D3D Reset forced windowed; native format preserved");HRESULT hr=originalReset(p,params);log("D3D Reset result hr=%08lx",hr);return hr;}
static void capture(IDirect3DDevice9* device,unsigned frame){
    if(!probeDiagnostics)return;
    IDirect3DSurface9 *back=0,*resolved=0,*memory=0;HRESULT hr=device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back);D3DSURFACE_DESC desc={};D3DLOCKED_RECT rect={};HANDLE file=INVALID_HANDLE_VALUE;bool locked=false;
    do{if(FAILED(hr)||!back)break;hr=back->GetDesc(&desc);if(FAILED(hr)||!desc.Width||!desc.Height||desc.Width>4096||desc.Height>4096||(desc.Format!=D3DFMT_X8R8G8B8&&desc.Format!=D3DFMT_A8R8G8B8)){hr=E_FAIL;break;}
        IDirect3DSurface9* source=back;if(desc.MultiSampleType!=D3DMULTISAMPLE_NONE){hr=device->CreateRenderTarget(desc.Width,desc.Height,desc.Format,D3DMULTISAMPLE_NONE,0,FALSE,&resolved,0);if(FAILED(hr))break;hr=device->StretchRect(back,0,resolved,0,D3DTEXF_NONE);if(FAILED(hr))break;source=resolved;}
        hr=device->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&memory,0);if(FAILED(hr))break;hr=device->GetRenderTargetData(source,memory);if(FAILED(hr))break;hr=memory->LockRect(&rect,0,D3DLOCK_READONLY);if(FAILED(hr))break;locked=true;
        wchar_t path[32768];_snwprintf(path,32768,L"%s\\game-frame-%u.bmp",sandbox,frame);file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,0,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,0);if(file==INVALID_HANDLE_VALUE){hr=HRESULT_FROM_WIN32(GetLastError());break;}
        DWORD bytes=desc.Width*desc.Height*4;BITMAPFILEHEADER fh={};fh.bfType=0x4d42;fh.bfOffBits=sizeof(fh)+sizeof(BITMAPINFOHEADER);fh.bfSize=fh.bfOffBits+bytes;BITMAPINFOHEADER info={};info.biSize=sizeof(info);info.biWidth=desc.Width;info.biHeight=-(LONG)desc.Height;info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;info.biSizeImage=bytes;DWORD done=0;
        if(!WriteFile(file,&fh,sizeof(fh),&done,0)||done!=sizeof(fh)||!WriteFile(file,&info,sizeof(info),&done,0)||done!=sizeof(info)){hr=E_FAIL;break;}
        for(UINT y=0;y<desc.Height;y++)if(!WriteFile(file,(BYTE*)rect.pBits+y*rect.Pitch,desc.Width*4,&done,0)||done!=desc.Width*4){hr=E_FAIL;break;}
    }while(false);
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);if(locked)memory->UnlockRect();if(memory)memory->Release();if(resolved)resolved->Release();if(back)back->Release();log("game-only backbuffer capture frame=%u hr=%08lx width=%u height=%u",frame,hr,desc.Width,desc.Height);
}
static volatile LONG campaignCaptureAt=0;static volatile LONG campaignCaptureTag=0;
static HRESULT WINAPI present(IDirect3DDevice9*p,const RECT*a,const RECT*b,HWND c,const RGNDATA*d){
    static ULONGLONG started=0;static unsigned lastCapture=0;
    ULONGLONG now=GetTickCount64();if(!started)started=now;unsigned seconds=(unsigned)((now-started)/1000);
    unsigned n=++presentCalls;
    // Frame rate every 10 s with the game's flow (16 menus/hub, 128 missions): tells smooth from stuttering in real
    // sessions and in the fake-internet tests.
    {static ULONGLONG windowStart=0;static unsigned windowFrames=0;ULONGLONG tick=GetTickCount64();if(!windowStart)windowStart=tick;++windowFrames;
        if(tick-windowStart>=10000){DWORD flow=*(volatile DWORD*)0x69e72c,current=0;if(flow&&!IsBadReadPtr((void*)(flow+8),4))current=*(volatile DWORD*)(flow+8);log("D3D FPS last_10s=%.1f flow=%lu",windowFrames*1000.0/(tick-windowStart),current);windowStart=tick;windowFrames=0;}}
    // Research: screenshots shortly after a requested moment (frame numbers 300000+offset).
    if(campaignCaptureAt){LONG offset=(LONG)n-campaignCaptureAt;if(offset==30||offset==90||offset==180||offset==300)capture(p,300000+campaignCaptureTag*1000+offset);if(offset>300)campaignCaptureAt=0;}if(n==1||n%6000==0){log("D3D Present count=%u elapsed_s=%u",n,seconds);if(!selfTest)log("read-only state rng=%08lx sim_mode=%lu fixed_sim_fps=%g",*(volatile DWORD*)0x58cf24,*(volatile DWORD*)0x69ed24,*(volatile float*)0x591df8);}
    if(n==120)capture(p,n);if(seconds>=5&&seconds/5>lastCapture){lastCapture=seconds/5;capture(p,200000+seconds);}
    // 60 frames per second, as in co-op lockstep, also when a player plays alone: uncapped, the game ran at about
    // 375 fps (owner session 20261023T173158472) and characters could not walk onto low floor plates.
    if(playBot||networkRole||humanInputMode||soloMode){
        static LARGE_INTEGER frequency={0},next={0};LARGE_INTEGER counter;
        if(!frequency.QuadPart)QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&counter);
        if(!next.QuadPart||counter.QuadPart>next.QuadPart+frequency.QuadPart/20)next.QuadPart=counter.QuadPart;
        while(counter.QuadPart<next.QuadPart){Sleep(1);QueryPerformanceCounter(&counter);}next.QuadPart+=frequency.QuadPart/60;
    }
    // Black-screen reports: log failing Presents (device lost etc.) and the first frames after the game window is
    // activated again, with a picture of what the game drew (up to five per session, also in normal play).
    HRESULT result=originalPresent(p,a,b,c,d);
    if(FAILED(result)||result==S_PRESENT_OCCLUDED){static unsigned failures=0;if(failures++<20||failures%600==0)log("D3D PRESENT_RESULT hr=%08lx count=%u cooperative=%08lx",(unsigned long)result,failures,(unsigned long)p->TestCooperativeLevel());}
    if(gameActivatedAt&&n-gameActivatedAt==120){log("D3D AFTER_ACTIVATE frames=120 hr=%08lx cooperative=%08lx",(unsigned long)result,(unsigned long)p->TestCooperativeLevel());if(gameActivations<=5){bool saved=probeDiagnostics;probeDiagnostics=true;capture(p,400000+gameActivations);probeDiagnostics=saved;}gameActivatedAt=0;}
    return result;
}
static HRESULT WINAPI endScene(IDirect3DDevice9* p){HRESULT hr=originalEndScene(p);unsigned n=++sceneCalls;if(n==1||n%600==0)log("D3D EndScene count=%u hr=%08lx",n,hr);if(n==120||n==600)capture(p,n+100000);return hr;}
static HRESULT WINAPI device(IDirect3D9*p,UINT a,D3DDEVTYPE b,HWND w,DWORD flags,D3DPRESENT_PARAMETERS* params,IDirect3DDevice9** out){logParams("D3D CreateDevice ORIGINAL",params);windowParams(params);HRESULT hr=originalDevice(p,a,b,w,flags,params,out);log("D3D CreateDevice forced windowed native format preserved hr=%08lx",hr);logParams("D3D CreateDevice RETURNED",params);if(SUCCEEDED(hr)&&out&&*out){void** table=*(void***)*out;if(!patchComSlot(table,16,(void*)reset,(void**)&originalReset)||!patchComSlot(table,17,(void*)present,(void**)&originalPresent)||!patchComSlot(table,42,(void*)endScene,(void**)&originalEndScene)){(*out)->Release();*out=0;return E_FAIL;}}return hr;}
static IDirect3D9* WINAPI d3d(UINT v){IDirect3D9* p=originalD3D(v);log("Direct3DCreate9 returned=%p",p);if(p&&!patchComSlot(*(void***)p,16,(void*)device,(void**)&originalDevice)){p->Release();return 0;}return p;}
static void refreshDeviceHooks(){if(selfTest||!originalPresent)return;void* dev=*(void**)0x69e694;if(!dev)return;void** table=*(void***)dev;
    if(table[16]==(void*)originalReset)patchComSlot(table,16,(void*)reset,(void**)&originalReset);
    if(table[17]==(void*)originalPresent)patchComSlot(table,17,(void*)present,(void**)&originalPresent);
    if(table[42]==(void*)originalEndScene)patchComSlot(table,42,(void*)endScene,(void**)&originalEndScene);
}
static DWORD (WINAPI* originalInput)(DWORD,XINPUT_STATE*);
static DWORD WINAPI input(DWORD i,XINPUT_STATE*s){
    if(humanInputMode)return humanInput(i,s);
    DWORD r=ERROR_DEVICE_NOT_CONNECTED;
    if(inputBot){
        if(i<=(playBot?1u:0u)&&s){
            ULONGLONG now=GetTickCount64();if(!inputBotStarted)inputBotStarted=now;ULONGLONG elapsed=now-inputBotStarted;
            ZeroMemory(s,sizeof(*s));s->dwPacketNumber=(DWORD)(elapsed/16+1);
            DWORD flow[4]={0};SIZE_T got=0;void* p=*(void**)0x69e72c;if(p)ReadProcessMemory(GetCurrentProcess(),p,flow,sizeof(flow),&got);
            DWORD* movie=*(DWORD**)0x6a9980;bool frontendMovie=flow[2]==16&&movie&&movie[1]==1;
            if(i==0&&elapsed>=4000&&(flow[2]<8||frontendMovie)&&(elapsed-4000)%2500<120)s->Gamepad.wButtons=((elapsed-4000)/2500)%2?XINPUT_GAMEPAD_START:XINPUT_GAMEPAD_A;
            if(flow[2]!=128)playBotStarted=0;
            else if(playBot){
                if(!playBotStarted)playBotStarted=now;ULONGLONG gameTime=now-playBotStarted;
                if(!networkRole&&i==1&&gameTime>=15000&&(gameTime-15000)%3000<150&&(LONG)*(DWORD*)0x583518<0)s->Gamepad.wButtons=XINPUT_GAMEPAD_START;
                if(!actionBot&&gameTime>=17000){
                    unsigned phase=(unsigned)((gameTime-17000)/2500)%4;
                    SHORT sign=i?-(SHORT)25000:(SHORT)25000;
                    if(phase==0)s->Gamepad.sThumbLX=sign;
                    if(phase==1)s->Gamepad.sThumbLY=sign;
                    if(phase==2)s->Gamepad.sThumbLX=-sign;
                    if(phase==3)s->Gamepad.sThumbLY=-sign;
                    if(gameTime%1200<100)s->Gamepad.wButtons|=XINPUT_GAMEPAD_X;
                }
            }
            r=ERROR_SUCCESS;
        }
    }else r=originalInput(i,s);
    unsigned n=++inputCalls;if(n<=4||n%6000==0)log("XInputGetState call=%u slot=%lu result=%lu packet=%lu bot=%u",n,i,r,r==0&&s?s->dwPacketNumber:0,inputBot?1:0);return r;
}
static DWORD (WINAPI* originalCaps)(DWORD,DWORD,XINPUT_CAPABILITIES*);
static DWORD WINAPI caps(DWORD i,DWORD flags,XINPUT_CAPABILITIES* c){if(humanInputMode)return humanCaps(i,flags,c);if(!inputBot)return originalCaps(i,flags,c);if(i>(playBot?1u:0u)||!c)return ERROR_DEVICE_NOT_CONNECTED;ZeroMemory(c,sizeof(*c));c->Type=XINPUT_DEVTYPE_GAMEPAD;c->SubType=XINPUT_DEVSUBTYPE_GAMEPAD;c->Gamepad.wButtons=0xf3ff;c->Gamepad.bLeftTrigger=255;c->Gamepad.bRightTrigger=255;c->Gamepad.sThumbLX=c->Gamepad.sThumbLY=c->Gamepad.sThumbRX=c->Gamepad.sThumbRY=32767;return ERROR_SUCCESS;}
static DWORD (WINAPI* originalTime)();
static DWORD WINAPI timeHook(){DWORD t=originalTime();unsigned n=++timeCalls;if(n==1||n%600==0)refreshDeviceHooks();if(n==1||n%6000==0){log("timeGetTime call=%u raw_ms=%lu",n,t);if(!selfTest){DWORD state[4]={0};SIZE_T read=0;void* flow=*(void**)0x69e72c;BOOL ok=flow&&ReadProcessMemory(GetCurrentProcess(),flow,state,sizeof(state),&read);log("scheduler observe flow=%p readable=%u current=%lu pending=%lu sim_mode=%lu fixed_fps=%g rng=%08lx",flow,ok&&read==sizeof(state),state[2],state[3],*(volatile DWORD*)0x69ed24,*(volatile float*)0x591df8,*(volatile DWORD*)0x58cf24);void* dev=*(void**)0x69e694;DWORD table=0,pres=0;ReadProcessMemory(GetCurrentProcess(),dev,&table,4,&read);if(table)ReadProcessMemory(GetCurrentProcess(),(void*)(table+17*4),&pres,4,&read);log("render observe enabled=%u phase=%lu frame=%lu begin_mask=%08lx end_mask=%08lx device=%p present_slot=%08lx expected=%p",*(volatile BYTE*)0x69ecff,*(volatile DWORD*)0x694b98,*(volatile DWORD*)0x694ba0,*(volatile DWORD*)0x591db8,*(volatile DWORD*)0x591dd8,dev,pres,present);}}return networkInputClock?10000+(DWORD)((ULONGLONG)networkTick*1000/60):t;}
static FARPROC (WINAPI* originalProc)(HMODULE,LPCSTR);
static DWORD (WINAPI* originalRumble)(DWORD,XINPUT_VIBRATION*);
static DWORD WINAPI rumble(DWORD index,XINPUT_VIBRATION* vibration){if(inputBot||humanInputFixture||networkRole)return ERROR_SUCCESS;return originalRumble(index,vibration);}
static FARPROC WINAPI proc(HMODULE h,LPCSTR n){if((ULONG_PTR)n>0xffff){if(!strcmp(n,"XInputGetState")){originalInput=(decltype(originalInput))originalProc(h,n);log("XInputGetState resolved/log wrapper installed");return originalInput?(FARPROC)input:0;}if(!strcmp(n,"XInputSetState")){originalRumble=(decltype(originalRumble))originalProc(h,n);return originalRumble?(FARPROC)rumble:0;}if(!strcmp(n,"XInputGetCapabilities")){originalCaps=(decltype(originalCaps))originalProc(h,n);return originalCaps?(FARPROC)caps:0;}if(!strncmp(n,"Reg",3)){log("BLOCK dynamic registry API %s",n);SetLastError(ERROR_ACCESS_DENIED);return 0;}}return originalProc(h,n);}

struct Hook {const char* name;void* replacement;void** original;bool found;};
#define H(n,f) {n,(void*)f,0,false}
#define O(n,f,o) {n,(void*)f,(void**)&o,false}
static Hook hooks[]={H("SHGetFolderPathW",folder),H("RegCreateKeyW",regCreate),H("RegOpenKeyExA",regOpenA),H("RegSetValueExW",regSet),H("RegQueryValueExW",regQuery),H("RegQueryValueExA",regQueryA),H("RegCloseKey",regClose),O("CreateFileW",createW,originalCreateW),O("GetFileAttributesW",attributesHook,originalAttributes),O("ExpandEnvironmentStringsW",expandHook,originalExpand),O("CreateFileA",createA,originalCreateA),O("WriteFile",write,originalWrite),O("_wfopen",wfopenHook,originalWfopen),O("fopen",fopenHook,originalFopen),O("_wmkdir",mkdirHook,originalMkdir),O("_wremove",removeHook,originalRemove),H("ShellExecuteW",shell),H("ClipCursor",clip),H("ShowCursor",cursor),O("CreateWindowExA",window,originalWindow),O("ShowWindow",show,originalShow),O("Direct3DCreate9",d3d,originalD3D),O("GetProcAddress",proc,originalProc),O("timeGetTime",timeHook,originalTime)};
static bool __cdecl testEnabled(const char* path,bool fallback){if(path&&!strcmp(path,"/user/test/enable")){log("AUTOTEST enable request observed, returning true");return true;}return ((bool(__cdecl*)(const char*,bool))0x43754f)(path,fallback);}
static void* __stdcall testNameExists(const char* path){if(path&&!strcmp(path,"/user/test/testname")){log("AUTOTEST name existence request observed");return (void*)1;}return ((void*(__stdcall*)(const char*))0x437d6b)(path);}
static ULONG_PTR __cdecl testNameValue(const char* path,ULONG_PTR fallback){if(path&&!strcmp(path,"/user/test/testname")){log("AUTOTEST selected shipped %s function",autoTestName);return (ULONG_PTR)autoTestName;}if(path&&!strcmp(path,"/user/test/area_name")){log("AUTOTEST selected area %s",networkArea);return (ULONG_PTR)networkArea;}if(path&&!strcmp(path,"/user/test/sidekick_name")){static const char sidekick[]="rhino";log("AUTOTEST selected sidekick %s",sidekick);return (ULONG_PTR)sidekick;}return ((ULONG_PTR(__cdecl*)(const char*,ULONG_PTR))0x437515)(path,fallback);}
typedef void (__thiscall* FlowNativeFn)(void*,unsigned);
static bool readMemory(const void* address,void* output,SIZE_T bytes){SIZE_T got=0;return ReadProcessMemory(GetCurrentProcess(),address,output,bytes,&got)&&got==bytes;}
static DWORD readWord(ULONG_PTR address){DWORD word=0;readMemory((void*)address,&word,sizeof(word));return word;}
static float readFloat(ULONG_PTR address){float value=0;readMemory((void*)address,&value,sizeof(value));return value;}
static HKL inputActualLayout=0,inputCanonicalLayout=0;
[[noreturn]] static void startupInputFailure(const char* reason){
    networkFailed=true;log("NET FAIL input startup rejected reason=%s; no key substituted; owned game exits86",reason);
    ExitProcess(86);
}
// Players without the US layout loaded (tester 2026-10-23: only Russian/other layouts) used to be refused at startup.
// The key-name initializer then gets this marker instead of a real layout, and its two translation calls
// (52671F MapVirtualKeyExW, 52673A ToUnicodeEx) are answered from a table generated from the Windows US layout
// (qa/make_us_keyboard_table.py). No keyboard layout is loaded, activated or changed on the player's PC.
struct UsKey{WORD scan;BYTE vk;wchar_t ch;};
static const UsKey usKeyboardTable[]={
#include "us_keyboard_table.inc"
};
static const HKL builtinUsLayout=(HKL)(ULONG_PTR)0xF00D0409u;
static const UsKey* usKey(UINT scan){for(const auto& k:usKeyboardTable)if(k.scan==scan)return &k;return 0;}
static UINT WINAPI keyNameMapVirtualKey(UINT code,UINT type,HKL layout){
    if(layout!=builtinUsLayout)return MapVirtualKeyExW(code,type,layout);
    if(type!=3)return 0;const UsKey* k=usKey(code);return k?k->vk:0;
}
static int WINAPI keyNameToUnicode(UINT vk,UINT scan,const BYTE* state,LPWSTR out,int size,UINT flags,HKL layout){
    if(layout!=builtinUsLayout)return ToUnicodeEx(vk,scan,state,out,size,flags,layout);
    // The initializer always asks with an all-zero key state, as the table was generated.
    for(const auto& k:usKeyboardTable)if(k.vk==vk&&(UINT)(k.scan&0xffu)==(scan&0xffu)&&k.ch){if(size<1||!out)return 0;out[0]=k.ch;if(size>1)out[1]=0;return 1;}
    return 0;
}
static HKL WINAPI canonicalKeyboardLayout(DWORD thread){
    // Only52653C is patched. Its saved HKL is consumed by the initializer's
    // MapVirtualKeyEx/ToUnicodeEx internal-name generation; native DirectInput
    // display names at descriptor+54 are obtained independently and preserved.
    inputActualLayout=GetKeyboardLayout(thread);
    if(thread!=0)startupInputFailure("unexpected keyboard initializer thread argument");
    HKL loaded[256]={};int count=GetKeyboardLayoutList(256,loaded);
    if(count<=0||count>256)startupInputFailure("cannot enumerate already loaded keyboard layouts");
    std::uint32_t handles[256]={};for(int i=0;i<count;i++)handles[i]=(std::uint32_t)(ULONG_PTR)loaded[i];
    std::uint32_t selected=0;
    HKL canonical=builtinUsLayout;
    // Tests force the table on a PC that has the US layout (COOP_PROBE_BUILTIN_US_KEYS=1).
    wchar_t force[4]={};bool forceBuiltin=GetEnvironmentVariableW(L"COOP_PROBE_BUILTIN_US_KEYS",force,4)==1&&force[0]==L'1';
    if(!forceBuiltin&&coop::SelectLoadedUSLayout(handles,(std::size_t)count,selected)){
        canonical=(HKL)(ULONG_PTR)selected;
        // Wherever the real US layout exists, check the built-in table against it.
        unsigned mismatches=0;const BYTE zero[256]={};
        for(const auto& k:usKeyboardTable){UINT vk=MapVirtualKeyExW(k.scan,3,canonical);wchar_t out[4]={};int got=vk?ToUnicodeEx(vk,k.scan&0xffu,zero,out,4,0,canonical):0;
            if(vk!=k.vk||(got==1?out[0]:0)!=k.ch){if(mismatches++<8)log("INPUT US_TABLE_MISMATCH scan=%04x table_vk=%02x real_vk=%02x table_ch=%04x real_ch=%04x",k.scan,k.vk,vk,k.ch,got==1?out[0]:0);}}
        log("INPUT US_TABLE_CHECK rows=%u mismatches=%u",(unsigned)(sizeof(usKeyboardTable)/sizeof(usKeyboardTable[0])),mismatches);
    }
    else log("INPUT KEY_LAYOUT %s (count=%d); using the built-in US key table; no layout activated or installed",forceBuiltin?"built-in table forced by test":"no US layout loaded",count);
    struct Translation{UINT scan;wchar_t expected;};
    const Translation translations[]={{0x11,L'w'},{0x1e,L'a'},{0x1f,L's'},{0x20,L'd'},
        {0x15,L'y'},{0x2c,L'z'},{2,L'1'},{0x0c,L'-'},{0x27,L';'},{0x28,L'\''},
        {0x29,L'`'},{0x2b,L'\\'},{0x1a,L'['},{0x1b,L']'},{0x33,L','},{0x34,L'.'},{0x35,L'/'}};
    const BYTE state[256]={};
    for(const auto& test:translations){
        UINT vk=keyNameMapVirtualKey(test.scan,3,canonical);wchar_t output[4]={};
        if(!vk||keyNameToUnicode(vk,test.scan,state,output,4,0,canonical)!=1||output[0]!=test.expected)
            startupInputFailure("already loaded US handle failed canonical physical-key validation");
    }
    inputCanonicalLayout=canonical;
    log("INPUT KEY_LAYOUT actual=%p canonical=%p validated_physical_keys=%u internal_names_only=1 display_names_native=1",inputActualLayout,canonical,(unsigned)(sizeof(translations)/sizeof(translations[0])));
    return canonical;
}
static bool readNativeKeyName(DWORD address,coop::KeyName& out){
    return coop::ReadNativeKeyName(address,[](std::uint32_t from,void* to,std::size_t bytes){
        return readMemory((const void*)(ULONG_PTR)from,to,bytes);
    },out);
}
static void formatKeyName(const coop::KeyName& name,char (&out)[1281]){
    unsigned at=0;for(std::size_t i=0;i<name.length;i++)at+=(unsigned)sprintf(out+at,"%04x%s",(unsigned)name.text[i],i+1<name.length?"-":"");out[at]=0;
}
static DWORD __fastcall guardedKeyLookup(void* provider,void*,DWORD requested){
    DWORD table=0,functions[6]={};
    if(!provider||!readMemory(provider,&table,sizeof(table))||!table||
       !readMemory((void*)(table+0x10),functions,sizeof(functions)))startupInputFailure("unreadable input provider vtable");
    // Other providers retain their native ABI/semantics. The crash path is
    // specifically the keyboard getter5262F0 and global descriptor table.
    if(functions[1]!=0x5262f0)return ((DWORD(__thiscall*)(void*,DWORD))0x525c30)(provider,requested);
    if(functions[0]!=0x5262e0||functions[5]!=0x526350)startupInputFailure("keyboard provider ABI guard");
    DWORD count=0,descriptors=0;
    if(!readMemory((void*)0x5955cc,&count,4)||!readMemory((void*)0x5955c8,&descriptors,4)||!descriptors)
        startupInputFailure("unreadable keyboard descriptor table");
    coop::KeyName target;
    if(!readNativeKeyName(requested,target)){
        log("INPUT KEY_LOOKUP count=%lu request_utf16=malformed actual_layout=%p canonical_layout=%p",count,GetKeyboardLayout(0),inputCanonicalLayout);
        startupInputFailure("malformed keyboard binding name");
    }
    char encoded[1281]={};formatKeyName(target,encoded);
    log("INPUT KEY_LOOKUP count=%lu request_utf16=%s actual_layout=%p canonical_layout=%p",count,encoded,GetKeyboardLayout(0),inputCanonicalLayout);
    if(!inputCanonicalLayout)startupInputFailure("canonical keyboard initializer was not observed");
    std::uint32_t found=0xffffffffu,last=0xffffffffu;
    auto result=coop::FindKeyName(count,target,[&](std::uint32_t index,coop::KeyName& name){
        last=index;std::uint64_t address=std::uint64_t(descriptors)+std::uint64_t(index)*0xa4+4;
        return address<=0xfffffff0ull&&readNativeKeyName((DWORD)address,name);
    },found);
    if(result!=coop::KeyLookup::Found){
        log("INPUT KEY_LOOKUP_REJECT count=%lu last_index=%lu request_utf16=%s status=%s",count,(DWORD)last,encoded,result==coop::KeyLookup::Missing?"missing":"malformed");
        startupInputFailure("keyboard binding cannot be resolved within the descriptor count");
    }
    log("INPUT KEY_LOOKUP_FOUND index=%lu count=%lu",(DWORD)found,count);
    return found;
}
static void selectVirtualPad(){
    if(virtualPadSelected||!originalCaps)return;virtualPadSelected=true;virtualPadIndex=~0u;
    for(int index=3;index>=0;index--){if(humanInputFixture&&index==0)continue;XINPUT_CAPABILITIES capabilities={};if(originalCaps((DWORD)index,0,&capabilities)==ERROR_DEVICE_NOT_CONNECTED){virtualPadIndex=(DWORD)index;break;}}
    if(virtualPadIndex==~0u){networkFailed=true;log("NET FAIL no unused XInput slot for remote input provider");}else log("HUMAN reserved remote XInput index=%lu fixture=%u",virtualPadIndex,humanInputFixture);
}
static DWORD WINAPI humanCaps(DWORD index,DWORD flags,XINPUT_CAPABILITIES* capabilities){
    selectVirtualPad();if(!capabilities)return ERROR_BAD_ARGUMENTS;
    if(humanKeyboardFixture&&index!=virtualPadIndex)return ERROR_DEVICE_NOT_CONNECTED;
    if(index!=virtualPadIndex&&!(humanInputFixture&&index==0))return originalCaps(index,flags,capabilities);
    ZeroMemory(capabilities,sizeof(*capabilities));capabilities->Type=XINPUT_DEVTYPE_GAMEPAD;capabilities->SubType=XINPUT_DEVSUBTYPE_GAMEPAD;capabilities->Gamepad.wButtons=0xf3ff;capabilities->Gamepad.bLeftTrigger=capabilities->Gamepad.bRightTrigger=255;capabilities->Gamepad.sThumbLX=capabilities->Gamepad.sThumbLY=capabilities->Gamepad.sThumbRX=capabilities->Gamepad.sThumbRY=32767;return ERROR_SUCCESS;
}
static bool captureGameMode(DWORD (&sample)[3]);
static coop::Actions physicalFixtureActions(bool analog){
    if(campaignMode&&campaignFixture){coop::Actions actions={};
        // Retail New Game shows tutorial hints as native pause mode 1 with a single OK.
        // Host-only accept pulse; lockstep input delivers it to both seats on the same tick.
        DWORD mode[3]={};if(captureGameMode(mode)&&mode[0]==1){
            if(tutorialUiTest&&readWord(readWord(0x6a9940)+0x4e4)==14&&!tutorialClosed){
                if(tutorialFixtureStart==~0u)tutorialFixtureStart=networkTick;
                unsigned age=networkTick-tutorialFixtureStart;
                if(networkRole==2&&age<100&&age%20<2)actions[10]=1.0f;
            }else if(networkRole==1&&networkTick%60<2)actions[10]=1.0f;
            return actions;}
        // Results screen after a level end (scene 8, endlevel.fen): the host bot accepts it like a player (level-end-443 sat there).
        if(readWord(readWord(0x6a9940)+0x4e4)==8){if(networkRole==1&&networkTick%60<2)actions[10]=1.0f;return actions;}
        unsigned direction=((networkTick/120+networkRole)%2)?18:19;actions[direction]=0.65f;return actions;}
    if(swapFixture&&networkRole==1&&((networkTick>=600&&networkTick<650)||(networkTick>=1500&&networkTick<1550))){coop::Actions actions={};actions[networkSwapAction]=1.0f;return actions;}
    // Inject before native binding/filtering, so the native-axis oracle still
    // compares the actual captured provider state with its ordinary replay.
    if(networkPauseFixture&&networkTick>=234&&networkTick<=430){
        coop::Actions actions={};
        if(networkRole==1){if(networkTick==240)actions[5]=1;if(networkTick==400)actions[11]=1;}
        return actions;
    }
    coop::Actions actions=networkBotActions(networkRole==1?0:1,networkTick);
    if(analog&&networkTick>=180&&networkTick<500){static const float samples[]={0.05f,0.1f,0.149f,0.15f,0.151f,0.25f,0.5f,0.85f};actions[networkRole==1?19:18]=samples[(networkTick-180)/40];}
    return actions;
}
static DWORD WINAPI humanInput(DWORD index,XINPUT_STATE* state){
    selectVirtualPad();if(!state)return ERROR_BAD_ARGUMENTS;
    if(humanKeyboardFixture&&index!=virtualPadIndex)return ERROR_DEVICE_NOT_CONNECTED;
    if(index!=virtualPadIndex&&!(humanInputFixture&&index==0))return originalInput(index,state);
    // A real disconnected API may leave its caller's previous state untouched.
    if(humanInputFixture&&humanDisconnectFixture&&index==0&&networkTick>=901&&networkTick<932)return ERROR_DEVICE_NOT_CONNECTED;
    ZeroMemory(state,sizeof(*state));state->dwPacketNumber=(DWORD)(GetTickCount64()/16+1);
    if(humanInputFixture&&index==0){
        DWORD flow=readWord(0x69e72c),current=readWord(flow+8);ULONGLONG now=GetTickCount64();if(!inputBotStarted)inputBotStarted=now;ULONGLONG elapsed=now-inputBotStarted;DWORD movie=readWord(0x6a9980);
        if(elapsed>=4000&&(current<8||(!campaignMode&&current==16&&readWord(movie+4)==1))&&(elapsed-4000)%2500<120)state->Gamepad.wButtons=((elapsed-4000)/2500)%2?XINPUT_GAMEPAD_START:XINPUT_GAMEPAD_A;
        if(current==128){
            coop::Actions actions=physicalFixtureActions(true);state->dwPacketNumber=networkTick+1;
            if(actions[5]>0.5f)state->Gamepad.wButtons|=XINPUT_GAMEPAD_START;
            if(actions[10]>0.5f)state->Gamepad.wButtons|=XINPUT_GAMEPAD_A;if(actions[11]>0.5f)state->Gamepad.wButtons|=XINPUT_GAMEPAD_B;if(actions[12]>0.5f)state->Gamepad.wButtons|=XINPUT_GAMEPAD_X;if(actions[13]>0.5f)state->Gamepad.wButtons|=XINPUT_GAMEPAD_Y;
            state->Gamepad.sThumbLX=(SHORT)((actions[19]-actions[18])*32767);state->Gamepad.sThumbLY=(SHORT)((actions[16]-actions[17])*32767);
        }
    }
    return ERROR_SUCCESS;
}
static bool localDeviceConnected(){DWORD wrapper=readWord(0x595390);return wrapper&&((bool(__thiscall*)(void*))0x526910)((void*)wrapper);}
static void __cdecl keyboardPollFixture(){
    ((void(__cdecl*)())0x526240)();
    // This buffer belongs only to this game process. No OS keyboard input is sent.
    memset((void*)0x595470,0,256);DWORD keyboard=readWord(0x595570),wrapper=readWord(0x595390);
    if(!keyboard||!wrapper)return;if(readWord(readWord(keyboard)+0x24)!=0x526350){networkFailed=true;log("NET FAIL keyboard fixture provider guard");return;}
    if(readWord(wrapper+4)!=keyboard){assignNativeDevice(0,keyboard);wrapper=readWord(0x595390);}
    if(readWord(wrapper)<28)return;coop::Actions actions={};DWORD flow=readWord(0x69e72c),current=readWord(flow+8),movie=readWord(0x6a9980);ULONGLONG now=GetTickCount64();if(!inputBotStarted)inputBotStarted=now;ULONGLONG elapsed=now-inputBotStarted;
    if(current==128)actions=physicalFixtureActions(false);
    else if(elapsed>=4000&&(current<8||(!campaignMode&&current==16&&readWord(movie+4)==1))&&(elapsed-4000)%2500<120){actions[5]=1;actions[10]=1;}
    unsigned pressed=0;
    for(unsigned action=0;action<28;action++)if(actions[action]>0.5f){
        DWORD head=readWord(wrapper+0x10)+action*8,node=readWord(head+4);bool found=false;
        for(unsigned traversed=0;node&&node!=head&&traversed<64;traversed++,node=readWord(node+4))if(readWord(node+8)==keyboard){DWORD key=readWord(node+12),count=readWord(0x5955cc),descriptors=readWord(0x5955c8);if(key>=count||key>255||!descriptors)continue;DWORD scan=readWord(descriptors+key*0xa4);if(scan<256){*((BYTE*)0x595470+scan)=0x80;found=true;++pressed;break;}}
        if(!found&&current==128){networkFailed=true;log("NET FAIL keyboard fixture missing binding action=%u",action);return;}
    }
    if(current==128&&networkTick%120==0)log("HUMAN KEYBOARD_FIXTURE tick=%u native_provider=%08lx keys_pressed=%u",networkTick,keyboard,pressed);
}
static coop::Actions captureLocalActions(){
    coop::Actions actions={};DWORD wrapper=readWord(0x595390),mapped=readWord(wrapper+0x3c);
    if(!localDeviceConnected()){if(humanInputFixture&&networkTick>=901&&networkTick<932)log("HUMAN DISCONNECT_NEUTRAL tick=%u",networkTick);return actions;}
    if(!wrapper||!mapped||readWord(wrapper)<28||!readMemory((void*)mapped,actions.data(),sizeof(float)*28)){networkFailed=true;log("NET FAIL local mapped-input capture");return actions;}
    for(float& value:actions){if(!std::isfinite(value)){networkFailed=true;log("NET FAIL nonfinite physical input value");value=0;}else if(value<0)value=0;else if(value>1)value=1;}
    return actions;
}
static DWORD __cdecl randomObserved(DWORD* state,DWORD limit,DWORD caller,DWORD outerCaller){
    // Particle systems (class at 575434, e.g. effects/sandstorm: timer 4D14D8, spawn 4D1262) advance with the frame's
    // real time outside the lockstep tick; they are visual only and get the per-PC ambient generator too
    // (egypt2-bad-net-472: a sandstorm emitter drew from the shared generator one tick apart on the two PCs).
    // Draw-list items (4D266A -> 4D26E5: random angle/size of a decal mark, the list of the Transylvania 3 fix) too
    // (full-campaign-487 diverged in Nepal 3 on these draws).
    bool ambient=networkLockstep()&&networkAmbientSplit&&state==(DWORD*)0x58cf24&&(outerCaller==0x49115a||outerCaller==0x49233f||(outerCaller>=0x4d1262&&outerCaller<0x4d1a00)||
        caller==0x4d26f5||outerCaller==0x4d2707||outerCaller==0x4d2718);
    if(ambient)state=&networkAmbientRng;
    bool tracked=networkLockstep()&&networkReady&&!networkFailed&&!networkComplete&&(state==(DWORD*)0x58cf24||(networkInSimulation&&GetCurrentThreadId()==networkSimulationThread));
    DWORD old=tracked&&limit?*state:0,result=0;
    __asm {
        push edi
        mov edi,state
        push limit
        mov eax,0x4318f5
        call eax
        add esp,4
        mov result,eax
        pop edi
    }
    if(probeDiagnostics&&tracked&&InterlockedIncrement(&networkRngCalls)<=120000){DWORD actor=readWord(0x69e9f8),header=readWord(actor+8),serial=header?readWord(header+8):~0u;log("NET RNG call=%ld tick=%u simulation=%u thread=%lu caller=%08lx outer=%08lx bound=%lu old=%08lx new=%08lx result=%lu common=%u actor=%lu state=%p",networkRngCalls,networkTick,networkInSimulation&&GetCurrentThreadId()==networkSimulationThread,GetCurrentThreadId(),caller,outerCaller,limit,old,limit?*state:0,result,state==(DWORD*)0x58cf24,serial,state);}
    return result;
}
static void __declspec(naked) randomEntry(){
    __asm {
        mov eax,esp
        mov ecx,dword ptr [eax]
        xor edx,edx
        cmp ecx,0x431931
        jne no_outer
        mov edx,dword ptr [eax+12]
        cmp edx,0x431967
        jne no_outer
        mov edx,dword ptr [eax+24]
    no_outer:
        push edx
        push ecx
        push dword ptr [eax+4]
        push edi
        call randomObserved
        add esp,16
        ret
    }
}
static unsigned inputHash(const coop::Actions& actions){unsigned hash=2166136261u;const BYTE* bytes=(const BYTE*)actions.data();for(unsigned i=0;i<sizeof(float)*28;i++)hash=(hash^bytes[i])*16777619u;return hash;}
static void mappedAxes(const coop::Actions& actions,float* axes){
    // Native401602..401762 filters each direction before subtracting its opposite.
    float filtered[8]={};float deadzone=readFloat(0x55afa8);
    for(unsigned i=0;i<8;i++)filtered[i]=actions[16+i]<deadzone?0.0f:actions[16+i];
    axes[0]=filtered[3]-filtered[2];axes[1]=filtered[0]-filtered[1];axes[2]=filtered[7]-filtered[6];axes[3]=filtered[4]-filtered[5];
}
static void verifyNativeAxes(const coop::Actions& actions){
    if(!humanInputFixture||!localDeviceConnected())return;
    float expected[4]={0},native[4]={0};mappedAxes(actions,expected);
    if(!readMemory((void*)(0x691170+0xe4),native,sizeof(native))){networkFailed=true;log("NET FAIL native input oracle could not read axes");return;}
    for(unsigned i=0;i<4;i++)if(!std::isfinite(native[i])||fabsf(native[i]-expected[i])>0.000001f){networkFailed=true;log("NET FAIL native input oracle tick=%u axis=%u native=%.9g replay=%.9g",networkTick,i,native[i],expected[i]);return;}
    if(networkTick<600&&networkTick%30==0)log("HUMAN AXIS_ORACLE tick=%u mapped=%.9g,%.9g,%.9g,%.9g native=%.9g,%.9g replay=%.9g,%.9g",networkTick,actions[16],actions[17],actions[18],actions[19],native[0],native[1],expected[0],expected[1]);
}
static void applyNetworkActions(unsigned player,const coop::Actions& actions){
    static bool previous[2][28]={0};static float duration[2][28]={0},previousAxes[2][4]={0};
    BYTE* context=(BYTE*)(0x691170+player*0x11c);DWORD* states=(DWORD*)(context+4);float* durations=(float*)(context+0x74);
    bool anyAction=false;
    for(unsigned action=0;action<28;action++){bool down=actions[action]>0.5f;states[action]=(down?1u:0u)|(down!=previous[player][action]?2u:0u);duration[player][action]=down==previous[player][action]?duration[player][action]+1.0f/60.0f:0;durations[action]=duration[player][action];previous[player][action]=down;if(states[action])anyAction=true;}
    if(networkInputClock&&anyAction)*(DWORD*)(context+0x110)=10000+(DWORD)((ULONGLONG)networkTick*1000/60);
    float axes[4]={0};mappedAxes(actions,axes);
    memcpy(context+0xf4,previousAxes[player],16);memcpy(context+0xe4,axes,16);memcpy(previousAxes[player],axes,16);*(float*)(context+0x10c)=1.0f/60.0f;
    DWORD wrapper=readWord(0x595390+player*4),mapped=readWord(wrapper+0x3c);
    if(wrapper&&mapped&&readWord(wrapper)>=28)memcpy((void*)mapped,actions.data(),sizeof(float)*28);
    networkInputHash[player]=inputHash(actions);
}
static coop::Actions networkBotActions(unsigned player,unsigned tick){
    coop::Actions actions={};
    if(tick==90||tick==91)actions[10]=1.0f;
    if(tick<600)return actions;
    if((tick+player*7)%18<6)actions[12]=1.0f;
    if((tick+player*13)%300<3)actions[10]=1.0f;
    if((tick+player*17)%120<4)actions[13]=1.0f;
    DWORD manager=readWord(0x6a9900),self=readWord(manager+0x12c+player*12),selfTransform=readWord(self+0x1d0);
    float position[3]={0};if(!self||!selfTransform||!readMemory((void*)selfTransform,position,12))return actions;
    float closest=1e20f,target[3]={0};DWORD node=readWord(manager+0x34);
    for(unsigned visited=0;node&&visited<128;visited++){
        DWORD handle[3]={0};if(!readMemory((void*)node,handle,sizeof(handle)))break;node=handle[0];DWORD actor=handle[1];if(!actor||handle[2]<2||handle[2]==~0u)continue;
        DWORD header=readWord(actor+8),table=readWord(actor);if(!header||readWord(header+8)!=handle[2]||readWord(table+0x2f4)!=0x45a6c9||readWord(table+0x21c)!=0x45119b||readFloat(actor+0x398)<=0)continue;
        BYTE teamFlag=0;readMemory((void*)(actor+0x304),&teamFlag,1);if(teamFlag)continue;
        float candidate[3]={0};DWORD transform=readWord(actor+0x1d0);if(!transform||!readMemory((void*)transform,candidate,12)||fabsf(candidate[2]-position[2])>4)continue;
        float dx=candidate[0]-position[0],dy=candidate[1]-position[1],distance=dx*dx+dy*dy;
        if(distance<closest){closest=distance;memcpy(target,candidate,12);}
    }
    if(closest>3.24f&&closest<1e18f){
        DWORD camera=readWord(readWord(0x6a9930)+0x6c);float eye[3]={0},look[3]={0},fx=1,fy=0;
        if(camera&&readMemory((void*)(camera+0x1c),eye,12)&&readMemory((void*)(camera+0x28),look,12)){
            float x=look[0]-eye[0],y=look[1]-eye[1],length=sqrtf(x*x+y*y);if(std::isfinite(length)&&length>0.001f){fx=x/length;fy=y/length;}
        }
        float length=sqrtf(closest),dx=(target[0]-position[0])/length,dy=(target[1]-position[1])/length;
        float x=(-fy*dx+fx*dy)*0.85f,y=(fx*dx+fy*dy)*0.85f;
        actions[x>=0?19:18]=fabsf(x);actions[y>=0?16:17]=fabsf(y);
    }
    return actions;
}
// Round-trip timing (real play logs): how long menu/hub exchanges and gameplay frames wait for the partner.
struct WaitStats{const char* name;unsigned count;double total,worst;void add(double ms){++count;total+=ms;if(ms>worst)worst=ms;if(count%600==0){log("NET LATENCY %s samples=%u average_ms=%.1f worst_ms=%.1f",name,count,total/count,worst);total=0;worst=0;count=0;}}};
static WaitStats phaseWaits={"phase",0,0,0},frameWaits={"frame",0,0,0};
static double milliseconds(){static LARGE_INTEGER frequency={};if(!frequency.QuadPart)QueryPerformanceFrequency(&frequency);LARGE_INTEGER now;QueryPerformanceCounter(&now);return now.QuadPart*1000.0/frequency.QuadPart;}
// Input delay in frames. The research scenario keeps 3 (its fixtures expect it); the campaign uses 6 (100 ms), so
// an internet round trip does not stall every frame (first two-PC test, 2026-10-24). Both seats must agree (hello).
static unsigned networkAgreedDelay=0;   // set before the first mission from the measured connection (0 = default)
static unsigned networkDelay(){return networkAgreedDelay?networkAgreedDelay:(campaignMode?6u:3u);}
// Round-trip samples (ms) from the non-blocking hub rounds.
static float hubRoundSamples[256];static unsigned hubRoundCount=0;
static unsigned measuredDelayFrames(){
    if(hubRoundCount<8)return 6;unsigned n=hubRoundCount<256?hubRoundCount:256;float sorted[256];memcpy(sorted,hubRoundSamples,n*sizeof(float));std::sort(sorted,sorted+n);
    // A hub round lasts about one round trip (each side answers when the other's update arrives): half of it each way.
    float p50=sorted[n/2],p90=sorted[(n*9)/10];unsigned frames=(unsigned)ceilf(p90/2.0f/16.67f)+2;frames=frames<6?6:frames>14?14:frames;
    log("NET HUB_ROUNDS samples=%u p50_ms=%.0f p90_ms=%.0f delay_frames=%u",n,p50,p90,frames);return frames;
}
static bool openNetworkConnection(){
    if(networkReady)return true;
    {
        coop::Digest build={},scenario={};const char* digest="ebd30047d7e58c95bf7dc4bd1fb4a5780e16aa472b61c25a8c421632662083db";
        for(unsigned i=0;i<32;i++){unsigned byte=0;sscanf(digest+i*2,"%2x",&byte);build[i]=(BYTE)byte;}
        if(networkContentVerified)build=networkContent;
        // Private scenario tag: Tokyo1/1, seed8466,60Hz,delay3,combat bot2.
        scenario[0]=12;scenario[1]=0x21;scenario[2]=0x12;scenario[3]=60;scenario[4]=(BYTE)networkDelay();scenario[5]=networkPathSync?1:0;scenario[6]=networkAmbientSplit?1:0;scenario[7]=humanInputMode?1:0;scenario[8]=humanInputFixture?1:0;
        memcpy(scenario.data()+12,&networkFrameLimit,sizeof(networkFrameLimit));
        unsigned areaHash=0;if(!campaignMode){areaHash=2166136261u;for(const char* c=networkArea;*c;c++)areaHash=(areaHash^(BYTE)*c)*16777619u;}memcpy(scenario.data()+16,&areaHash,sizeof(areaHash));
        scenario[20]=campaignMode?1:0;scenario[21]=campaignFixture?1:0;scenario[22]=campaignJoinPending?1:0;
        scenario[9]=networkTransitionFixture?1:0;
        scenario[10]=(BYTE)networkTransitionTarget;scenario[11]=networkPauseFixture?1:0;
        networkWire=new coop::InputWire;
        log("NET opening role=%u address=%s port=%u frame_limit=%u content_preflight=%u scenario=%s jitter_fixture=%u",networkRole,networkAddress,networkPort,networkFrameLimit,networkContentVerified,campaignMode?"retail-campaign":"research",networkJitterFixture);
        // A person's game meets its partner at the main menu whenever its own startup videos end (or are
        // skipped), so a human campaign waits up to 120s; it narrows to 30s once New Game is chosen.
        networkReady=networkWire->Open(networkRole==1,networkAddress,networkPort,build,scenario,networkDelay(),campaignMode&&!campaignFixture?120000:30000);
        if(!networkReady){networkFailed=true;log("NET FAIL opening %s",networkWire->Error().c_str());return false;}
        // Join at the hub (first time or a rejoin) connects while one side is already far into the
        // campaign and the other is a freshly started process still past its one-time startup menus;
        // give that side room to get there instead of the game hard-failing on the first exchange.
        // campaignJoinReady() narrows this back to the normal 30s once both are actively exchanging.
        if(campaignJoinPending){networkWire->SetTimeout(120000);log("NET widened phase timeout to 120s for the join-at-hub handshake");}
        log("NET READY role=%u",networkRole);
    }
    return true;
}
static void campaignHubPartnerReset();
// A joined partner left (lost connection or closed their game). The host keeps playing alone as an
// ordinary game and can link again with Host; the partner comes back like a new joiner, at the hub.
static void networkSessionReset();
static bool campaignPartnerLost(const char* why){
    if(!campaignJoined||networkRole!=1)return false;
    log("NET PARTNER_LOST %s; host continues alone and can Host again",why);
    networkWire->Close();networkReady=networkFailed=networkComplete=false;networkRole=0;networkDeferred=true;networkTick=0;networkInputClock=false;
    campaignJoinPending=campaignJoinSent=campaignJoinReceived=campaignJoined=false;campaignMission=campaignGameplay=false;campaignAutoTogether=campaignTogetherOffered=campaignTogetherDecided=campaignTogetherJoinerStart=false;networkAgreedDelay=0;hubRoundCount=0;
    campaignPhaseSequence=0;campaignLoadSequence=0;campaignCycle=0;campaignHubPartnerReset();networkSwapped=networkSwapLatched=false;networkSwapHeld=0;networkSessionReset();applyNetworkActions(0,{});applyNetworkActions(1,{});return true;
}
static void networkFinishScript(){
    if(!networkWire->Finish()){networkFailed=true;log("NET FAIL finish %s",networkWire->Error().c_str());return;}
    networkComplete=true;log("NET SCRIPT_COMPLETE frames=%u; peer completion confirmed; no co-op QA awarded",networkTick);
    if(networkCleanExit){applyNetworkActions(0,{});applyNetworkActions(1,{});if(!gameWindow||!PostMessageW(gameWindow,WM_CLOSE,0,0)){networkFailed=true;log("NET FAIL clean close request error=%lu",GetLastError());}else log("NET clean WM_CLOSE posted to owned game window");return;}
    wchar_t path[32768];_snwprintf(path,32768,L"%s\\probe-finished.signal",sandbox);HANDLE marker=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,0,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,0);
    if(marker==INVALID_HANDLE_VALUE){networkFailed=true;log("NET FAIL completion marker error=%lu",GetLastError());return;}
    const char message[]="Paired bot frame limit completed. Transport and state logs require review.\r\n";DWORD written=0;BOOL saved=WriteFile(marker,message,sizeof(message)-1,&written,0);FlushFileBuffers(marker);CloseHandle(marker);if(!saved||written!=sizeof(message)-1){networkFailed=true;log("NET FAIL completion marker write");}
    applyNetworkActions(0,{});applyNetworkActions(1,{});return;
}
// Cutscene (NIS) load gate. The game asks 4E09E1 (ECX = NIS manager 6A9984) whether a loaded NIS is ready before it
// starts one; the answer comes from each PC's own background loading, so the two games could start the same cutscene
// on different frames (tester session 2026-10-24: divergence at the end of Tokyo 1 while meeting_blackcat loaded; host
// on D:, joiner on C:). Each frame carries this PC's "NIS loaded" status in action 27 (reserved, never applied); during
// lockstep the game only hears "ready" once both PCs reported it for the same tick.
static const unsigned networkNisAction=27;
static bool networkNisAgreed=false;
// The agreed status lags by the input delay. Between cutscenes it reads "ready", so with a long delay and a fast load a
// PC could see the old "ready" after its own load began and start alone (level-chain-bad-net-470, delay 11: the Green
// Goblin intro began on one PC only). A PC starts a cutscene only once the agreed status has shown "not ready" after
// its own load began; both PCs then start on the tick the agreed status turns ready again.
static unsigned nisLocalLoadStart=0,nisAgreedNotReadyAt=0;
// Action 27 carries this PC's load status bits: 4 = cutscene loaded, 2 = level exit unlocked, 1 = next area loaded,
// 8 = an area change is waiting here (see areaChangeRun), 16 = the cutscene stream is done here (see streamDoneDecision).
static float readyBits(bool nis,bool unlocked,bool loaded,bool change,bool stop){return (nis?0.5f:0.0f)+(unlocked?0.25f:0.0f)+(loaded?0.125f:0.0f)+(change?0.0625f:0.0f)+(stop?0.03125f:0.0f);}
static unsigned readyBitsOf(float value){unsigned bits=(unsigned)(value*32.0f+0.5f);if(bits>31)return 0;
    return (bits&16?4u:0u)|(bits&8?2u:0u)|(bits&4?1u:0u)|(bits&2?8u:0u)|(bits&1?16u:0u);}
// Level exits (4D2301 at 4D23FD): the exit-unlocked script flag and the streamed next area finish at different
// moments on two PCs, so in co-op both games take the exit only from the agreed status of both.
static unsigned networkExitAgreed=0;
static bool localExitUnlocked(){DWORD script=readWord(0x69ed1c);return script&&(readWord(script+0x24)&6)==4;}
static bool localNextAreaLoaded(){DWORD areas=readWord(0x6a9974);if(!areas)return false;DWORD slot=readWord(areas+0x14);return readWord(areas+(slot==0?1:0)*4+4)!=0||readWord(0x69eda4)!=0;}
static unsigned __cdecl exitDecision(){   // 0 native, 1 exit still locked, 2 wait for the next area, 3 go
    if(!(campaignMode&&networkLockstep()&&networkInSimulation&&GetCurrentThreadId()==networkSimulationThread))return 0;
    {static unsigned calls=0;static unsigned last=~0u;if(last!=networkTick&&calls++<200){last=networkTick;log("NET EXIT_TRIGGER tick=%u agreed=%u local_unlocked=%d local_loaded=%d",networkTick,networkExitAgreed,localExitUnlocked(),localNextAreaLoaded());}}
    if(!(networkExitAgreed&2))return 1;if(!(networkExitAgreed&1)){static unsigned holds=0;if(holds++%120==0)log("NET EXIT_WAIT waiting for both games to load the next area tick=%u",networkTick);return 2;}
    return 3;
}
static void __declspec(naked) exitGateEntry(){
    __asm {
        call exitDecision
        cmp eax,1
        je exit_locked
        cmp eax,2
        je exit_wait
        cmp eax,3
        je exit_go
        mov eax,dword ptr ds:[0x69ed1c]
        mov eax,dword ptr [eax+0x24]
        push 0x4d2405
        ret
    exit_locked:
        push 0x4d240b
        ret
    exit_wait:
        push 0x4d2445
        ret
    exit_go:
        push 0x4d2435
        ret
    }
}
// Area changes (4E2C13, transition stage 2) are requested by level scripts that run outside the lockstep tick, so two
// PCs ask on different frames. In co-op the request is held, announced with bit 8 of action 27, and both games run
// it inside the simulation on the tick where both announcements have arrived.
static DWORD areaChangePending=0,areaChangeCaller=0;static bool areaChangeRunning=false,networkAreaChangeAgreed=false;static unsigned areaChangeQuietUntil=0;
static bool __cdecl nextAreaAllowed(DWORD caller,DWORD kind){
    if(areaChangeRunning)return true;
    if(!(campaignMode&&networkLockstep()&&networkReady&&!networkFailed&&!networkComplete))return true;
    DWORD flow=readWord(0x69e72c);if(!flow||readWord(flow+8)!=128)return true;
    if(!areaChangePending){areaChangePending=kind?kind:~0u;areaChangeCaller=caller;log("NET AREA_CHANGE requested tick=%u kind=%lu caller=%08lx in_sim=%d",networkTick,kind,caller,networkInSimulation);}
    return false;
}
// Cutscene end (4E12D4 on the NIS manager 6A9984): a cutscene counts as finished only when its stream object
// (+B4E4, state +C 0 or 8) is done playing, which happens in real time on each PC (level-chain-457: the Green Goblin
// intro ended at tick 8227 on one PC and 8229 on the other). Holding the stop itself crashed both games (458), so in
// mission gameplay the simulation reads the agreed "stream done" of both PCs instead (bit 16 of action 27).
static bool networkStreamDoneAgreed=true;
// A new partner starts a new lockstep session at tick 0 (rejoin-mission-490: the host still held the old session's
// shared state, so the first comparison failed).
static void networkSessionReset(){
    networkState=0;networkNisAgreed=false;nisLocalLoadStart=nisAgreedNotReadyAt=0;networkExitAgreed=0;
    areaChangePending=0;areaChangeCaller=0;networkAreaChangeAgreed=false;areaChangeQuietUntil=0;networkStreamDoneAgreed=true;
    campaignOverwriteReset();
    localMenuPointer={};appliedMenuPointer={};
}
static bool localStreamDone(){DWORD manager=readWord(0x6a9984);if(!manager)return true;DWORD stream=readWord(manager+0xb4e4);if(!stream)return true;DWORD state=readWord(stream+0xc);return state==0||state==8;}
static DWORD __cdecl streamDoneDecision(){   // 0 native, 1 done, 2 not done
    if(!(campaignMode&&networkLockstep()&&networkReady&&!networkFailed&&!networkComplete&&networkInSimulation&&GetCurrentThreadId()==networkSimulationThread))return 0;
    DWORD flow=readWord(0x69e72c);if(!flow||readWord(flow+8)!=128)return 0;
    bool local=localStreamDone();static bool lastLocal=true,lastAgreed=true;
    if(local!=lastLocal||networkStreamDoneAgreed!=lastAgreed){lastLocal=local;lastAgreed=networkStreamDoneAgreed;log("NET STREAM_DONE tick=%u here=%d agreed=%d",networkTick,local,networkStreamDoneAgreed);}
    return networkStreamDoneAgreed?1:2;
}
static void __declspec(naked) streamDoneEntry(){
    __asm {
        push ecx
        push edx
        push eax
        call streamDoneDecision
        mov edx,eax
        pop eax
        cmp edx,1
        je stream_done
        cmp edx,2
        je stream_busy
        pop edx
        pop ecx
        mov eax,dword ptr [eax+0xb4e4]
        push 0x4e12da
        ret
    stream_done:
        pop edx
        pop ecx
        mov eax,1
        ret
    stream_busy:
        pop edx
        pop ecx
        xor eax,eax
        ret
    }
}
static void areaChangeRun(unsigned tick){
    if(!areaChangePending||!networkAreaChangeAgreed||tick<areaChangeQuietUntil)return;
    if(!(networkExitAgreed&1)){static unsigned holds=0;if(holds++%120==0)log("NET EXIT_WAIT area change waits for both games to load the next area tick=%u",tick);return;}
    DWORD kind=areaChangePending==~0u?0:areaChangePending;areaChangePending=0;areaChangeQuietUntil=tick+40;areaChangeRunning=true;
    __asm{
        push esi
        push edi
        push kind
        mov eax,dword ptr ds:[0x6a9988]
        mov ecx,0x4e2c13
        call ecx
        pop edi
        pop esi
    }
    areaChangeRunning=false;log("NET AREA_CHANGE run tick=%u kind=%lu caller=%08lx stage=%lu",tick,kind,areaChangeCaller,readWord(readWord(0x6a9988)+0x2c));
}
static void __declspec(naked) nextAreaEntry(){
    __asm {
        push eax
        push dword ptr [esp+8]
        push dword ptr [esp+8]
        call nextAreaAllowed
        add esp,8
        test al,al
        pop eax
        jz next_wait
        push esi
        push edi
        mov edi,dword ptr ds:[0x6a9974]
        push 0x4e2c1b
        ret
    next_wait:
        ret 4
    }
}
// Diagnostics: every change of the transition manager stage (4E207A), with caller and whether it ran inside the tick.
static void __cdecl stageObserved(DWORD manager,DWORD caller,DWORD stage){
    static unsigned lines=0;DWORD current=readWord(manager+0x2c);if(current==stage||lines>=400)return;lines++;
    log("NET STAGE_SET tick=%u %lu->%lu caller=%08lx in_sim=%d",networkTick,current,stage,caller,networkInSimulation&&GetCurrentThreadId()==networkSimulationThread);
}
static void __declspec(naked) stageEntry(){
    __asm {
        pushad
        push dword ptr [esp+36]
        push dword ptr [esp+36]
        push ecx
        call stageObserved
        add esp,12
        popad
        push ebp
        mov ebp,esp
        and esp,0xfffffff8
        push 0x4e2080
        ret
    }
}
static bool installStageLog(){
    const BYTE expected[]={0x55,0x8b,0xec,0x83,0xe4,0xf8};BYTE* at=(BYTE*)0x4e207a;
    if(memcmp(at,expected,sizeof(expected))){log("FAIL stage guard");return false;}
    DWORD old=0;if(!VirtualProtect(at,5,PAGE_EXECUTE_READWRITE,&old))return false;
    at[0]=0xe9;*(DWORD*)(at+1)=(DWORD)(ULONG_PTR)stageEntry-(0x4e207a+5);DWORD ignored=0;VirtualProtect(at,5,old,&ignored);FlushInstructionCache(GetCurrentProcess(),at,5);return true;
}
static bool installNisStopGate(){
    const BYTE expected[]={0x8b,0x80,0xe4,0xb4,0x00,0x00,0x85,0xc0};BYTE* at=(BYTE*)0x4e12d4;
    if(memcmp(at,expected,sizeof(expected))){log("FAIL NIS stream-done guard");return false;}
    DWORD old=0;if(!VirtualProtect(at,5,PAGE_EXECUTE_READWRITE,&old))return false;
    at[0]=0xe9;*(DWORD*)(at+1)=(DWORD)(ULONG_PTR)streamDoneEntry-(0x4e12d4+5);DWORD ignored=0;VirtualProtect(at,5,old,&ignored);FlushInstructionCache(GetCurrentProcess(),at,5);
    log("NET NIS stream-done gate installed");return true;
}
static bool installExitGate(){
    {const BYTE expected[]={0x56,0x57,0x8b,0x3d,0x74,0x99,0x6a,0x00};BYTE* at=(BYTE*)0x4e2c13;
    if(memcmp(at,expected,sizeof(expected))){log("FAIL next area guard");return false;}
    DWORD old=0;if(!VirtualProtect(at,5,PAGE_EXECUTE_READWRITE,&old))return false;
    at[0]=0xe9;*(DWORD*)(at+1)=(DWORD)(ULONG_PTR)nextAreaEntry-(0x4e2c13+5);DWORD ignored=0;VirtualProtect(at,5,old,&ignored);FlushInstructionCache(GetCurrentProcess(),at,5);}
    const BYTE expected[]={0xa1,0x1c,0xed,0x69,0x00,0x8b,0x40,0x24};BYTE* at=(BYTE*)0x4d23fd;
    if(memcmp(at,expected,sizeof(expected))){log("FAIL level exit guard");return false;}
    DWORD old=0;if(!VirtualProtect(at,5,PAGE_EXECUTE_READWRITE,&old))return false;
    at[0]=0xe9;*(DWORD*)(at+1)=(DWORD)(ULONG_PTR)exitGateEntry-(0x4d23fd+5);DWORD ignored=0;VirtualProtect(at,5,old,&ignored);FlushInstructionCache(GetCurrentProcess(),at,5);
    log("NET level exit gate installed");return true;
}
static bool localNisLoaded(){
    DWORD manager=readWord(0x6a9984);if(!manager)return false;bool loaded=readWord(manager+0xb244)||readWord(manager+0xb248);
    DWORD state=readWord(manager+0xb4e4);if(state&&readWord(state+0xc)==2)loaded=false;return loaded;
}
// Research (COOP_PROBE_NIS_GATE_TEST=1): fake loaded status, host from tick 600, partner from tick 900.
static bool networkNisGateTest=false;
static bool __cdecl nisReadyAllowed(){
    if(!(campaignMode&&networkLockstep()&&networkInSimulation&&GetCurrentThreadId()==networkSimulationThread))return true;
    static unsigned holds=0;if(!networkNisAgreed&&localNisLoaded()&&holds++%120==0)log("NET NIS_WAIT loaded here, waiting for the partner's cutscene load tick=%u",networkTick);
    return networkNisAgreed&&(!nisLocalLoadStart||nisAgreedNotReadyAt>=nisLocalLoadStart+networkDelay());   // own "not ready" sample has arrived
}
static void __declspec(naked) nisReadyEntry(){
    __asm {
        push ecx
        call nisReadyAllowed
        pop ecx
        test al,al
        jz not_ready
        push esi
        xor esi,esi
        lea eax,[ecx+0xb24c]
        push 0x4e09ea
        ret
    not_ready:
        xor al,al
        ret
    }
}
// Game fix: Mysterio's laser (ability type 0x15 "Laser", class table 562CC0). In this Game.exe a no-CD crack's
// loader stub (string "...\sd46nocd.dll", call to the Windows XP address 7C801D77) overwrote 487318..4873E3: the end of
// the laser's deactivate (vtable +18, 4872B6) and the start of its init (487379, called from 483BB0). The first
// laser of the Nepal 4 boss fight crashed both PCs at 48739F (write into read-only code). Both pieces are rebuilt here
// from the surviving code and from the sister classes (init 480E11/48526A, deactivate 48B48E), only when the exact
// broken bytes are present.
//   init(obj, owner, def, spawn) stdcall: base init 48C61C(obj, owner, 0), class counter 69EA40, def at +78,
//   active +4, start position spawn+0 -> +18, end time +90 = def+1C (def+20 is the time after a hit, 4871EB),
//   firing +94; then the original code from 4873E4 (ebx obj, eax def, ecx spawn, edi 0, 12-byte frame).
static volatile LONG laserInits=0,laserDeactivates=0;
static void __declspec(naked) laserInit(){
    __asm {
        lock inc dword ptr [laserInits]
        push ebp
        mov ebp,esp
        sub esp,0Ch
        inc dword ptr ds:[0x69ea40]
        push ebx
        mov ebx,dword ptr [ebp+8]
        push esi
        push edi
        push 0
        push dword ptr [ebp+0Ch]
        push ebx
        mov eax,0x48c61c
        call eax
        mov eax,dword ptr [ebp+10h]
        mov dword ptr [ebx+78h],eax
        mov byte ptr [ebx+4],1
        mov esi,dword ptr [ebp+14h]
        lea edi,[ebx+18h]
        movsd
        movsd
        movsd
        mov eax,dword ptr [ebp+10h]
        fld dword ptr [eax+1Ch]
        fstp dword ptr [ebx+90h]
        mov byte ptr [ebx+94h],1
        mov ecx,dword ptr [ebp+14h]
        xor edi,edi
        push 0x4873e4
        ret
    }
}
//   deactivate (thiscall): the surviving start (effects 49065E, counter, 4B64E1, owner callback B8(obj, B0), +88,
//   physics flags, velocity zeroed), then the pool return used by every 48CE8A-type object (pool [6A992C]+40 for
//   type 0x15, active list +0C/+10 linked through obj+60, free list +14/+18, count +4) and the base deactivate 48C6DE.
static void __declspec(naked) laserDeactivate(){
    __asm {
        lock inc dword ptr [laserDeactivates]
        push esi
        mov esi,ecx
        push edi
        lea edi,[esi+4Ch]
        mov eax,0x49065e
        call eax
        dec dword ptr ds:[0x69ea40]
        push esi
        mov eax,0x4b64e1
        call eax
        pop ecx
        mov eax,dword ptr [esi+0B8h]
        test eax,eax
        je no_callback
        push dword ptr [esi+0B0h]
        push esi
        call eax
        pop ecx
        pop ecx
    no_callback:
        and dword ptr [esi+88h],0
        mov eax,dword ptr [esi+74h]
        mov eax,dword ptr [eax+4]
        or dword ptr [eax+18h],4
        mov eax,dword ptr [esi+74h]
        mov eax,dword ptr [eax+8]
        and dword ptr [eax+34h],0FFFFFFF7h
        mov eax,dword ptr [esi+74h]
        mov eax,dword ptr [eax+4]
        xorps xmm0,xmm0
        movss dword ptr [eax+0F8h],xmm0
        movss dword ptr [eax+0FCh],xmm0
        movss dword ptr [eax+100h],xmm0
        mov eax,dword ptr ds:[0x6a992c]
        mov edi,dword ptr [eax+40h]
        cmp dword ptr [edi+4],0
        jbe pool_done
        push esi
        lea eax,[edi+0Ch]
        mov ecx,0x4817a9
        call ecx
        lea eax,[edi+18h]
        cmp dword ptr [edi+14h],0
        jne free_linked
        mov dword ptr [eax],esi
    free_linked:
        mov eax,dword ptr [edi+14h]
        mov dword ptr [esi+60h],eax
        mov dword ptr [edi+14h],esi
        dec dword ptr [edi+4]
    pool_done:
        mov eax,esi
        pop edi
        pop esi
        mov ecx,0x48c6de
        jmp ecx
    }
}
static bool installLaserFix(){
    const BYTE stub[]={0xe8,0x00,0x00,0x00,0x00,0x58,0xc6,0x40,0x8f,0xfe},tail[]={0xc6,0x83,0x99,0x00,0x00,0x00,0x00},start[]={0x56,0x8b,0xf1,0x57,0x8d,0x7e,0x4c};
    if(memcmp((void*)0x487399,stub,sizeof(stub))||memcmp((void*)0x4873e4,tail,sizeof(tail))||memcmp((void*)0x4872b6,start,sizeof(start))){log("GAME_FIX laser: Game.exe does not have the broken laser code, nothing patched");return true;}
    for(DWORD at:{0x487379u,0x4872b6u}){DWORD old=0;if(!VirtualProtect((void*)at,5,PAGE_EXECUTE_READWRITE,&old))return false;
        *(BYTE*)at=0xe9;*(DWORD*)(at+1)=(DWORD)(ULONG_PTR)(at==0x487379u?(void*)laserInit:(void*)laserDeactivate)-(at+5);DWORD ignored=0;VirtualProtect((void*)at,5,old,&ignored);FlushInstructionCache(GetCurrentProcess(),(void*)at,5);}
    log("GAME_FIX laser init 487379 and deactivate 4872B6 rebuilt (no-CD stub damage)");return true;
}
// Game fix: Transylvania 3 crashed both PCs while its first frame was drawn, at 4D33B4 (4D324D, called from the render
// method 4D6632 with the list [esi+90]): an item of that list (next at +2C) whose object at +28 is still empty gets its
// visible flag (+42) written. The visibility test 43BE95 already ran; empty items are now skipped in that loop
// (the later drawing loop 4D2B97 reads only the item's own fields).
static void __declspec(naked) drawListGuard(){
    __asm {
        mov ecx,eax
        mov eax,dword ptr [ebp+8]
        mov eax,dword ptr [eax+28h]
        test eax,eax
        jz item_empty
        test ecx,ecx
        jne item_visible
        push 0x4d33b4
        ret
    item_visible:
        push 0x4d33b9
        ret
    item_empty:
        push 0x4d33c3
        ret
    }
}
static bool installDrawListFix(){
    const BYTE expected[]={0x85,0xc0,0x8b,0x45,0x08,0x8b,0x40,0x28,0x75,0x05};BYTE* at=(BYTE*)0x4d33aa;
    if(memcmp(at,expected,sizeof(expected))){log("GAME_FIX draw list: unexpected bytes at 4D33AA, nothing patched");return true;}
    DWORD old=0;if(!VirtualProtect(at,5,PAGE_EXECUTE_READWRITE,&old))return false;
    at[0]=0xe9;*(DWORD*)(at+1)=(DWORD)(ULONG_PTR)drawListGuard-(0x4d33aa+5);DWORD ignored=0;VirtualProtect(at,5,old,&ignored);FlushInstructionCache(GetCurrentProcess(),at,5);
    log("GAME_FIX draw list 4D33AA skips items without an object");return true;
}
static bool installNisGate(){
    const BYTE expected[]={0x56,0x33,0xf6,0x8d,0x81,0x4c,0xb2,0x00,0x00};BYTE* at=(BYTE*)0x4e09e1;
    if(memcmp(at,expected,sizeof(expected))){log("FAIL NIS ready guard");return false;}
    DWORD old=0;if(!VirtualProtect(at,5,PAGE_EXECUTE_READWRITE,&old))return false;
    at[0]=0xe9;*(DWORD*)(at+1)=(DWORD)(ULONG_PTR)nisReadyEntry-(0x4e09e1+5);DWORD ignored=0;VirtualProtect(at,5,old,&ignored);FlushInstructionCache(GetCurrentProcess(),at,5);
    log("NET NIS ready gate installed");return true;
}
// Per-tick parts of the shared state, kept for the last 64 ticks; logged when the two games disagree, so the next
// report shows which part differed (characters, progress, area or game mode).
struct StateParts{unsigned tick,actors,progress,area,mode,previous,owner,rng;};static StateParts networkParts[64]={};
// Character swap in co-op: either player holds the game's character-switch action (TAB on the keyboard) for half a
// second and the two players trade characters. Computed from the exchanged inputs of the same tick, so both games
// swap on the same frame; the native switch itself stays off while two people play. Kept until swapped back.
static unsigned networkSwapSlot(coop::Actions& host,coop::Actions& partner){
    bool held=host[networkSwapAction]>0.5f||partner[networkSwapAction]>0.5f;host[networkSwapAction]=0;partner[networkSwapAction]=0;
    if(!held){networkSwapHeld=0;networkSwapLatched=false;}
    else if(!networkSwapLatched&&++networkSwapHeld>=30){networkSwapped=!networkSwapped;networkSwapLatched=true;log("NET CHARACTER_SWAP tick=%u host_slot=%u",networkTick,networkSwapped?1u:0u);}
    return networkSwapped?1u:0u;
}
static void networkInput(){
    if(networkFailed||networkComplete){applyNetworkActions(0,{});applyNetworkActions(1,{});if(networkFailed&&gameWindow&&!networkCloseRequested){networkCloseRequested=true;log("NET failure requests normal close of owned game window");PostMessageW(gameWindow,WM_CLOSE,0,0);}return;}
    if(!networkReady&&!openNetworkConnection())return;
    if(networkFrameLimit&&networkTick>=networkFrameLimit&&(!networkMissionEndFixture||(campaignCycleTarget>1&&campaignCycle+1>=campaignCycleTarget))){networkFinishScript();return;}
    static coop::Actions history[32];coop::Actions local=humanInputMode?captureLocalActions():networkBotActions(networkRole==1?0:1,networkTick),remote={};
    if(tutorialUiTest)local=physicalFixtureActions(false);
    // Campaign test bots in real-input runs: the host accepts the results screen after a level end (scene 8), as a player would.
    if(campaignFixture&&humanInputMode&&!humanInputFixture&&networkRole==1&&readWord(readWord(0x6a9940)+0x4e4)==8&&networkTick%60<2)local[10]=1.0f;
    if(campaignMode){bool nis=localNisLoaded();if(networkNisGateTest)nis=networkTick>=(networkRole==1?600u:900u);
        {static bool lastNis=true;if(lastNis&&!nis)nisLocalLoadStart=networkTick;lastNis=nis;}
        local[networkNisAction]=readyBits(nis,localExitUnlocked(),localNextAreaLoaded(),areaChangePending!=0,localStreamDone());}
    history[networkTick%32]=local;
    static coop::MenuPointer pointers[32]={};
    coop::MenuPointer pointer=networkRole==1?localMenuPointer:coop::MenuPointer{},remotePointer={};
    localMenuPointer.flags&=~coop::menuPointerClick;
    if(tutorialUiTest&&networkRole==1&&tutorialFixtureStart!=~0u&&!tutorialClosed){
        pointer={32768,52428,coop::menuPointerActive};unsigned age=networkTick-tutorialFixtureStart;
        if(age>=120&&age%30<2)pointer.flags|=coop::menuPointerClick;
    }
    pointers[networkTick%32]=pointer;
    verifyNativeAxes(local);if(networkFailed)return;
    static std::uint64_t states[32]={};states[networkTick%32]=networkState;std::uint64_t remoteState=0;
    if(networkJitterFixture){if((networkTick+networkRole*7)%43==0)Sleep(45+networkRole*10);if(networkTick==1500&&networkRole==2)Sleep(500);}
    double stepBegan=milliseconds();bool stepped=networkWire->Step(networkTick,local,remote,networkState,&remoteState,&pointer,&remotePointer);if(stepped)frameWaits.add(milliseconds()-stepBegan);
    if(!stepped){
        if(campaignPartnerLost(networkWire->PeerStopped()?"partner closed the game":"connection lost during play"))return;
        if(networkWire->PeerStopped()){networkComplete=true;applyNetworkActions(0,{});applyNetworkActions(1,{});log("NET peer ended session normally frame=%u; no co-op QA awarded",networkTick);
            if(!gameWindow||!PostMessageW(gameWindow,WM_CLOSE,0,0)){networkFailed=true;log("NET FAIL peer stop close error=%lu",GetLastError());}return;}
        networkFailed=true;log("NET FAIL frame=%u %s",networkTick,networkWire->Error().c_str());applyNetworkActions(0,{});applyNetworkActions(1,{});return;}
    const unsigned delay=networkDelay();
    if(networkTick>=delay&&remoteState!=states[(networkTick-delay)%32]){
        for(unsigned back=0;back<8&&back<=networkTick;back++){const StateParts& p=networkParts[(networkTick-delay-back)%64];
            log("NET DIVERGENCE_PARTS tick=%u actors=%08x progress=%08x area=%08x mode=%u/%u/%u rng=%08x",p.tick,p.actors,p.progress,p.area,p.mode,p.previous,p.owner,p.rng);}
        networkFailed=true;log("NET FAIL shared-state divergence observed_tick=%ld own=%016llx peer=%016llx",(LONG)networkTick-(LONG)delay-1,states[(networkTick-delay)%32],remoteState);return;}
    coop::Actions delayed=networkTick<delay?coop::Actions{}:history[(networkTick-delay)%32];
    coop::Actions& host=networkRole==1?delayed:remote;coop::Actions& partner=networkRole==1?remote:delayed;
    if(campaignMode){unsigned both=readyBitsOf(host[networkNisAction])&readyBitsOf(partner[networkNisAction]);bool agreed=(both&4)!=0;if(!agreed)nisAgreedNotReadyAt=networkTick;
        if(agreed!=networkNisAgreed)log("NET NIS_AGREED %s tick=%u",agreed?"ready":"not ready",networkTick);networkNisAgreed=agreed;
        networkAreaChangeAgreed=(both&8)!=0;networkStreamDoneAgreed=(both&16)!=0;unsigned exit=both&3;if(exit!=networkExitAgreed)log("NET EXIT_AGREED unlocked=%u next_area_loaded=%u tick=%u",exit>>1,exit&1,networkTick);networkExitAgreed=exit;
        host[networkNisAction]=0;partner[networkNisAction]=0;}
    DWORD mode[3]={};if(!captureGameMode(mode)){networkFailed=true;log("NET FAIL mission menu mode unreadable");return;}
    DWORD scene=readWord(readWord(0x6a9940)+0x4e4),movie=readWord(0x6a9980);
    bool menu=coop::MissionMenuActive(128,mode[0],scene,movie&&readWord(movie+4)==1);
    appliedMenuPointer=networkRole==1?(networkTick<delay?coop::MenuPointer{}:pointers[(networkTick-delay)%32]):remotePointer;
    applyMissionPointer();
    if(menu&&(appliedMenuPointer.flags&coop::menuPointerClick))host[10]=1;
    bool peerAccept=partner[10]>0.5f,hostAccept=host[10]>0.5f;
    DWORD pauseAction=readWord(readWord(0x69e56c)+14*4);
    if(pauseAction>=28){networkFailed=true;log("NET FAIL native pause mapping unavailable");return;}
    coop::ApplyMissionMenuOwnership(host,partner,menu,pauseAction);
    if(tutorialUiTest&&!tutorialClosed){
        if(scene==14&&mode[0]==1){
            if(tutorialOpen==~0u){tutorialOpen=networkTick;log("TUTORIAL_UI OPEN tick=%u area=Tokyo/1_Level/1_Area scene=14",networkTick);}
            if(peerAccept&&!hostAccept&&++tutorialIgnored==3&&!tutorialIgnoredLogged){tutorialIgnoredLogged=true;log("TUTORIAL_UI PEER_IGNORED tick=%u scene=14 attempts=3 host=0 peer=%u",networkTick,partner[10]>0.5f?1u:0u);}
            if(host[10]>0.5f&&!tutorialAccepted){tutorialAccepted=true;log("TUTORIAL_UI HOST_ACCEPT tick=%u scene=14 host=1 peer=%u",networkTick,partner[10]>0.5f?1u:0u);}
        }else if(tutorialAccepted){tutorialClosed=true;log("TUTORIAL_UI CLOSED tick=%u scene=14 host=1 peer=0",networkTick);}
    }
    unsigned hostSlot=networkSwapSlot(host,partner);applyNetworkActions(hostSlot,host);applyNetworkActions(1-hostSlot,partner);++networkTick;
}
// Native constructor4E1EFB writes vtables at+0/+28, zeroes2C/30,
// and initializes5C=FFFFFFFF (4E1F29..37). Called only after gameplay setup.
// Select fields explicitly: no process pointers, padding or inactive pause-owner bytes.
static bool captureGameMode(DWORD (&sample)[3]){
    DWORD manager=0,table=0,scriptTable=0;
    if(!readMemory((void*)0x6a9988,&manager,sizeof(manager))||!manager||
       !readMemory((void*)manager,&table,sizeof(table))||table!=0x57879c||
       !readMemory((void*)(manager+0x28),&scriptTable,sizeof(scriptTable))||scriptTable!=0x5787a8||
       !readMemory((void*)(manager+0x2c),&sample[0],sizeof(DWORD))||
       !readMemory((void*)(manager+0x30),&sample[1],sizeof(DWORD)))return false;
    sample[2]=~0u;
    if(sample[0]==1&&(!readMemory((void*)(manager+0x5c),&sample[2],sizeof(DWORD))||(sample[2]!=~0u&&sample[2]>3)))return false;
    coop::ModeSample canonical={};
    if(!coop::CanonicalModeSample(sample[0],sample[1],sample[2],canonical))return false;
    sample[0]=canonical.mode;sample[1]=canonical.previous;sample[2]=canonical.owner;
    return true;
}
static void observePauseFixture(unsigned tick,unsigned actorHash,const DWORD (&mode)[3]){
    // Later native tutorial/system pauses are separate events. Their mode,
    // owner, world and progress remain covered by the continuous live sample.
    if(!networkPauseFixture||tick>430)return;
    static bool entered=false,resumed=false;
    static unsigned stableHash=0,pausedFrames=0;
    if(mode[0]==1){
        if(!entered){
            if(tick<243||tick>260||mode[1]!=0||mode[2]!=0){networkFailed=true;log("NET FAIL PAUSE_FIXTURE unexpected entry tick=%u mode=%lu previous=%lu owner=%lu",tick,mode[0],mode[1],mode[2]);return;}
            entered=true;stableHash=actorHash;log("PAUSE ASSERT ENTERED tick=%u mode=1 previous=0 owner=0 actor=%08x",tick,actorHash);
        }
        if(resumed||mode[1]!=0||mode[2]!=0||actorHash!=stableHash){networkFailed=true;log("NET FAIL PAUSE_FIXTURE paused world or ownership changed tick=%u frames=%u actor=%08x expected=%08x mode=%lu previous=%lu owner=%lu",tick,pausedFrames,actorHash,stableHash,mode[0],mode[1],mode[2]);return;}
        ++pausedFrames;
        if(pausedFrames==90)log("PAUSE ASSERT WORLD_FROZEN tick=%u stable_frames=90 actor=%08x",tick,actorHash);
    }else if(entered&&!resumed){
        // Back is captured400/applied403. Native FE close animation completes
        // asynchronously over later menu ticks; observe the real mode change.
        if(tick<403||tick>430||mode[0]!=0||pausedFrames<90){networkFailed=true;log("NET FAIL PAUSE_FIXTURE unexpected resume tick=%u mode=%lu stable_frames=%u",tick,mode[0],pausedFrames);return;}
        resumed=true;log("PAUSE ASSERT RESUMED tick=%u mode=0 stable_frames=%u",tick,pausedFrames);
    }
    if(tick==260&&!entered){networkFailed=true;log("NET FAIL PAUSE_FIXTURE did not enter pause by260 mode=%lu",mode[0]);return;}
    if(tick==430){
        if(!entered||!resumed||mode[0]!=0||pausedFrames<90){networkFailed=true;log("NET FAIL PAUSE_FIXTURE incomplete by430 entered=%u resumed=%u mode=%lu stable_frames=%u",entered,resumed,mode[0],pausedFrames);return;}
        log("PAUSE ASSERT COMPLETE tick=430 stable_frames=%u; native controls only; no co-op QA awarded",pausedFrames);
    }
}
static void observeNetworkState(unsigned tick){
    DWORD positions[2][3]={0},slots[2]={~0u,~0u};DWORD manager=readWord(0x6a9900);
    for(unsigned player=0;player<2;player++){
        DWORD handle[3]={0};if(!manager||!readMemory((void*)(manager+0x128+player*12),handle,sizeof(handle))||!handle[1]||handle[2]==~0u)continue;
        DWORD header=readWord(handle[1]+8);if(!header||readWord(header+8)!=handle[2])continue;
        DWORD actor=handle[1],table=readWord(actor),transform=readWord(actor+0x1d0);if(readWord(table+0x2f4)==0x45a6c9&&transform)readMemory((void*)transform,positions[player],12);
        DWORD controller=readWord(actor+0x2dc);slots[player]=readWord(controller+0xc0);
    }
    struct ActorState {DWORD serial,kind,position[3],heading,slot,health,maximumHealth;};ActorState actors[128]={};unsigned count=0;DWORD node=readWord(manager+0x34);
    for(unsigned visited=0;node&&visited<128;visited++){
        DWORD handle[3]={0};if(!readMemory((void*)node,handle,sizeof(handle)))break;node=handle[0];DWORD actor=handle[1];if(!actor||handle[2]==~0u)continue;DWORD header=readWord(actor+8);if(!header||readWord(header+8)!=handle[2])continue;
        ActorState& state=actors[count++];state.serial=handle[2];state.kind=readWord(actor);DWORD transform=readWord(actor+0x1d0);
        state.slot=~0u;
        if(readWord(state.kind+0x2f4)==0x45a6c9&&transform){readMemory((void*)transform,state.position,12);WORD heading=0;readMemory((void*)(transform+12),&heading,2);state.heading=heading;state.slot=readWord(readWord(actor+0x2dc)+0xc0);}
        state.health=state.maximumHealth=~0u;if(readWord(state.kind+0x21c)==0x45119b){state.health=readWord(actor+0x398);state.maximumHealth=readWord(actor+0x39c);}
        if((probeDiagnostics&&(tick%120==0||(tick>=1400&&tick<=1600)))||tick%600==0){float xyz[3]={0},health=0,maximum=0;memcpy(xyz,state.position,12);memcpy(&health,&state.health,4);memcpy(&maximum,&state.maximumHealth,4);BYTE team=0;readMemory((void*)(actor+0x304),&team,1);log("NET actor tick=%u serial=%lu kind=%08lx xyz=%.9g,%.9g,%.9g hp=%.9g/%.9g slot=%ld team_flag=%u heading=%lu",tick,state.serial,state.kind,xyz[0],xyz[1],xyz[2],health,maximum,(LONG)state.slot,team,state.heading);}
    }
    std::sort(actors,actors+count,[](const ActorState& a,const ActorState& b){return a.serial<b.serial;});
    unsigned worldHash=2166136261u;const BYTE* bytes=(BYTE*)actors;for(unsigned i=0;i<count*sizeof(ActorState);i++)worldHash=(worldHash^bytes[i])*16777619u;
    DWORD gameMode[3]={};if(!captureGameMode(gameMode)){networkFailed=true;log("NET FAIL initialized game-mode read/guard tick=%u",tick);return;}
    if(probeDiagnostics&&tick%120==0)log("NET mode tick=%u mode=%lu previous=%lu owner=%lu",tick,gameMode[0],gameMode[1],gameMode[2]);
    observePauseFixture(tick,worldHash,gameMode);if(networkFailed)return;
    if(networkPauseFixture&&tick>=234&&tick<=430&&(tick%30==0||tick==243||tick==403||tick==404))log("PAUSE OBSERVE tick=%u mode=%lu previous=%lu owner=%lu actor=%08x",tick,gameMode[0],gameMode[1],gameMode[2],worldHash);
    BYTE progress[0x1fc]={0};if(!readMemory((void*)0x69c470,progress,sizeof(progress))){networkFailed=true;log("NET FAIL serialized progress read");return;}
    // Descriptor+0 is only a short name (e.g.1_Area) reused in other regions.
    // Descriptor+4 is the full canonical path; require a bounded terminator.
    unsigned progressHash=coop::ProgressSample(progress),areaHash=2166136261u;DWORD area=readWord(readWord(0x6a9974)+0x18),name=readWord(area+4);
    bool areaTerminated=false;
    for(unsigned i=0;i<96;i++){BYTE c=0;if(!name||!readMemory((void*)(name+i),&c,1)){networkFailed=true;log("NET FAIL current full area path read");return;}if(!c){areaTerminated=i>0;break;}areaHash=(areaHash^c)*16777619u;}
    if(!areaTerminated){networkFailed=true;log("NET FAIL current full area path empty or unbounded");return;}
    {StateParts& parts=networkParts[tick%64];parts={tick,worldHash,progressHash,areaHash,gameMode[0],gameMode[1],gameMode[2],readWord(0x58cf24)};}
    unsigned context[5]={progressHash,areaHash,gameMode[0],gameMode[1],gameMode[2]};bytes=(BYTE*)context;for(unsigned i=0;i<sizeof(context);i++)worldHash=(worldHash^bytes[i])*16777619u;
    if(probeDiagnostics&&tick%120==0){char hex[sizeof(progress)*2+1]={0};for(unsigned i=0;i<sizeof(progress);i++)sprintf(hex+i*2,"%02x",progress[i]);log("NET progress tick=%u fields=%08x raw=%s",tick,progressHash,hex);}
    networkState=((std::uint64_t)readWord(0x58cf24)<<32)|worldHash;
    if(networkMutateFixture&&tick==240){networkState^=1;log("NET MUTATION_FIXTURE corrupted sampled state at tick=240");}
    if(probeDiagnostics||tick%120==0)log("NET state tick=%u rng=%08lx pos0=%08lx,%08lx,%08lx pos1=%08lx,%08lx,%08lx slots=%ld,%ld input=%08x,%08x world=%u,%08x progress=%08x area=%08x",tick,readWord(0x58cf24),positions[0][0],positions[0][1],positions[0][2],positions[1][0],positions[1][1],positions[1][2],(LONG)slots[0],(LONG)slots[1],networkInputHash[0],networkInputHash[1],count,worldHash,progressHash,areaHash);
}
typedef void (__thiscall* SimulationFn)(void*,float);
static SimulationFn originalSimulation=0;
static bool triggerContains(DWORD trigger,const float* point){bool result=false;
    __asm {
        push esi
        mov esi,trigger
        mov eax,point
        mov ecx,0x4d4fd9
        call ecx
        mov result,al
        pop esi
    }
    return result;
}
static bool campaignTriggerName(DWORD trigger,char (&name)[33]);
static void transitionFixture(unsigned tick){
    if(!networkTransitionFixture||tick<networkTransitionNextTick)return;
    DWORD areas=readWord(0x6a9974),currentArea=readWord(areas+0x18),slot=readWord(areas+0x14);
    if(tick%120==0){DWORD transition=readWord(0x6a9988);log("TRANSITION STATUS tick=%u slot=%lu stage=%lu exit_kind=%lu area_id=%08lx next_link=%08lx preload_id=%08lx script_flags=%08lx",tick,slot,readWord(transition+0x2c),readWord(transition+0x34),readWord(currentArea),readWord(readWord(currentArea+0x10)),readWord(readWord(areas+0x1c)),readWord(readWord(0x69ed1c)+0x24));}
    if(networkTransitionWarped){if(!networkTransitionChanged&&currentArea&&currentArea!=networkTransitionStartArea){networkTransitionChanged=true;++networkTransitionCount;char areaName[64]={0};DWORD name=readWord(currentArea);for(unsigned i=0;i<sizeof(areaName)-1;i++){char c=0;if(!readMemory((void*)(name+i),&c,1)||!c)break;if((c<'A'||c>'Z')&&(c<'a'||c>'z')&&(c<'0'||c>'9')&&c!='_')break;areaName[i]=c;}log("TRANSITION AREA_CHANGED tick=%u old_slot=%lu new_slot=%lu count=%u area=%s",tick,networkTransitionStartSlot,slot,networkTransitionCount,areaName);if(networkTransitionCount<networkTransitionTarget){networkTransitionWarped=false;networkTransitionNextTick=tick+600;}}return;}
    if(networkTransitionCount>=networkTransitionTarget){
        // Mission-end discovery: name every trigger in the final fixture area once.
        static bool finalLogged=false;static unsigned changedAt=0;if(!finalLogged&&networkTransitionChanged&&!changedAt)changedAt=tick+1;if(!finalLogged&&changedAt&&tick>=changedAt+120){finalLogged=true;DWORD pool=readWord(0x6a9970),count=readWord(pool+8),items=readWord(pool+4);log("MISSION_END TRIGGERS tick=%u count=%lu area=%08lx",tick,count,readWord(currentArea));
            for(unsigned i=0;i<count&&i<256;i++){DWORD trigger=readWord(items+i*4);if(!trigger)continue;char name[33];if(!campaignTriggerName(trigger,name))strcpy(name,"-");log("MISSION_END TRIGGER index=%u name=%s table=%08lx flags=%08lx enabled=%lu enter=%08lx center=%.6g,%.6g,%.6g extents=%.6g,%.6g,%.6g",i,name,readWord(trigger),readWord(trigger+0x74),readWord(trigger+0x78),readWord(readWord(trigger)+0x24),readFloat(trigger+0x50),readFloat(trigger+0x54),readFloat(trigger+0x58),readFloat(trigger+0x60),readFloat(trigger+0x64),readFloat(trigger+0x68));}}
        return;}
    // Native4D2301 rejects entry while the transition manager is busy. Establish
    // this precondition before the fixture's one-shot enter, just as a player must.
    DWORD exitStage=readWord(readWord(0x6a9988)+0x2c);
    if(exitStage){if(tick%120==0)log("TRANSITION WAIT_IDLE_MANAGER tick=%u stage=%lu",tick,exitStage);return;}
    DWORD pool=readWord(0x6a9970),count=readWord(pool+8),items=readWord(pool+4);if(count>4096){networkFailed=true;log("NET FAIL transition trigger pool bound");return;}
    for(unsigned i=0;i<count;i++){
        DWORD trigger=readWord(items+i*4);if(readWord(trigger)!=0x576074||!readWord(trigger+0x78)||readWord(trigger+0x80)!=2)continue;
        if(!(readWord(trigger+0x74)&1)||readWord(readWord(trigger)+0x24)!=0x4d244b){if(tick%120==0)log("TRANSITION EXIT_ENTER_DISABLED tick=%u trigger_index=%u flags=%08lx",tick,i,readWord(trigger+0x74));continue;}
        float center[3]={readFloat(trigger+0x50),readFloat(trigger+0x54),readFloat(trigger+0x58)};
        // Both area slots can retain registered exits. This forward-only fixture
        // must not re-enter a volume it already consumed in the preceding area.
        bool used=false;for(unsigned j=0;j<networkTransitionCount;j++)if(!memcmp(center,networkTransitionUsedCenters[j],sizeof(center)))used=true;
        if(used){if(tick%120==0)log("TRANSITION PREVIOUS_EXIT_SKIPPED tick=%u trigger_index=%u",tick,i);continue;}
        if(!std::isfinite(center[0])||!std::isfinite(center[1])||!std::isfinite(center[2])||!triggerContains(trigger,center)){networkFailed=true;log("NET FAIL transition invalid exit volume");return;}
        DWORD party=readWord(0x6a9900),actors[2]={readWord(party+0x12c),readWord(party+0x138)};
        for(DWORD actor:actors)if(!actor||readWord(readWord(actor)+0x640)!=0x459fe4||readWord(readWord(actor)+0x2f4)!=0x45a6c9){networkFailed=true;log("NET FAIL transition party warp guard");return;}
        float target[3]={center[0],center[1],center[2]-readFloat(0x55b520)};
        for(DWORD actor:actors){WORD heading=0;readMemory((void*)(readWord(actor+0x1d0)+12),&heading,2);((void(__thiscall*)(void*,const float*,unsigned,bool))0x459fe4)((void*)actor,target,heading,true);float point[3]={0};readMemory((void*)readWord(actor+0x1d0),point,12);point[2]+=readFloat(0x55b520);if(!triggerContains(trigger,point)){networkFailed=true;log("NET FAIL transition party outside exit after warp");return;}}
        networkTransitionWarped=true;networkTransitionChanged=false;networkTransitionStartArea=currentArea;networkTransitionStartSlot=slot;log("TRANSITION PARTY_INSIDE_REAL_EXIT tick=%u trigger_index=%u kind=2 center=%.9g,%.9g,%.9g slot=%lu flags=%08lx extents=%.9g,%.9g,%.9g",tick,i,center[0],center[1],center[2],slot,readWord(trigger+0x74),readFloat(trigger+0x60),readFloat(trigger+0x64),readFloat(trigger+0x68));
        memcpy(networkTransitionUsedCenters[networkTransitionCount],center,sizeof(center));
        // The gate invokes the same enabled enter callback after proving containment.
        // Ordinary character physics may snap the setup warp to a different floor.
        ((void(__thiscall*)(void*,void*))0x4d244b)((void*)trigger,(void*)actors[0]);
        log("TRANSITION NATIVE_ENTER_CALLBACK tick=%u stage_after=%lu script_flags=%08lx",tick,readWord(readWord(0x6a9988)+0x2c),readWord(readWord(0x69ed1c)+0x24));return;
    }
    if(tick%120==0)log("TRANSITION WAITING_FORWARD_EXIT tick=%u trigger_count=%lu",tick,count);
}
// Research level runner (COOP_PROBE_LEVEL_END_TEST=1, campaign bots): the first mission starts in Tokyo/1_Level/4_Area
// (campaignBeforeLoad swaps the requested descriptor 5836CC -> 583660), then, inside the lockstep simulation on both
// seats at the same tick, the party is put into the area's LevelEnd trigger and its enter callback runs, so the real
// end script plays (Black Cat meeting NIS, results) and the game returns to the hub like after a played level.
static unsigned levelEndDone=0;static bool levelEndTest=false,levelEndEntered=false;static unsigned levelEndAreaTick=0,levelEndBossWait=1200;static DWORD levelEndArea=0;
// Mission start area -> where the runner starts it, and that mission's last area (analysis/native/campaign-areas.json
// forward chains). Boss missions start in the area before the boss arena (a direct start in the arena crashes the
// retail game at 45CE49 on both seats) and walk into it through the real forward exit.
struct LevelEndArea{DWORD start,run,last;bool boss;};
static const LevelEndArea levelEndAreas[]={{0x5836cc,0x583660,0x583660,false},{0x583764,0x583764,0x583740,true},{0x583834,0x5837a4,0x5837a4,false},
    {0x583ce0,0x583c74,0x583c74,false},{0x583de0,0x583d50,0x583d50,false},{0x583ee4,0x583e54,0x583e54,false},{0x583fa0,0x583f7c,0x583f58,true},
    {0x5839c0,0x583954,0x583954,false},{0x583a7c,0x583a58,0x583a34,true},{0x583b50,0x583ac0,0x583ac0,false},{0x583c0c,0x583be8,0x583bc4,true},
    {0x584454,0x5843c4,0x5843c4,false},{0x584558,0x5844c8,0x5844c8,false},{0x584680,0x5845cc,0x5845cc,false},{0x584740,0x584740,0x5846f8,true},
    {0x584098,0x584008,0x584008,false},{0x58419c,0x58410c,0x58410c,false},{0x5842a0,0x5842a0,0x584210,false},{0x58435c,0x584338,0x584314,true},{0x5838cc,0x5838cc,0x5838a8,true},
    // Middle areas of missions that start at 1_Area: Nepal 4 (a 2_Area start crashed both seats at 48739F after the
    // Mysterio intro; the 1_Area start crashed the same way) and Transylvania 3 (a 4_Area start crashed at 4D33B4).
    {0,0x58471c,0x5846f8,true},{0,0x58427c,0x584210,false},{0,0x584258,0x584210,false}};
static const LevelEndArea* levelEndByStart(DWORD area){for(auto& level:levelEndAreas)if(level.start==area)return &level;return 0;}
static const LevelEndArea* levelEndByArea(DWORD area){for(auto& level:levelEndAreas)if(level.run==area||level.last==area)return &level;return 0;}
static bool triggerText(DWORD trigger,char (&name)[33]){memset(name,0,sizeof(name));if(!readMemory((void*)(trigger+0x80),name,32))return false;name[32]=0;
    for(unsigned i=0;name[i];i++)if(name[i]<0x20||name[i]>0x7e)return false;return name[0]!=0;}
static void levelEndFixture(unsigned tick){
    if(!levelEndTest)return;DWORD areas=readWord(0x6a9974),current=readWord(areas+0x18);const LevelEndArea* level=levelEndByArea(current);
    if(current!=levelEndArea){levelEndArea=current;levelEndAreaTick=tick;levelEndEntered=false;
        if(level){DWORD pool=readWord(0x6a9970),count=readWord(pool+8),items=readWord(pool+4);log("LEVEL_END area %08lx reached tick=%u last=%d boss=%d triggers=%lu",current,tick,current==level->last,level->boss,count);
            for(unsigned i=0;i<count&&i<128;i++){DWORD trigger=readWord(items+i*4);if(!trigger)continue;char name[33];bool named=triggerText(trigger,name);
                log("LEVEL_END TRIGGER index=%u name=%s table=%08lx enter=%08lx flags=%08lx kind=%lu center=%.5g,%.5g,%.5g",i,named?name:"-",readWord(trigger),readWord(readWord(trigger)+0x24),readWord(trigger+0x74),readWord(trigger+0x80),readFloat(trigger+0x50),readFloat(trigger+0x54),readFloat(trigger+0x58));}}}
    if(!level||levelEndEntered||tick<levelEndAreaTick+600||readWord(readWord(0x6a9988)+0x2c))return;   // settle, and wait for an idle transition manager
    DWORD party=readWord(0x6a9900),actors[2]={readWord(party+0x12c),readWord(party+0x138)};
    for(DWORD actor:actors)if(!actor||readWord(readWord(actor)+0x640)!=0x459fe4){if(tick%600==0)log("LEVEL_END party warp guard");return;}
    auto enterTrigger=[&](DWORD trigger,DWORD enter,const char* what){
        float center[3]={readFloat(trigger+0x50),readFloat(trigger+0x54),readFloat(trigger+0x58)},target[3]={center[0],center[1],center[2]-readFloat(0x55b520)};
        for(DWORD actor:actors){WORD heading=0;readMemory((void*)(readWord(actor+0x1d0)+12),&heading,2);((void(__thiscall*)(void*,const float*,unsigned,bool))0x459fe4)((void*)actor,target,heading,true);}
        ((void(__thiscall*)(void*,void*))enter)((void*)trigger,(void*)actors[0]);levelEndEntered=true;
        log("LEVEL_END ENTERED %s tick=%u area=%08lx enter=%08lx flags=%08lx center=%.5g,%.5g,%.5g",what,tick,current,enter,readWord(trigger+0x74),center[0],center[1],center[2]);};
    DWORD pool=readWord(0x6a9970),count=readWord(pool+8),items=readWord(pool+4);
    if(current!=level->last){
        // Not the last area yet (boss missions): take the area's forward exit, even while the fight still keeps it locked.
        for(unsigned i=0;i<count&&i<256;i++){DWORD trigger=readWord(items+i*4);if(!trigger||readWord(trigger)!=0x576074||!readWord(trigger+0x78)||readWord(trigger+0x80)!=2)continue;
            enterTrigger(trigger,0x4d244b,"forward_exit");return;}
        if((tick-levelEndAreaTick)%600==0)log("LEVEL_END no forward exit tick=%u",tick);return;}
    for(unsigned i=0;i<count&&i<256;i++){DWORD trigger=readWord(items+i*4);char name[33];if(!trigger||!triggerText(trigger,name))continue;
        // Missions end at a LevelEnd trigger, a sidekick meeting (Egypt 2: MeetingLizard) or a transformation scene (Nepal 2).
        bool end=!_strnicmp(name,"Meeting",7)||strstr(name,"Transformation")!=0;for(unsigned j=0;name[j];j++)if(!_strnicmp(name+j,"LevelEnd",8))end=true;if(!end)continue;
        DWORD enter=readWord(readWord(trigger)+0x24);if(enter<0x401000||enter>=0x555000){log("LEVEL_END trigger %s has no enter callback",name);continue;}
        enterTrigger(trigger,enter,name);levelEndDone|=1u<<(unsigned)(level-levelEndAreas);return;}
    if(level->boss&&tick>=levelEndAreaTick+levelEndBossWait){
        // Boss arena: run the area script's own BossBattleDefeated function (records the win, then ends the mission),
        // through the same name-hash event call the game uses for OnEnter_Start (40DF49 hash, 40BA8C call).
        DWORD world=readWord(areas+4+readWord(areas+0x14)*4),script=world?readWord(world+0x88):0;if(!script){log("LEVEL_END boss arena has no script");levelEndEntered=true;return;}
        DWORD hash=0xffffffff;for(const char* c="BossBattleDefeated";*c;c++)hash=hash*33+(BYTE)*c;
        __asm{
            push esi
            mov esi,script
            push hash
            mov eax,0x40ba8c
            call eax
            pop esi
        }
        levelEndEntered=true;levelEndDone|=1u<<(unsigned)(level-levelEndAreas);log("LEVEL_END ENTERED BossBattleDefeated tick=%u area=%08lx script=%08lx lasers_started=%ld lasers_ended=%ld",tick,current,script,laserInits,laserDeactivates);return;}
    if((tick-levelEndAreaTick)%600==0)log("LEVEL_END no LevelEnd trigger yet tick=%u",tick);
}
// Test-only: call the retail mission-end routine (native131 body 4E323C, the same
// call LevelEnd/meeting/boss scripts make) on both seats at one lockstep tick.
// Its guard 4E3D1E yields through the VM argument while a transition is busy,
// so a private scratch VM is passed and the call is retried on later ticks.
static void missionEndFixture(unsigned tick){
    if(!networkMissionEndFixture||networkMissionEndInvoked||(campaignCycleTarget>1&&campaignCycle+1>=campaignCycleTarget)||tick<campaignCycleStartTick+1500)return;
    static DWORD scratchVm[16];memset(scratchVm,0,sizeof(scratchVm));DWORD vm=(DWORD)(ULONG_PTR)scratchVm;
    __asm{
        push ebx
        mov eax,vm
        mov ecx,0x4e323c
        call ecx
        pop ebx
    }
    BYTE done=0;readMemory((void*)0x69ecfd,&done,1);
    if(done){networkMissionEndInvoked=true;log("MISSION_END INVOKED tick=%u scene=%lu flow=%lu",tick,readWord(readWord(0x6a9940)+0x4e4),readWord(readWord(0x69e72c)+8));}
    else if(tick%120==0)log("MISSION_END WAIT tick=%u transition_stage=%lu",tick,readWord(readWord(0x6a9988)+0x2c));
}
static void __fastcall simulation(void* task,void*,float dt){
    DWORD flow=readWord(0x69e72c);
    if(networkLockstep()&&flow&&readWord(flow+8)==128){if(!networkReady||networkFailed||networkComplete||!networkTick)return;if(readWord(0x69ed24)!=0){networkFailed=true;log("NET FAIL unexpected simulation mode");return;}networkSimulationThread=GetCurrentThreadId();networkInSimulation=true;BYTE previousPathFlag=*(BYTE*)0x590718;if(networkPathSync)*(BYTE*)0x590718=1;areaChangeRun(networkTick-1);transitionFixture(networkTick-1);missionEndFixture(networkTick-1);levelEndFixture(networkTick-1);if(!networkFailed)originalSimulation(task,dt);if(networkPathSync)*(BYTE*)0x590718=previousPathFlag;networkInSimulation=false;if(!networkFailed)observeNetworkState(networkTick-1);}
    else originalSimulation(task,dt);
}
struct TaskTimingHook {void** table;SimulationFn original;};
static TaskTimingHook taskTimingHooks[64]={};static unsigned taskTimingCount=0;
static void __fastcall fixedTask(void* task,void*,float dt){
    void** table=*(void***)task;SimulationFn original=0;
    for(unsigned i=0;i<taskTimingCount;i++)if(taskTimingHooks[i].table==table){original=taskTimingHooks[i].original;break;}
    if(!original){networkFailed=true;log("NET FAIL unknown task timing callback");return;}
    DWORD flow=readWord(0x69e72c);
    if(networkLockstep()&&flow&&readWord(flow+8)==128){dt=1.0f/60.0f;*(float*)(flow+0x18)=dt;if(*((BYTE*)task+0x1c))dt*=*(float*)flow;*(float*)(flow+0x14)=dt;}
    original(task,dt);
}
static bool prepareTaskTiming(){
    DWORD flow=readWord(0x69e72c),head=readWord(flow+4),node=readWord(head+4);unsigned visited=0;
    if(!flow||!head||!node)return false;
    do{
        DWORD tableAddress=readWord(node);void** table=(void**)tableAddress;DWORD callback=readWord(tableAddress);bool known=false;
        for(unsigned i=0;i<taskTimingCount;i++)if(taskTimingHooks[i].table==table)known=true;
        if(!known){
            if(taskTimingCount>=64||!tableAddress||!((callback>=0x401000&&callback<0x555000)||callback==(DWORD)(ULONG_PTR)simulation))return false;
            TaskTimingHook& hook=taskTimingHooks[taskTimingCount];hook.table=table;hook.original=(SimulationFn)callback;
            if(!patchComSlot(table,0,(void*)fixedTask,(void**)&hook.original))return false;++taskTimingCount;log("NET task clock callback=%08lx table=%08lx",callback,tableAddress);
        }
        if(node==head)break;
        node=readWord(node+4);if(!node||++visited>64)return false;
    }while(true);
    return true;
}
static void assignNativeDevice(unsigned slot,DWORD device){
    __asm {
        push edi
        mov edi,slot
        mov eax,device
        push 0
        mov edx,0x40182d
        call edx
        add esp,4
        pop edi
    }
}
static unsigned __cdecl localDeviceSlot(DWORD device,unsigned slot){
    if(!humanInputMode||!device)return slot;
    DWORD table=readWord(device);if(readWord(table+0x24)==0x52d5c0&&readWord(device+4)==virtualPadIndex)return 1;
    return 0;
}
static void __declspec(naked) reassignLocalDevice(){
    __asm {
        push ebp
        mov ebp,esp
        push edi
        push esi
        mov esi,eax
        push edi
        push esi
        call localDeviceSlot
        add esp,8
        mov edi,eax
        mov eax,esi
        push dword ptr [ebp+8]
        mov edx,0x40182d
        call edx
        add esp,4
        pop esi
        pop edi
        mov esp,ebp
        pop ebp
        ret
    }
}
static bool prepareNetworkPlayers(){
    DWORD devices=readWord(0x595460),count=readWord(0x595464);if(!devices||count>64)return false;
    DWORD primary=readWord(0x691284);
    for(unsigned index=0;index<count;index++){DWORD device=readWord(devices+index*4),table=readWord(device);if(table&&readWord(table+0x24)==0x52d5c0&&(!humanInputMode||readWord(device+4)==virtualPadIndex)){assignNativeDevice(1,device);if(humanInputMode){DWORD local=primary&&primary!=device?primary:readWord(0x595570);if(local)assignNativeDevice(0,local);}log("NET selected virtual pad for logical slot1 device=%08lx",device);return readWord(0x6913a0)==device;}}
    return false;
}
// A terminal peer STOP is handled like the working gameplay STOP path: let the
// native scheduler unwind to its message pump and ordinary shutdown. ExitProcess
// inside a menu/VM callback skipped that path and hit stale native owner nodes.
static void campaignCloseFromPeer(const char* boundary,unsigned sequence){
    if(campaignPeerStop)return;
    campaignPeerStop=true;networkComplete=true;applyNetworkActions(0,{});applyNetworkActions(1,{});
    log("NET peer ended session normally %s=%u; deferred owned WM_CLOSE",boundary,sequence);
    if(!networkCloseRequested){
        if(!gameWindow||!PostMessageW(gameWindow,WM_CLOSE,0,0)){networkFailed=true;log("NET FAIL peer phase stop close error=%lu",GetLastError());}
        else{networkCloseRequested=true;log("NET peer phase stop WM_CLOSE posted; native callbacks unwind before teardown");}
    }
}
[[noreturn]] static void campaignReject(const char* reason){
    networkFailed=true;log("NET FAIL CAMPAIGN phase=%u reason=%s; native operation not forwarded",campaignPhaseSequence,reason);
    // ExitProcess ran the game's static destructors mid-frame and crashed at 4378DB
    // (runs 043/050/057/058). The log is already flushed; stop without game teardown.
    TerminateProcess(GetCurrentProcess(),87);ExitProcess(87);
}
static bool campaignArea(DWORD descriptor,char (&path)[33],DWORD& id){
    bool known=false;for(unsigned i=0;i<103;i++)if(readWord(0x584928+i*4)==descriptor){known=true;break;}
    DWORD name=0;if(!known||!readMemory((void*)(descriptor+4),&name,4)||!name||!readMemory((void*)(descriptor+8),&id,4)||id>=103)return false;
    for(unsigned i=0;i<33;i++){char c=0;if(!readMemory((void*)(name+i),&c,1))return false;if(!c){path[i]=0;return i>0;}if(i==32||!(isalnum((unsigned char)c)||c=='/'||c=='_'))return false;path[i]=c;}
    return false;
}
static unsigned campaignProgress(){BYTE progress[0x1fc]={};if(!readMemory((void*)0x69c470,progress,sizeof(progress)))campaignReject("unreadable shared progress");return coop::ProgressSample(progress);}
static coop::PhaseState campaignState(unsigned kind,unsigned id,unsigned selection){
    coop::PhaseState state={};state.kind=kind;state.id=id;state.selection=selection;state.flags=coop::phaseReady;state.content=networkContent;
    const char tag[]="fof-retail-menu-v1";memcpy(state.identity.data(),tag,sizeof(tag)-1);
    state.auxiliary[1]=campaignProgress();state.auxiliary[2]=readWord(0x69c624);state.auxiliary[4]=0;
    // Native operation 69EB0C (0 New Game, 2 load) only means something in the main menu and save list; a host that
    // Continued keeps 2 for the rest of the session while a joiner's New Game has 0 (auto-continue-409 hub mismatch).
    state.auxiliary[3]=kind==coop::phaseMenu&&(id==1||id==2)?readWord(0x69eb0c):0;
    state.auxiliary[5]=1;state.auxiliary[6]=readWord(readWord(0x69e72c)+8);state.auxiliary[7]=readWord(readWord(0x6a9940)+0x4e4);return state;
}
// Menus, hub and load barriers: the partner may be minutes behind (cutscenes and movies are not synced, so one player
// can skip what the other watches: run together-skiphost-188 timed out at the save notice after the opening). Wait
// with no time limit and without advancing this game (so both still pair the same calls in the same order: a
// running wait broke that, run 193), keeping the window responsive. Link loss or closing the game ends the wait.
// While this game waits for the partner (usually still watching a cutscene this player skipped), a small window over
// the game says so instead of a silent freeze; it closes when the wait ends. The text comes from the launcher in its
// language (COOP_PROBE_WAIT_TEXT); English otherwise. It never takes focus from the game.
static HWND campaignNoticeWindow=0;static wchar_t campaignNoticeText[512];
static LRESULT CALLBACK campaignNoticeProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam){
    if(message==WM_PAINT){PAINTSTRUCT paint;HDC dc=BeginPaint(window,&paint);RECT r;GetClientRect(window,&r);
        HBRUSH back=CreateSolidBrush(RGB(28,28,34));FillRect(dc,&r,back);DeleteObject(back);
        HFONT font=CreateFontW(-17,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");HGDIOBJ previous=SelectObject(dc,font);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(240,240,240));InflateRect(&r,-20,-18);DrawTextW(dc,campaignNoticeText,-1,&r,DT_CENTER|DT_WORDBREAK|DT_NOPREFIX);
        SelectObject(dc,previous);DeleteObject(font);EndPaint(window,&paint);return 0;}
    if(message==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    return DefWindowProcW(window,message,wParam,lParam);
}
static void campaignWaitNotice(bool show){
    if(!show){if(campaignNoticeWindow){DestroyWindow(campaignNoticeWindow);campaignNoticeWindow=0;log("CAMPAIGN WAIT_NOTICE hidden");}return;}
    if(campaignNoticeWindow)return;
    static bool registered=false;HINSTANCE instance=GetModuleHandleW(0);
    if(!registered){WNDCLASSW c={};c.lpfnWndProc=campaignNoticeProc;c.hInstance=instance;c.hCursor=LoadCursorW(0,(LPCWSTR)IDC_ARROW);c.lpszClassName=L"FofCoopWaitNotice";registered=RegisterClassW(&c)!=0;if(!registered)return;
        DWORD n=GetEnvironmentVariableW(L"COOP_PROBE_WAIT_TEXT",campaignNoticeText,512);
        if(!n||n>=512)wcscpy_s(campaignNoticeText,L"Waiting for your partner\x2026\nThey may still be watching a cutscene. The game continues as soon as they catch up.");}
    RECT game={0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};if(gameWindow)GetWindowRect(gameWindow,&game);
    int width=520,height=120,x=game.left+(game.right-game.left-width)/2,y=game.top+(game.bottom-game.top)/3-height/2;
    campaignNoticeWindow=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"FofCoopWaitNotice",L"Friend or Foe co-op",WS_POPUP|WS_BORDER,x,y,width,height,gameWindow,0,instance,0);
    if(campaignNoticeWindow){ShowWindow(campaignNoticeWindow,SW_SHOWNOACTIVATE);UpdateWindow(campaignNoticeWindow);log("CAMPAIGN WAIT_NOTICE shown");}
}
static bool campaignWaitPhase(unsigned sequence,const coop::PhaseState& state,coop::PhaseState& peer){
    if(campaignStopping())return false;
    if(networkWire->PhaseAvailable(sequence))return networkWire->ExchangePhase(sequence,state,peer);
    if(!networkWire->SendPhase(sequence,state))return false;
    ULONGLONG started=GetTickCount64(),reported=started;
    struct HideNotice{~HideNotice(){campaignWaitNotice(false);}}hide;
    for(;;){
        int got=networkWire->TryReceivePhase(sequence,peer);if(got>0)return true;if(got<0)return false;
        // Short waits (the partner a few frames behind) stay silent; only a real wait shows the notice.
        if(GetTickCount64()-started>=1500)campaignWaitNotice(true);
        MSG message;while(PeekMessageW(&message,0,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        ULONGLONG now=GetTickCount64();if(now-reported>=10000){reported=now;log("CAMPAIGN WAITING_FOR_PARTNER sequence=%u kind=%lu id=%lu seconds=%llu",sequence,state.kind,state.id,(now-started)/1000);}
        Sleep(5);
    }
}
static bool campaignMenuPending=false;static coop::PhaseState campaignMenuSent;static double campaignMenuSentAt=0;
static coop::PhaseState campaignExchange(const coop::PhaseState& state){
    if(campaignStopping())return state;
    if(!networkReady&&!openNetworkConnection())campaignReject("transport open");
    // A hub update still in flight (sent, not yet answered) is completed first, so both games keep pairing the same
    // sequence numbers; its content is no longer needed once the hub is left.
    if(campaignMenuPending){coop::PhaseState stale={};ULONGLONG began=GetTickCount64();int got=0;
        while((got=networkWire->TryReceivePhase(campaignPhaseSequence,stale))==0){MSG message;while(PeekMessageW(&message,0,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}if(GetTickCount64()-began>=1500)campaignWaitNotice(true);Sleep(2);}
        campaignWaitNotice(false);campaignMenuPending=false;if(got<0){if(campaignStopping())return state;if(campaignPartnerLost(networkWire->PeerStopped()?"partner closed the game":"connection lost in a menu"))return state;if(networkWire->PeerStopped()){campaignCloseFromPeer("phase",campaignPhaseSequence);return state;}campaignReject("phase transport failure");}
        ++campaignPhaseSequence;}
    coop::PhaseState peer={};unsigned sequence=campaignPhaseSequence++;double began=milliseconds();
    bool exchanged=campaignWaitPhase(sequence,state,peer);if(exchanged)phaseWaits.add(milliseconds()-began);
    if(!exchanged){
        // This player closed the game while waiting (the wait keeps handling window messages): a normal end.
        if(campaignStopping()){networkComplete=true;log("NET local/pending close during a partner wait phase=%u; normal end",sequence);return state;}
        // The caller sees its own state echoed; from the next frame on the host is an ordinary solo game.
        if(campaignPartnerLost(networkWire->PeerStopped()?"partner closed the game":"connection lost in a menu"))return state;
        if(networkWire->PeerStopped()){campaignCloseFromPeer("phase",sequence);return state;}
        log("NET FAIL CAMPAIGN phase transport=%s",networkWire->Error().c_str());campaignReject("phase transport failure or timeout");
    }
    if(networkWire->PeerStopped()){
        if(campaignPartnerLost("partner closed the game"))return state;
        campaignCloseFromPeer("phase",sequence);return state;
    }
    if(!coop::CampaignPhaseValid(peer))campaignReject("invalid phase metadata");return peer;
}
// Menu/hub sync without blocking: send this side's state once, then poll every frame while the game keeps running
// with neutral input. A partner still watching a cutscene this player skipped (each PC plays cutscenes on its own)
// is waited for without a time limit; a dropped link still ends the wait at once.
static bool campaignMenuExchange(coop::PhaseState& state,coop::PhaseState& peer){
    if(campaignStopping())return false;
    if(!networkReady&&!openNetworkConnection())campaignReject("transport open");
    unsigned sequence=campaignPhaseSequence;int got=0;
    if(!campaignMenuPending){
        if(!networkWire->SendPhase(sequence,state))got=-1;else{campaignMenuPending=true;campaignMenuSent=state;campaignMenuSentAt=milliseconds();}
    }
    if(got==0)got=networkWire->TryReceivePhase(sequence,peer);
    if(got==0){static unsigned waits=0;if(waits++%600==0)log("CAMPAIGN MENU_WAIT sequence=%u kind=%lu (partner not there yet)",sequence,campaignMenuSent.id);return false;}
    if(got<0){
        campaignMenuPending=false;if(campaignStopping())return false;
        if(campaignPartnerLost(networkWire->PeerStopped()?"partner closed the game":"connection lost in a menu"))return false;
        if(networkWire->PeerStopped()){campaignCloseFromPeer("phase",sequence);return false;}
        log("NET FAIL CAMPAIGN phase transport=%s",networkWire->Error().c_str());campaignReject("phase transport failure or timeout");
    }
    if(networkWire->PeerStopped()){
        campaignMenuPending=false;if(campaignPartnerLost("partner closed the game"))return false;
        campaignCloseFromPeer("phase",sequence);return false;
    }
    campaignMenuPending=false;campaignPhaseSequence++;state=campaignMenuSent;hubRoundSamples[hubRoundCount++%256]=(float)(milliseconds()-campaignMenuSentAt);
    if(!coop::CampaignPhaseValid(peer))campaignReject("invalid phase metadata");return true;
}
static void campaignExactBarrier(coop::PhaseState state){
    auto peer=campaignExchange(state);if(campaignStopping())return;if(!coop::CampaignPhaseEqual(state,peer))campaignReject("phase identity/selection/progress mismatch");
    state.flags=coop::phaseReady|coop::phaseConfirmed;peer=campaignExchange(state);if(campaignStopping())return;
    if(!coop::CampaignPhaseEqual(state,peer))campaignReject("phase confirmation mismatch");
}
// Hub walking is read by the character controller outside the hub scene update,
// so the last mutually exchanged host stick bits are re-applied after each clear.
static unsigned campaignHubMovement=0,campaignHubMovement2=0,campaignHubPairedCalls=0;static bool campaignHubPlayer2=false;
// Hub jump/attack (owner report 2026-10-24: no jump or attack on the keyboard in the Helicarrier): character actions
// besides walking are re-applied like the walk. Excluded: d-pad 0..3 (walk), 4 back key, 5 Start, 11 Back, 13 character
// switch, 16..19 walk, 20..23 camera. Action 10 is jump and also Accept: it is only applied away from the terminals,
// where Accept keeps its mutually confirmed path.
static const unsigned campaignHubActionMask=(1u<<6)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<12)|(1u<<14)|(1u<<15)|(1u<<24)|(1u<<25)|(1u<<26)|(1u<<27);
static unsigned campaignHubActions=0,campaignHubActions2=0;static bool campaignHubAtTerminal=false;
static bool campaignHostClick=false;  // host's left click in a menu, consumed as one Accept (campaignFreshChoices)
// Research (COOP_PROBE_HUB_JUMP_TEST=1): the host bot taps jump away from the terminals; P1 height is logged.
static bool campaignHubJumpTest=false;
static bool campaignHubP2Position(float* xyz);
// P1 walks with the host's exchanged stick bits; with the hub sidekick (research) P2 walks with the joiner's.
static void campaignApplyHubMovement(){coop::Actions shared={},second={};for(unsigned i=16;i<20;i++){if(campaignHubMovement&(1u<<i))shared[i]=1;if(campaignHubPlayer2&&(campaignHubMovement2&(1u<<i)))second[i]=1;}
    const unsigned allowed=campaignHubActionMask|(campaignHubAtTerminal?0u:1u<<10);for(unsigned i=0;i<28;i++){if(campaignHubActions&allowed&(1u<<i))shared[i]=1;if(campaignHubPlayer2&&(campaignHubActions2&allowed&(1u<<i)))second[i]=1;}applyNetworkActions(0,shared);applyNetworkActions(1,second);}
static void campaignNeutralMenu(){
    // Clear native consumers without changing replay's private held-key history.
    for(unsigned slot=0;slot<2;slot++){
        memset((void*)(0x691170+slot*0x11c+4),0,28*4);memset((void*)(0x691170+slot*0x11c+0xe4),0,8*4);
        DWORD wrapper=readWord(0x595390+slot*4),mapped=readWord(wrapper+0x3c);if(wrapper&&mapped&&readWord(wrapper)>=28)memset((void*)mapped,0,sizeof(float)*28);
    }
}
static unsigned campaignMenuBits(const coop::Actions& actions){unsigned bits=0;for(unsigned i=0;i<28;i++)if(actions[i]>0.5f)bits|=1u<<i;return bits;}
static DWORD campaignMenuContext=0;static unsigned campaignMenuKind=0,campaignMenuCalls[16]={};
// Hub sidekick computer (trigger SidekickSelect): its focus value (scene+0x84) observed in runs sidekick-294/301.
static const unsigned campaignSidekickTerminal=0xdceff376u;
// Sidekick Select screen (scene 42, table 56F298, update 4A6C6F): grid row at [scene+0x88]+0x8D08, sidekick id per row
// in table 55AABC (13 rows), unlocked flags at 69C54B+id; Accept writes 69C624 (4A6D0A). It is player 2's character,
// so the joiner moves the cursor and chooses; the host only opens it at the computer.
static bool campaignSidekickRowAvailable(unsigned row){if(row>=13)return false;BYTE open=0;readMemory((void*)(0x69c54b+readWord(0x55aabc+row*4)),&open,1);return open!=0;}
static bool campaignMainAcceptObserved=false;
static bool campaignAutosaveArmed=false,campaignAutosaveAccepted=false;static DWORD campaignAutosaveCaption=0,campaignAutosaveProfile=0;
using CampaignUpdate=void(__thiscall*)(void*,float);
static CampaignUpdate originalCampaignMain=0,originalCampaignProfile=0,originalCampaignHub=0,originalCampaignMission=0,originalCampaignSaveAck=0,originalCampaignAutosaveNotice=0;
static DWORD campaignMissionGraphHash(){DWORD result=0;
    __asm{
        mov edx,0x56c59c
        mov eax,0x40df5e
        call eax
        mov result,eax
    }
    return result;
}
static bool campaignMissionTarget(unsigned row,char (&path)[33],DWORD& area){
    // Exact49F113..49F140 descriptor expression, observed without changing the
    // selector or deferring its destructive49F107 deactivation callback.
    DWORD region=readWord(0x69ecf0);if(region>=5||row>=4)return false;
    DWORD regionObject=readWord(0x584908+region*4),levels=readWord(regionObject+0x10),level=readWord(levels+row*4);
    DWORD index=readWord(0x69eaa0+(region*4+row)*4),areas=readWord(level+0x10);if(index>=103||!areas)return false;
    return campaignArea(readWord(areas+index*4),path,area);
}
static coop::CampaignSaveAckFacts campaignSaveAckFacts(DWORD scene,unsigned row){
    coop::CampaignSaveAckFacts facts={};BYTE active=0;DWORD handler=0,storage=0,table=0;
    facts.profileInitialized=campaignProfile;facts.row=row;facts.slot=readWord(0x6a78cc)==campaignSlot?0u:~0u;facts.operation=readWord(0x69eb0c);
    facts.exactSceneType=readWord(scene)==0x56f090;
    if(!readMemory((void*)(scene+0x7a),&active,1)||!readMemory((void*)(scene+0xa4),&facts.close,4)||
       !readMemory((void*)(scene+0x98),&handler,4)||!handler||!readMemory((void*)handler,&table,4))return facts;
    facts.active=active!=0;facts.exactTaskType=table==0x57e6d8;facts.handlerIsNativeTask=handler==readWord(0x69ed3c);
    if(!facts.exactTaskType||!facts.handlerIsNativeTask||!readMemory((void*)(handler+4),&storage,4)||!storage||
       !readMemory((void*)(handler+0x38),&facts.state,4)||!readMemory((void*)(handler+0x3c),&facts.uiWord,4)||
       !readMemory((void*)(storage+4),&facts.storageToken,4))return facts;
    facts.readable=true;return facts;
}
static coop::CampaignAutosaveNoticeFacts campaignAutosaveNoticeFacts(DWORD scene,unsigned row){
    coop::CampaignAutosaveNoticeFacts facts={};BYTE active=0,forcedClose=1,styleFlag=0;DWORD list=readWord(scene+0x8c);
    facts.sourceAttested=campaignAutosaveArmed;facts.captionMatches=campaignAutosaveCaption&&readWord(0x69ea84)==campaignAutosaveCaption;
    facts.profileInitialized=campaignProfile;facts.exactSceneType=readWord(scene)==0x56c1c0;facts.exactProfileType=readWord(campaignAutosaveProfile)==0x56efe8;facts.row=row;
    if(!list||!readMemory((void*)(scene+0x7a),&active,1)||!readMemory((void*)(list+0x8d04),&facts.count,4)||
       !readMemory((void*)0x6a78cc,&facts.slot,4)||(facts.slot=facts.slot==campaignSlot?0u:~0u,false)||!readMemory((void*)0x69eb0c,&facts.operation,4)||
       !readMemory((void*)(campaignAutosaveProfile+0x90),&facts.profileState,4)||
       !readMemory((void*)0x587310,&facts.label0,4)||!readMemory((void*)0x587314,&facts.label1,4)||!readMemory((void*)0x587318,&facts.label2,4)||
       !readMemory((void*)0x69ea73,&forcedClose,1)||!readMemory((void*)0x69ea90,&styleFlag,1))return facts;
    facts.active=active!=0;facts.forcedClose=forcedClose;facts.styleFlag=styleFlag;facts.readable=true;return facts;
}
static void __cdecl campaignObserveAutosaveConfigure(DWORD profile,DWORD caption){
    // EBX is Profile39 at the sole4A554D call after resolving the localized
    // MEM_AUTOSAVE_PROMPT resource. EAX remains local and never enters the wire.
    // A game played alone (opened, not yet linked) shows its own notice untouched.
    if(!campaignLive())return;
    if(!campaignProfile||campaignAutosaveArmed||campaignAutosaveAccepted||!caption||readWord(profile)!=0x56efe8||
       readWord(profile+0x90)!=5||readWord(0x6a78cc)!=campaignSlot||readWord(0x69eb0c)!=0)campaignReject("autosave notice source precondition");
    campaignAutosaveCaption=caption;campaignAutosaveProfile=profile;campaignAutosaveArmed=true;
    log("CAMPAIGN AUTOSAVE_NOTICE source=MEM_AUTOSAVE_PROMPT configured=1");
}
static DWORD campaignNoticeConfigureAddress=0x49a5cd;
static void __declspec(naked) campaignAutosaveConfigureBridge(){
    __asm{
        pushfd
        pushad
        push eax
        push ebx
        call campaignObserveAutosaveConfigure
        add esp,8
        popad
        popfd
        jmp dword ptr [campaignNoticeConfigureAddress]
    }
}
// The hub is a walked 3D room: a level script sets scene+84 (49C840) when P1
// enters a terminal trigger. Host-only fixture: learn the stick->world mapping,
// then walk P1 into each named "Level*" trigger until focus is the mission terminal.
static bool campaignHubPlayer(float (&xyz)[3]){DWORD actor=readWord(readWord(0x6a9900)+0x12c),transform=actor?readWord(actor+0x1d0):0;
    return transform&&readWord(readWord(actor)+0x2f4)==0x45a6c9&&readMemory((void*)transform,xyz,12)&&std::isfinite(xyz[0])&&std::isfinite(xyz[1]);}
static bool campaignTriggerName(DWORD trigger,char (&name)[33]){memset(name,0,sizeof(name));if(readWord(trigger)!=0x575f20||!readMemory((void*)(trigger+0x80),name,32))return false;name[32]=0;
    for(unsigned i=0;name[i];i++)if(name[i]<0x20||name[i]>0x7e)return false;return name[0]!=0;}
static bool campaignSidekickTest=false,campaignSidekickVisited=false;static unsigned campaignSidekickFocus=0;
// Hub walking shares the stick actions 16..19, re-applied after every native input update. The keyboard's Laptop
// layout walks with the d-pad actions 0..3 instead (run menu-settings-018 bindings: W/S/A/D and arrows on 0..3, none
// on 16..19), so a d-pad direction counts as the matching stick direction there (0 up=16, 1 down=17, 2 left=18, 3 right=19).
static unsigned campaignWalkBits(unsigned bits){return (bits&0xf0000u)|((bits&0xfu)<<16);}
// Research (COOP_PROBE_HUB_WALK_DPAD=1): the hub bot walks with the d-pad actions, as a Laptop-layout keyboard does.
static bool campaignHubWalkDpad=false;
static unsigned campaignHubWalkBits(unsigned focus,unsigned calls){
    static unsigned phase=0,phaseStart=0,tried=0,targetStart=0,arrived=0,bestAt=0,detourUntil=0,detours=0;static int target=-1;static float start[3],axisX[2],axisY[2],best=1e9f;
    if(campaignHubWalkReset){campaignHubWalkReset=false;tried=0;target=-1;arrived=0;}
    float p[3]={};if(!campaignHubPlayer(p))return 0;
    // At the mission terminal: Accept, and keep stepping toward its centre. The hub is not in lockstep, so the partner's
    // copy of P1 can stand a few cm off; at the edge of the volume only this PC saw the terminal (level-regions-461).
    bool missionFocus=focus&&focus==campaignMissionGraphHash();
    if(missionFocus&&(calls%120==60||phase!=3||target<0))return calls%120==60?1u<<10:0;
    if(phase==0){memcpy(start,p,sizeof(start));phase=1;phaseStart=calls;}
    if(phase==1){if(calls-phaseStart<30)return 1u<<19;axisX[0]=p[0]-start[0];axisX[1]=p[1]-start[1];memcpy(start,p,sizeof(start));phase=2;phaseStart=calls;}
    if(phase==2){if(calls-phaseStart<30)return 1u<<16;axisY[0]=p[0]-start[0];axisY[1]=p[1]-start[1];phase=3;
        log("CAMPAIGN HUB_WALK calibrated right=%.4g,%.4g up=%.4g,%.4g",axisX[0],axisX[1],axisY[0],axisY[1]);
        // A measurement taken while the game is busy (join handshake, a warp) is implausible: measure again (run
        // net-bad-rejoin-439 under fake latency calibrated right=12.33 m in 30 frames, up=0, and gave up for good).
        float rx=std::hypot(axisX[0],axisX[1]),ry=std::hypot(axisY[0],axisY[1]);static unsigned recalibrations=0;
        if(rx<0.05f||ry<0.05f||rx>6.0f||ry>6.0f){if(++recalibrations<=20){log("CAMPAIGN HUB_WALK calibration implausible, measuring again (%u)",recalibrations);phase=0;}else{log("CAMPAIGN HUB_WALK stick did not move P1");phase=9;}}}
    if(phase!=3)return 0;
    DWORD pool=readWord(0x6a9970),count=readWord(pool+8),items=readWord(pool+4);if(count>256)return 0;
    if(target<0||(tried>>target)&1){target=-1;
        // Run 048: LevelSelect01 carries comOvlMissionSelect on a new profile; try it first.
        // Research (COOP_PROBE_HUB_SIDEKICK_TEST): visit the sidekick computer first.
        if(campaignSidekickTest&&!campaignSidekickVisited)for(unsigned i=0;i<count&&i<32;i++){char name[33];if(campaignTriggerName(readWord(items+i*4),name)&&!strcmp(name,"SidekickSelect")){target=(int)i;break;}}
        for(int pass=0;pass<2&&target<0;pass++)for(unsigned i=0;i<count&&i<32;i++){char name[33];if(!((tried>>i)&1)&&campaignTriggerName(readWord(items+i*4),name)&&(pass?!strncmp(name,"Level",5):!strcmp(name,"LevelSelect01"))){target=(int)i;break;}}
        if(target<0){log("CAMPAIGN HUB_WALK no untried Level trigger left");phase=9;return 0;}
        char name[33];campaignTriggerName(readWord(items+target*4),name);targetStart=bestAt=calls;arrived=detourUntil=0;best=1e9f;log("CAMPAIGN HUB_WALK target index=%d name=%s from=%.4g,%.4g",target,name,p[0],p[1]);}
    DWORD trigger=readWord(items+target*4);
    if(focus&&campaignSidekickTest&&!campaignSidekickVisited){char name[33];campaignTriggerName(trigger,name);
        if(!strcmp(name,"SidekickSelect")){static unsigned pressed=0;if(calls%120==60){++pressed;campaignSidekickFocus=focus;campaignSidekickVisited=true;log("CAMPAIGN SIDEKICK_TEST focus=%08x accept",focus);campaignCaptureAt=(LONG)presentCalls;return 1u<<10;}return 0;}}
    if(focus==campaignSidekickTerminal&&campaignSidekickVisited)focus=0; // still standing at the sidekick computer
    if(focus&&!missionFocus){char name[33];campaignTriggerName(trigger,name);log("CAMPAIGN HUB_WALK focus=%08x at target=%s is not the mission terminal",focus,name);tried|=1u<<target;return 0;}
    float d[2]={readFloat(trigger+0x50)-p[0],readFloat(trigger+0x54)-p[1]},distance=std::hypot(d[0],d[1]);
    if(calls-targetStart>1800){log("CAMPAIGN HUB_WALK timeout target=%d distance=%.4g detours=%u",target,distance,detours);tried|=1u<<target;return 0;}
    float feet[3]={p[0],p[1],p[2]+readFloat(0x55b520)};
    if(missionFocus&&distance<0.6f)return 0;
    if(!missionFocus&&(triggerContains(trigger,feet)||distance<0.3f)){if(!arrived){arrived=calls;log("CAMPAIGN HUB_WALK inside target=%d distance=%.4g",target,distance);}if(calls-arrived>120){log("CAMPAIGN HUB_WALK no focus at target=%d",target);tried|=1u<<target;}return 0;}
    if(distance<best-0.1f){best=distance;bestAt=calls;}
    if(calls>=detourUntil&&calls-bestAt>90){detourUntil=calls+40;bestAt=calls+40;++detours;log("CAMPAIGN HUB_WALK detour target=%d distance=%.4g count=%u",target,distance,detours);}
    if(calls<detourUntil){float side=(detours&1)?1.0f:-1.0f,t=d[0];d[0]=-d[1]*side;d[1]=t*side;}
    else if(distance<1.0f&&calls%3)return 0;
    float bestScore=-2;unsigned bits=0;
    for(int sx=-1;sx<=1;sx++)for(int sy=-1;sy<=1;sy++){if(!sx&&!sy)continue;float v[2]={sx*axisX[0]+sy*axisY[0],sx*axisX[1]+sy*axisY[1]},n=std::hypot(v[0],v[1]);if(n<1e-4f)continue;
        float score=(v[0]*d[0]+v[1]*d[1])/(n*distance);if(score>bestScore){bestScore=score;bits=(sx>0?1u<<19:sx<0?1u<<18:0)|(sy>0?1u<<16:sy<0?1u<<17:0);}}
    return bits;
}
// The launcher writes <sandbox>\connect.json ({"role":"host"} or {"role":"join"}) atomically once
// Steam has a validated partner. Until then campaign menus stay frozen and nothing is exchanged.
// 528BC0 reads the DirectInput mouse (595980..595990, deltas 59599C/5959A0) and the absolute
// cursor (595994/595998) that menus use for hover. Mouse input is neither exchanged nor applied
// on the partner's PC, so a local hover moved one seat's menu row (runs 043/067: slot row 2).
// In co-op campaign play the game sees a parked, motionless, unpressed mouse.
// Co-op parks the mouse: a native click would confirm a menu on one PC only. The host keeps the pointer in menus
// (owner request 2026-10-24: "make at least only host can move cursor"): hovering moves the host's menu cursor (the
// partner's lists follow the host's row) and a left click becomes the host's Accept, which both games still confirm.
// Mission dialogs use the delayed host pointer; gameplay and the partner's local mouse stay parked.
static void applyMissionPointer(){
    memset((void*)0x595980,0,0x14);*(LONG*)0x59599c=0;*(LONG*)0x5959a0=0;*(LONG*)0x595994=-32000;*(LONG*)0x595998=-32000;
    DWORD mode[3]={},scene=readWord(readWord(0x6a9940)+0x4e4),movie=readWord(0x6a9980);
    if(!captureGameMode(mode)||!coop::MissionMenuActive(readWord(readWord(0x69e72c)+8),mode[0],scene,movie&&readWord(movie+4)==1)||!(appliedMenuPointer.flags&coop::menuPointerActive))return;
    RECT bounds={};POINT position={};if(!gameWindow||!GetClientRect(gameWindow,&bounds)||bounds.right<=1||bounds.bottom<=1)return;
    position.x=(LONG)((ULONGLONG)appliedMenuPointer.x*(bounds.right-1)/65535);position.y=(LONG)((ULONGLONG)appliedMenuPointer.y*(bounds.bottom-1)/65535);
    if(ClientToScreen(gameWindow,&position)){*(LONG*)0x595994=position.x;*(LONG*)0x595998=position.y;}
}
static void __cdecl campaignMouseUpdate(){
    ((void(__cdecl*)())0x528bc0)();if(!campaignLive())return;
    if(readWord(readWord(0x69e72c)+8)==128){
        static bool wasDown=false;bool down=(*(BYTE*)0x59598c&0x80)!=0;
        unsigned pendingClick=localMenuPointer.flags&coop::menuPointerClick;localMenuPointer={};
        if(networkRole==1&&!campaignFixture){RECT bounds={};POINT position={*(LONG*)0x595994,*(LONG*)0x595998};
            if(gameWindow&&ScreenToClient(gameWindow,&position)&&GetClientRect(gameWindow,&bounds)&&bounds.right>1&&bounds.bottom>1&&position.x>=0&&position.y>=0&&position.x<bounds.right&&position.y<bounds.bottom){
                localMenuPointer.x=(unsigned)((ULONGLONG)position.x*65535/(bounds.right-1));localMenuPointer.y=(unsigned)((ULONGLONG)position.y*65535/(bounds.bottom-1));
                // Mouse polling can run between simulation ticks while the network stalls.
                // Keep one pending click until networkInput consumes it.
                localMenuPointer.flags=coop::menuPointerActive|pendingClick|((down&&!wasDown)?coop::menuPointerClick:0u);
            }}
        wasDown=down;applyMissionPointer();return;
    }
    DWORD flow=readWord(0x69e72c);bool hostMenus=networkRole==1&&!campaignFixture&&flow&&readWord(flow+8)!=128;
    if(hostMenus){static bool wasDown=false;bool down=(*(BYTE*)0x59598c&0x80)!=0;if(down&&!wasDown)campaignHostClick=true;wasDown=down;
        memset((void*)0x595988,0,0xc);return;}   // wheel and buttons cleared; position and movement kept
    memset((void*)0x595980,0,0x14);*(LONG*)0x59599c=0;*(LONG*)0x5959a0=0;*(LONG*)0x595994=-32000;*(LONG*)0x595998=-32000;
}
static bool campaignTryConnect(){
    if(!networkDeferred)return true;
    static ULONGLONG lastPoll=0;ULONGLONG now=GetTickCount64();if(now-lastPoll<250)return false;lastPoll=now;
    static bool waitingLogged=false;if(!waitingLogged){waitingLogged=true;log("NET DEFERRED_WAIT solo play until Host/Join connects a partner");}
    wchar_t path[32768];_snwprintf(path,32768,L"%s\\connect.json",sandbox);
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);if(file==INVALID_HANDLE_VALUE)return false;
    char text[128]={};DWORD read=0;BOOL ok=ReadFile(file,text,sizeof(text)-1,&read,0);CloseHandle(file);if(!ok)return false;
    unsigned role=strstr(text,"\"role\":\"host\"")?1u:strstr(text,"\"role\":\"join\"")?2u:0u;
    if(!role)campaignReject("invalid deferred connect file");
    DeleteFileW(path);
    networkRole=role;campaignJoinPending=true;networkDeferred=false;log("NET DEFERRED_CONNECT role=%u join_at_hub=1",role);
    if(!openNetworkConnection())campaignReject("deferred transport open");
    return true;
}
// Progress travels in eight 64-byte phase payloads (identity 32 + selection 4 + auxiliary[1..7] 28).
static void campaignJoinPayload(coop::PhaseState& state,unsigned part,const BYTE* source){
    BYTE chunk[64]={};unsigned offset=part*64,count=offset<0x1fc?(0x1fcu-offset<64u?0x1fcu-offset:64u):0u;if(count&&source)memcpy(chunk,source+offset,count);
    memcpy(state.identity.data(),chunk,32);memcpy(&state.selection,chunk+32,4);memcpy(&state.auxiliary[1],chunk+36,28);
}
static void campaignJoinUnpack(const coop::PhaseState& state,unsigned part,BYTE* target){
    BYTE chunk[64]={};memcpy(chunk,state.identity.data(),32);memcpy(chunk+32,&state.selection,4);memcpy(chunk+36,&state.auxiliary[1],28);
    unsigned offset=part*64,count=offset<0x1fc?(0x1fcu-offset<64u?0x1fcu-offset:64u):0u;if(count)memcpy(target+offset,chunk,count);
}
static coop::PhaseState campaignJoinState(unsigned id){coop::PhaseState state={};state.kind=coop::phaseLoad;state.id=id;state.flags=coop::phaseReady;state.content=networkContent;return state;}
static unsigned campaignBytesHash(const BYTE* bytes,unsigned count){unsigned hash=2166136261u;for(unsigned i=0;i<count;i++)hash=(hash^bytes[i])*16777619u;return hash;}
// Joining during a mission: the host, playing alone, remembers the area it is in and leaves to the Helicarrier the
// way the pause menu's "Exit to Helicarrier" does (4A2B2B: pending flow 0x100, no mission result). The friend joins
// there as usual; the join sends this area along, and right after JOIN_COMPLETE both games start it the way the
// mission computer does (49F143: requested area +1C, 69ECEC=2, pending flow 0x20), so both continue in lockstep from
// the start of that area. A boss arena (descriptor +1C kind 0) cannot be started directly (Tokyo 2 arena crashed at
// 45CE49), so its previous area (+14) is used.
static DWORD campaignResumeArea=0;
static void campaignHostLeaveMissionForJoin(){
    DWORD flow=readWord(0x69e72c),areas=readWord(0x6a9974),manager=readWord(0x6a9988);
    if(!campaignMode||!campaignJoinPending||networkRole!=1||campaignJoinSent||campaignResumeArea||!flow||readWord(flow+8)!=128||readWord(flow+0xc)!=128)return;
    if(!manager||readWord(manager+0x2c)!=0)return;   // wait for ordinary gameplay (no pause, cutscene or area change)
    DWORD area=readWord(areas+0x18);char path[33]={};DWORD id=0;if(!campaignArea(area,path,id)||id==97)return;
    if(readWord(area+0x1c)==0&&readWord(area+0x14)){DWORD previous=readWord(area+0x14);char before[33]={};DWORD beforeId=0;if(campaignArea(previous,before,beforeId)){area=previous;strcpy(path,before);id=beforeId;}}
    campaignResumeArea=area;*(DWORD*)(flow+0xc)=0x100;
    log("CAMPAIGN JOIN_IN_MISSION host leaves to the Helicarrier; both resume at %s (area %lu)",path,id);
}
static void campaignResumeAfterJoin(){
    if(!campaignResumeArea)return;
    auto state=campaignJoinState(0x30400);state.selection=campaignResumeArea;auto peer=campaignExchange(state);if(campaignStopping())return;
    if(peer.id!=state.id||peer.selection!=state.selection)campaignReject("join resume area mismatch");
    char path[33]={};DWORD id=0;if(!campaignArea(campaignResumeArea,path,id)||id==97)campaignReject("join resume area unknown");
    DWORD flow=readWord(0x69e72c);*(DWORD*)(readWord(0x6a9974)+0x1c)=campaignResumeArea;*(DWORD*)0x69ecec=2;*(DWORD*)(flow+0xc)=0x20;
    campaignResumeArea=0;campaignMission=true;campaignHubPartnerReset();strcpy(campaignMissionPath,path);campaignMissionArea=id;
    log("CAMPAIGN ASSERT MISSION_COMMIT path=%s area=%lu pending=32 resume=1",path,id);
}
// Host, in the hub: send the live progress block; the joiner answers each part from its main menu.
static void campaignHostSendJoin(){
    BYTE progress[0x1fc]={};if(!readMemory((void*)0x69c470,progress,sizeof(progress)))campaignReject("host progress read");
    for(unsigned part=0;part<8;part++){auto state=campaignJoinState(0x30000+part);campaignJoinPayload(state,part,progress);auto peer=campaignExchange(state);if(campaignStopping())return;if(peer.id!=state.id)campaignReject("join progress part mismatch");}
    auto done=campaignJoinState(0x30008);done.auxiliary[1]=campaignBytesHash(progress,sizeof(progress));done.auxiliary[2]=campaignProgress();done.auxiliary[3]=campaignResumeArea;auto peer=campaignExchange(done);if(campaignStopping())return;
    if(peer.id!=done.id||peer.auxiliary[1]!=done.auxiliary[1])campaignReject("join progress transfer checksum");
    campaignJoinSent=true;campaignSlot=readWord(0x6a78cc);campaignNewGame=campaignProfile=campaignHub=true;campaignMission=campaignGameplay=false;campaignJoined=true;campaignHubWalkReset=true;
    log("CAMPAIGN JOIN_SENT progress=%08x bytes=508",done.auxiliary[2]);
}
// Joiner, at its main menu: receive the host's progress once the host has started sending it.
static bool campaignJoinerReceive(){
    if(campaignJoinReceived)return true;if(!networkWire->PhaseAvailable(campaignPhaseSequence))return false;
    BYTE progress[0x1fc]={};
    for(unsigned part=0;part<8;part++){auto peer=campaignExchange(campaignJoinState(0x30000+part));if(campaignStopping())return false;
        if(part==0&&peer.id==0x30200){campaignJoinPending=false;campaignAutoTogether=true;networkWire->SetTimeout(120000);log("CAMPAIGN AUTO_TOGETHER joiner: host is in the main menu; starting together");return false;}
        if(peer.id!=0x30000+part)campaignReject("join progress part order");campaignJoinUnpack(peer,part,progress);}
    auto done=campaignJoinState(0x30008);done.auxiliary[1]=campaignBytesHash(progress,sizeof(progress));auto peer=campaignExchange(done);if(campaignStopping())return false;
    if(peer.id!=done.id||peer.auxiliary[1]!=done.auxiliary[1])campaignReject("join progress transfer checksum");
    memcpy(campaignJoinProgress,progress,sizeof(progress));campaignJoinSelected=peer.auxiliary[2];campaignJoinReceived=true;campaignJoined=true;
    {char path[33]={};DWORD id=0;campaignResumeArea=peer.auxiliary[3]&&campaignArea(peer.auxiliary[3],path,id)&&id!=97?peer.auxiliary[3]:0;if(campaignResumeArea)log("CAMPAIGN JOIN_IN_MISSION the host is in a mission; resuming at %s after the join",path);}
    log("CAMPAIGN JOIN_RECEIVED progress=%08x; starting New Game locally",campaignJoinSelected);return true;
}
// Both in the hub with equal progress: from here on every choice is exchanged again.
static void campaignJoinReady(){
    auto state=campaignJoinState(0x30100);state.auxiliary[1]=campaignProgress();state.auxiliary[2]=readWord(0x69c624);auto peer=campaignExchange(state);if(campaignStopping())return;
    if(peer.id!=state.id||peer.auxiliary[1]!=state.auxiliary[1]||peer.auxiliary[2]!=state.auxiliary[2])campaignReject("joined hub progress mismatch");
    campaignJoinPending=false;networkWire->SetTimeout(30000);log("CAMPAIGN ASSERT JOIN_COMPLETE progress=%08x role=%u",state.auxiliary[1],networkRole);
}
// Native scene object for a scene id (registry at 6A9940+4: object, id pairs; count at +404).
static DWORD sceneObject(DWORD id){DWORD manager=readWord(0x6a9940),count=readWord(manager+0x404);if(!manager||count>128)return 0;for(DWORD i=0;i<count;i++)if(readWord(manager+8+i*8)==id)return readWord(manager+4+i*8);return 0;}
// Native list cursor (4AD688: ESI = menu list, stack = row; ret 4).
static void nativeListSelect(DWORD list,DWORD row){
    __asm {push esi
        mov esi,list
        push row
        mov eax,0x4ad688
        call eax
        pop esi}
}
static bool campaignSaveSlotUsed(unsigned row){
    wchar_t path[32768];_snwprintf(path,32768,L"%s\\Documents\\Activision\\Spider-Man FoF\\Saved Games\\SavedGame%u.SFOFSaveGame",profileRoot[0]?profileRoot:sandbox,row+1);
    DWORD attributes=GetFileAttributesW(path);if(attributes==INVALID_FILE_ATTRIBUTES){DWORD error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)campaignReject("cannot inspect package save slots");return false;}
    return true; // Directories and unfamiliar files also reserve a slot; never overwrite them for the partner.
}
// No free slot is a real outcome. Falling back to row zero would overwrite the partner's save.
static unsigned campaignFirstFreeSlot(){for(unsigned row=0;row<3;row++)if(!campaignSaveSlotUsed(row))return row;return 3;}
static bool campaignOverwritePending=false,campaignOverwriteArmed=false,campaignOverwriteAuthorized=false;
static DWORD campaignOverwriteProfile=0,campaignOverwriteCaption=0,campaignOverwriteLocalSlot=~0u;
static unsigned campaignOverwriteDecision=0;
static void campaignOverwriteReset(){campaignOverwritePending=campaignOverwriteArmed=campaignOverwriteAuthorized=false;campaignOverwriteProfile=campaignOverwriteCaption=0;campaignOverwriteLocalSlot=~0u;campaignOverwriteDecision=0;}
static bool campaignNativeSlotUsed(DWORD profile,unsigned row){
    BYTE used=0;DWORD list=readWord(profile+0x88);
    return row<3&&list&&readMemory((void*)(list+row*0x234+0xb4),&used,1)&&used!=0;
}
static coop::PhaseState campaignSlotState(unsigned id,unsigned row){
    auto state=campaignState(coop::phaseMenu,id,row);state.identity={};
    const char tag[]="fof-native-slot-choice-v2";memcpy(state.identity.data(),tag,sizeof(tag)-1);
    // Rows and slot occupancy belong to each PC. Confirm their semantic choice separately;
    // native scene ids differ while the host's overwrite dialog holds the partner's list.
    state.auxiliary[3]=readWord(0x69eb0c);for(unsigned i=4;i<8;i++)state.auxiliary[i]=0;return state;
}
static void campaignSlotPairGuard(const coop::PhaseState& own,const coop::PhaseState& peer){
    if(!coop::CampaignPhaseValid(peer)||peer.kind!=own.kind||peer.id!=own.id||peer.identity!=own.identity||peer.content!=own.content||
       peer.flags>coop::phaseReady||peer.auxiliary[1]!=own.auxiliary[1]||peer.auxiliary[2]!=own.auxiliary[2]||own.auxiliary[3]||peer.auxiliary[3])
        campaignReject("native save-slot choice metadata mismatch");
    if(networkRole==1&&peer.auxiliary[0])campaignReject("partner attempted a host-owned save-slot choice");
}
static void campaignSlotApply(unsigned bits,DWORD scene,unsigned kind){
    coop::Actions actions={};for(unsigned i=0;i<28;i++)if(bits&(1u<<i))actions[i]=1;
    applyNetworkActions(0,actions);applyNetworkActions(1,{});campaignMenuContext=scene;campaignMenuKind=kind;
}
static bool campaignSlotConfirm(coop::PhaseState choice){
    // The proposal was already exchanged. Confirm one canonical answer, like the
    // other menus, and stop immediately if that wait fell back to solo play.
    choice.flags=coop::phaseReady|coop::phaseConfirmed;auto peer=campaignExchange(choice);
    if(!networkRole||!networkReady||networkComplete||!campaignLive())return false;
    if(!coop::CampaignPhaseEqual(choice,peer))campaignReject("save-slot choice was not mutually confirmed");return true;
}
// Menu choices count only a fresh press: a key still held from the previous screen confirmed the next one (tester
// session 2026-10-24: Enter on New Game, held while the host waited for the partner, also confirmed the save slot).
// A host's left click in a menu counts as one Accept press (campaignMouseUpdate).
static unsigned campaignFreshChoices(unsigned physical){
    static unsigned previous=0;const unsigned choice=(1u<<10)|(1u<<11);unsigned fresh=physical&~previous&choice;previous=physical;
    if(campaignHostClick){fresh|=1u<<10;campaignHostClick=false;}
    return (physical&~choice)|fresh;
}
static const unsigned campaignSlotNavigation=0xfu|0xf0000u;
static unsigned campaignSlotHostBits(unsigned calls,unsigned selection,unsigned target,bool horizontal=false){
    if(!campaignFixture)return campaignFreshChoices(campaignMenuBits(campaignPhysicalMenu))&(campaignSlotNavigation|(1u<<10));
    if(selection!=target){
        if(calls<30||calls%20!=10)return 0;
        bool forward=selection==~0u||selection<target;
        // Save rows are vertical; the native Yes/No notice is horizontal.
        unsigned action=horizontal?readWord(readWord(0x69e56c)+(forward?11u:10u)*4):(forward?1u:0u);
        if(action>=28||!(campaignSlotNavigation&(1u<<action)))campaignReject("fixture native dialog navigation mapping");
        return 1u<<action;
    }
    return calls>=60&&calls%120==60?1u<<10:0;
}
static void campaignSlotRefuse(DWORD profile){
    log("CAMPAIGN SLOT_REFUSED reason=partner_has_no_empty_slots saves_unchanged=1");
    campaignOverwriteReset();campaignNewGame=false;campaignMainAcceptObserved=false;campaignMenuContext=0;campaignMenuKind=0;campaignNeutralMenu();
    // The native Profile39 Back branch calls this same ESI-based scene pop.
    __asm {push esi
        mov esi,profile
        mov eax,0x497932
        call eax
        pop esi}
    if(!campaignFixture)MessageBoxW(gameWindow,networkRole==2?
        L"You have no empty save slots. Co-op New Game was canceled. Your existing saves were kept.":
        L"Your partner has no empty save slots. Co-op New Game was canceled. Existing saves were kept.",
        L"Friend or Foe co-op",MB_OK|MB_ICONINFORMATION);
}
static bool campaignSlotMenu(DWORD scene){
    DWORD list=readWord(scene+0x88);unsigned row=list?readWord(list+0x8d08):~0u,freeRow=campaignFirstFreeSlot();
    if(readWord(scene)!=0x56efe8||readWord(0x69eb0c)!=0)campaignReject("native New Game slot list precondition");
    unsigned calls=++campaignMenuCalls[2],nav=0;
    const bool local=campaignLocalDrive();unsigned target=freeRow;
    if(!local&&networkRole==1)target=campaignSlotFixture<3?campaignSlotFixture:freeRow<3?freeRow:0;
    unsigned bits=0;
    if(local||networkRole==2){
        if(freeRow<3&&row!=freeRow&&calls>=30&&calls%20==10)nav=row==~0u||row<freeRow?1u<<1:1u<<0;
        if(local){
            if(freeRow>=3){campaignSlotRefuse(scene);return false;}
            if(row==freeRow&&calls>=60&&calls%120==60)nav=1u<<10;
            campaignSlotApply(nav,scene,2);return true;
        }
    }else bits=campaignSlotHostBits(calls,row,target);
    // Do not accept the row before the native navigation routine has applied its move.
    if(bits&campaignSlotNavigation)bits&=~(1u<<10);
    auto state=campaignSlotState(2,row);state.flags=(row<3&&(networkRole==1||row==freeRow))?coop::phaseReady:0;
    state.auxiliary[0]=bits;state.auxiliary[4]=campaignNativeSlotUsed(scene,row)?1u:0u;state.auxiliary[5]=freeRow;
    auto peer=campaignExchange(state);if(!networkRole||!networkReady||networkComplete||!campaignLive())return false;campaignSlotPairGuard(state,peer);
    static unsigned slotLogs=0;if(slotLogs<120&&(nav||(bits&campaignSlotNavigation)||calls%60==0)){++slotLogs;log("CAMPAIGN SLOT_SYNC role=%u selection=%ld free_row=%u used=%lu nav=%08x calls=%u own_ready=%u peer_ready=%u",networkRole,(LONG)row,freeRow,state.auxiliary[4],nav,calls,state.flags?1u:0u,peer.flags?1u:0u);}
    const auto& host=networkRole==1?state:peer;const auto& partner=networkRole==2?state:peer;
    if(partner.auxiliary[5]>=3){campaignSlotRefuse(scene);return false;}
    if(partner.flags&&(partner.selection!=partner.auxiliary[5]||partner.selection>=3||partner.auxiliary[4]))campaignReject("partner save slot is not empty");
    unsigned hostBits=host.auxiliary[0];
    if(hostBits&~(campaignSlotNavigation|(1u<<10)))campaignReject("invalid host save-slot actions");
    if((hostBits&campaignSlotNavigation)||!state.flags||!peer.flags)hostBits&=~(1u<<10);
    if(hostBits&(1u<<10)){
        if(host.selection>=3||host.auxiliary[4]>1)campaignReject("invalid native host save slot");
        auto choice=campaignSlotState(0x20100,host.selection);choice.auxiliary[0]=1u<<10;
        choice.auxiliary[4]=host.auxiliary[4];choice.auxiliary[5]=partner.selection;if(!campaignSlotConfirm(choice))return false;
        campaignOverwriteLocalSlot=row;campaignOverwriteProfile=scene;
        if(host.auxiliary[4]){
            campaignOverwritePending=true;campaignOverwriteArmed=campaignOverwriteAuthorized=false;campaignOverwriteDecision=0;campaignMenuCalls[9]=0;
            log("CAMPAIGN OVERWRITE_WAIT host_slot=%lu peer_slot=%lu role=%u",host.selection,partner.selection,networkRole);
            if(networkRole==2){campaignSlotApply(0,scene,2);return false;}
        }
    }
    campaignSlotApply(networkRole==1?hostBits:((hostBits&(1u<<10))?1u<<10:nav),scene,2);return true;
}
static bool campaignOverwriteReady(DWORD scene,unsigned row){
    DWORD list=readWord(scene+0x8c);BYTE active=0,forced=1,style=1;
    return campaignOverwritePending&&campaignOverwriteArmed&&networkRole==1&&readWord(scene)==0x56c1c0&&
        readWord(campaignOverwriteProfile)==0x56efe8&&readWord(campaignOverwriteProfile+0x90)==2&&
        readWord(readWord(campaignOverwriteProfile+0x88)+0x8d08)==campaignOverwriteLocalSlot&&campaignNativeSlotUsed(campaignOverwriteProfile,campaignOverwriteLocalSlot)&&
        !readWord(0x69eb0c)&&readWord(0x69ea84)==campaignOverwriteCaption&&list&&readWord(list+0x8d04)==2&&row<2&&
        readWord(0x587310)==2&&readWord(0x587314)==1&&readWord(0x587318)==~0u&&
        readMemory((void*)(scene+0x7a),&active,1)&&active&&readMemory((void*)0x69ea73,&forced,1)&&!forced&&
        readMemory((void*)0x69ea90,&style,1)&&!style;
}
// Host's real overwrite dialog pairs with the partner's held, empty native slot.
// The partner never creates a dummy dialog or changes its native used-slot flags.
static bool campaignOverwriteMenu(DWORD scene){
    unsigned row=networkRole==1?readWord(readWord(scene+0x8c)+0x8d08):~0u,calls=++campaignMenuCalls[9];
    auto state=campaignSlotState(9,row);state.flags=0;
    if(networkRole==1){state.flags=campaignOverwriteReady(scene,row)?coop::phaseReady:0;
        state.auxiliary[0]=campaignSlotHostBits(calls,row,campaignOverwriteFixtureNo?0u:1u,true);
        if(state.auxiliary[0]&campaignSlotNavigation)state.auxiliary[0]&=~(1u<<10);
    }else{
        DWORD profile=campaignOverwriteProfile,list=readWord(profile+0x88);
        BYTE active=0;bool ready=readWord(profile)==0x56efe8&&list&&readWord(list+0x8d08)==campaignOverwriteLocalSlot&&
            campaignOverwriteLocalSlot==campaignFirstFreeSlot()&&campaignOverwriteLocalSlot<3&&!campaignNativeSlotUsed(profile,campaignOverwriteLocalSlot)&&
            readMemory((void*)(profile+0x7a),&active,1)&&active;
        if(!ready)campaignReject("partner empty save slot changed during overwrite confirmation");state.flags=coop::phaseReady;
    }
    auto peer=campaignExchange(state);if(!networkRole||!networkReady||networkComplete||!campaignLive())return false;campaignSlotPairGuard(state,peer);
    const auto& host=networkRole==1?state:peer;unsigned bits=host.auxiliary[0];
    if(campaignFixture&&networkRole==1&&calls<=120&&calls%30==0){
        DWORD list=readWord(scene+0x8c);BYTE active=0,forced=0,style=0;readMemory((void*)(scene+0x7a),&active,1);readMemory((void*)0x69ea73,&forced,1);readMemory((void*)0x69ea90,&style,1);
        log("CAMPAIGN OVERWRITE_DETAIL calls=%u ready=%u peer_ready=%u row=%lu count=%lu profile_state=%lu slot=%lu used=%u caption_match=%u labels=%lu,%lu,%lu active=%u forced=%u style=%u bits=%08x",calls,state.flags,peer.flags,row,readWord(list+0x8d04),readWord(campaignOverwriteProfile+0x90),readWord(readWord(campaignOverwriteProfile+0x88)+0x8d08),campaignNativeSlotUsed(campaignOverwriteProfile,campaignOverwriteLocalSlot)?1u:0u,readWord(0x69ea84)==campaignOverwriteCaption?1u:0u,readWord(0x587310),readWord(0x587314),readWord(0x587318),active,forced,style,bits);
    }
    if(bits&~(campaignSlotNavigation|(1u<<10)))campaignReject("invalid overwrite dialog actions");
    if(!state.flags||!peer.flags)bits&=~(1u<<10);
    bool commit=false;
    if(bits&(1u<<10)){
        if(host.selection>1)campaignReject("invalid native overwrite answer");
        auto answer=campaignSlotState(0x20900,host.selection);answer.auxiliary[0]=1u<<10;if(!campaignSlotConfirm(answer))return false;
        campaignOverwriteDecision=host.selection+1;campaignOverwriteAuthorized=host.selection==1;commit=campaignOverwriteAuthorized;
        log("CAMPAIGN OVERWRITE_CHOICE answer=%s role=%u own_slot=%lu",commit?"yes":"no",networkRole,campaignOverwriteLocalSlot);
        if(networkRole==2&&!commit)campaignOverwriteReset();
    }
    campaignSlotApply(networkRole==1?bits:commit?1u<<10:0,scene?scene:campaignOverwriteProfile,9);return commit;
}
static void __cdecl campaignObserveSlotPrompt(DWORD profile,DWORD caption){
    if(!campaignLive()||!campaignOverwritePending||networkRole!=1)return;
    if(readWord(profile+0x90)!=2)return;
    if(profile!=campaignOverwriteProfile||readWord(profile)!=0x56efe8||readWord(0x69eb0c)||!caption||
       readWord(readWord(profile+0x88)+0x8d08)!=campaignOverwriteLocalSlot||!campaignNativeSlotUsed(profile,campaignOverwriteLocalSlot))
        campaignReject("native overwrite prompt source precondition");
    campaignOverwriteCaption=caption;campaignOverwriteArmed=true;log("CAMPAIGN OVERWRITE_PROMPT source=STATE_OVERWRITE_GAME_PROMPT configured=1 slot=%lu",campaignOverwriteLocalSlot);
}
static DWORD campaignSlotPromptAddress=0x49a5cd;
static void __declspec(naked) campaignSlotPromptBridge(){
    __asm {pushfd
        pushad
        push eax
        push edi
        call campaignObserveSlotPrompt
        add esp,8
        popad
        popfd
        jmp dword ptr [campaignSlotPromptAddress]}
}
static bool campaignMenuBegin(void* object,unsigned kind,DWORD listOffset){
    if(campaignStopping())return false;
    // A menu over the hub (mission list, sidekick screen, ...) must not see the walking sticks: the hub keeps applying
    // them every frame and the joiner's P2 walk moved the mission list cursor differently per seat (mismatch-367:
    // row 0 vs 2). Stop both walks while any other menu is up.
    if(kind!=3){campaignHubMovement=0;campaignHubMovement2=0;campaignHubActions=0;campaignHubActions2=0;}
    // A finished session closes its window next; no further phase exchanges.
    if(networkComplete){campaignNeutralMenu();return false;}
    if(campaignJoinPending){
        // The friend's own main menu stays free (settings) while it waits for the host; only New Game/Continue are held back.
        if(networkRole==2&&kind==1&&!campaignJoinReceived&&!campaignJoinerReceive())return false;
        if(kind==3){
            // Host waits frozen in the hub; the joiner arrives there after its local New Game.
            if(networkRole==1&&!networkWire->PhaseAvailable(campaignPhaseSequence)){campaignNeutralMenu();return true;}
            if(networkRole==2&&!campaignHub){campaignNeutralMenu();return true;}
            campaignJoinReady();if(campaignStopping())return false;campaignResumeAfterJoin();if(campaignStopping())return false;
        }
    }
    DWORD scene=(DWORD)(ULONG_PTR)object;BYTE active=0;if(!readMemory((void*)(scene+0x7a),&active,1)||!active)return false;
    if(kind==2)return campaignSlotMenu(scene);
    unsigned selection=0xffffffffu,available=1;DWORD list=0;
    if(listOffset){list=readWord(scene+listOffset);selection=list?readWord(list+0x8d08):0xffffffffu;if(kind==4&&selection<4){BYTE enabled=0;readMemory((void*)(list+selection*0x234+0xb5),&enabled,1);available=enabled?1:0;}
        if(kind==7)available=campaignSidekickRowAvailable(selection)?1:0;}
    else selection=kind==8?0u:readWord(scene+0x84);  // the popup's +84 is a pointer (differs per PC)
    auto state=campaignState(coop::phaseMenu,kind,selection);state.auxiliary[5]=available;
    float hubPosition[3]={};bool hubPositionValid=kind==3&&campaignHub&&!campaignMission&&campaignHubPlayer(hubPosition);
    if(kind==3){memcpy(&state.auxiliary[5],hubPosition,sizeof(hubPosition));
        // With a hub partner the joiner reports its player 2 instead (P2 is the joiner's character; the host follows it).
        float p2[3]={};if(campaignHubPlayer2&&networkRole==2&&campaignHubP2Position(p2))memcpy(&state.auxiliary[5],p2,sizeof(p2));}
    if(selection==0xffffffffu)state.flags=0;
    // Save slot list: saves are per PC, and New Game only initializes the first free slot (a later free row leaves
    // it invalid: runs 078/100). Each seat walks its own cursor there (actions 0 up, 1 down); the seats agree once
    // both are on their own first free slot, and only the host's Accept is shared.
    unsigned freeRow=campaignFirstFreeSlot(),slotNav=0;
    if(kind==2){
        if(selection!=freeRow){state.flags=0;unsigned step=campaignMenuCalls[2]+1;if(step>=30&&step%20==10)slotNav=selection==0xffffffffu||selection<freeRow?1u<<1:1u<<0;}
        state.selection=0;
    }
    if(kind==5){
        const auto facts=campaignSaveAckFacts(scene,selection);bool ready=coop::CampaignSaveAckReady(facts);
        state.identity={};const char tag[]="fof-native-slot0-save-ack-v1";memcpy(state.identity.data(),tag,sizeof(tag)-1);
        state.flags=ready?coop::phaseReady:0;state.auxiliary[5]=facts.uiWord&0xffu;state.auxiliary[6]=facts.state;state.auxiliary[7]=facts.exactTaskType?0x57e6d8:0;
        static unsigned previousReady=~0u;if(previousReady!=(ready?1u:0u)){previousReady=ready?1u:0u;log("CAMPAIGN SAVE_ACK ready=%u slot=%lu row=%u task_state=%lu ui_byte=%lu token=%lu exact_task=%u",ready?1u:0u,facts.slot,selection,facts.state,facts.uiWord&0xffu,facts.storageToken,facts.exactTaskType&&facts.handlerIsNativeTask?1u:0u);}
    }
    if(kind==6){
        const auto facts=campaignAutosaveNoticeFacts(scene,selection);bool ready=coop::CampaignAutosaveNoticeReady(facts);
        state.identity={};const char tag[]="MEM_AUTOSAVE_PROMPT";memcpy(state.identity.data(),tag,sizeof(tag)-1);
        state.flags=ready?coop::phaseReady:0;state.auxiliary[5]=facts.count;state.auxiliary[6]=facts.styleFlag|(facts.forcedClose<<8);state.auxiliary[7]=facts.profileState;
        static unsigned previousReady=~0u;if(previousReady!=(ready?1u:0u)){previousReady=ready?1u:0u;log("CAMPAIGN AUTOSAVE_NOTICE ready=%u row=%u count=%lu profile_state=%lu source_attested=%u caption_matches=%u",ready?1u:0u,selection,facts.count,facts.profileState,facts.sourceAttested?1u:0u,facts.captionMatches?1u:0u);}
    }
    char target[33]={};DWORD targetArea=~0u;
    if(kind==4&&selection<4){if(!campaignMissionTarget(selection,target,targetArea))campaignReject("invalid native mission selector descriptor");state.identity={};memcpy(state.identity.data(),target,strlen(target));state.auxiliary[7]=targetArea;}
    unsigned calls=++campaignMenuCalls[kind];unsigned bits=0;
    if(networkRole==1){
        if(campaignFixture){
            // Only validated default New Game, native slot0 and available first
            // mission choices. Unknown modal classes get no invented actions.
            if(calls>=60&&calls%120==60&&(kind==1||kind==2||kind==4||kind==5||kind==6||kind==8))bits=1u<<10;
            if(kind==2&&selection!=freeRow)bits=0;
            // The mission list fills its rows' availability a little after it opens; press only on an available row
            // (runs hubspawn-141/147 pressed too early: "native mission row is not available").
            if(kind==4&&(selection>=4||!available))bits=0;
            // With the hub walk stopped over menus (mismatch-367 fix) the list can open with no row highlighted
            // (together-376 sat there): step down onto a row; both seats move together since the host's bits are shared.
            if(kind==4&&selection>=4&&calls>=60&&calls%40==20)bits=1u<<1;
            // A fresh save can open the list on a locked row (together-391 sat on row 2): step up toward row 0.
            if(kind==4&&selection<4&&!available&&calls>=60&&calls%40==20)bits=1u<<0;
            if(kind==4&&calls%240==1)log("CAMPAIGN MISSION_LIST selection=%ld available=%u calls=%u",(LONG)selection,available,calls);
            if(kind==3&&calls>=60){bits=campaignHubWalkBits(selection,calls);if(campaignHubWalkDpad)bits=(bits&~0xf0000u)|((bits>>16)&0xfu);}
        }else{
            bits=campaignFreshChoices(campaignMenuBits(campaignPhysicalMenu));
            // A person drives these menus. A choice this co-op route cannot follow (anything but New Game, a used
            // save slot, which would ask to overwrite on this PC only, an unavailable mission, Accept away from the
            // hub terminal) simply does nothing, instead of stopping both games.
            bool allowed=true;
            if(kind==1)allowed=selection==0;
            if(kind==2)allowed=selection==freeRow;
            if(kind==3)allowed=selection==campaignMissionGraphHash()||selection==campaignSidekickTerminal;
            if(kind==4)allowed=selection<4&&available;
            if(kind==5)allowed=coop::CampaignSaveAckReady(campaignSaveAckFacts(scene,selection));
            if(kind==6)allowed=coop::CampaignAutosaveNoticeReady(campaignAutosaveNoticeFacts(scene,selection));
            if(!allowed)bits&=~(1u<<10);
            if(kind==1)bits&=~(1u<<11);
            // Hub pause (Resume, drop player 2, Exit) only fits one player's game: Start opened it on both and then
            // nobody's keys reached it (first two-PC test, 2026-10-24). Missions keep the shared native pause.
            if(kind==3)bits&=~(1u<<5);
        }
        if(kind==2||kind==5||kind==6||kind==8)bits&=1u<<10;
        if(kind==7)bits=0;
    }
    // The joiner drives the Sidekick Select screen (its own character): bot steps left to the first other unlocked
    // sidekick and accepts it; a person's input passes, except Accept on a locked row.
    if(kind==7&&networkRole==2){
        if(campaignFixture){static unsigned want=0xffffffffu;if(want==0xffffffffu){for(unsigned r=0;r<13;r++)if(r!=selection&&campaignSidekickRowAvailable(r)){want=r;break;}log("CAMPAIGN SIDEKICK_SELECT bot target row=%u from=%u",want,selection);}
            bits=0;if(calls>=60&&calls%40==20)bits=want>=13?1u<<11:selection==want?1u<<10:selection>want?1u<<2:1u<<3;}
        else{bits=campaignFreshChoices(campaignMenuBits(campaignPhysicalMenu));if(!available)bits&=~(1u<<10);}
    }
    if(campaignLocalDrive()){
        // Alone (test bot) or catching up (joiner): accept the game's own default rows with a slow local A.
        unsigned local=(kind!=3&&calls>=60&&calls%120==60)?1u<<10:0;
        // The slot list opens with no row (-1); up from there wraps to row 2, whose New Game init leaves the
        // slot invalid (076-080). Down selects row 0 from -1, up walks back from rows 1..2; confirm only row 0.
        // Menu rows move with actions 0 (up) and 1 (down), not the stick (run 088). The slot list may open on
        // row 2, whose New Game init leaves the slot invalid (076-086): walk up to row 0, confirm only there.
        if(kind==2&&selection!=0){local=(calls>=60&&calls%40==20)?(selection==0xffffffffu?1u<<1:1u<<0):0;}
        if(kind==5&&local&&!coop::CampaignSaveAckReady(campaignSaveAckFacts(scene,selection)))local=0;
        if(kind==6&&local&&!coop::CampaignAutosaveNoticeReady(campaignAutosaveNoticeFacts(scene,selection)))local=0;
        if(kind==2&&selection>2)local=0;
        // A joiner whose main menu row followed the host (auto together, then the host's Continue) starts New Game: row 0.
        if(kind==1&&selection!=0){local=0;if(list)nativeListSelect(list,0);}
        static unsigned localLogs=0;if(localLogs<200&&(local||calls%60==0)){++localLogs;log("CAMPAIGN LOCAL_DRIVE kind=%u selection=%ld calls=%u press=%08x",kind,(LONG)selection,calls,local);}
        coop::Actions shared={};for(unsigned i=0;i<28;i++)if(local&(1u<<i))shared[i]=1;applyNetworkActions(0,shared);applyNetworkActions(1,{});campaignMenuContext=scene;campaignMenuKind=kind;return true;
    }
    state.auxiliary[0]=bits;coop::PhaseState peer={};
    // Hub with a second character: the joiner's stick bits travel in aux4 (P2 movement); the bot joiner walks a little.
    if(kind==3){campaignHubAtTerminal=selection==campaignMissionGraphHash()||selection==campaignSidekickTerminal;
        unsigned physical=campaignFixture?0u:campaignMenuBits(campaignPhysicalMenu);if(campaignHubJumpTest&&networkRole==1&&!campaignHubAtTerminal&&calls%90<6)physical|=1u<<10;
        const unsigned actions=physical&(campaignHubActionMask|(1u<<10));
        if(campaignHubJumpTest&&hubPositionValid){static float lowest=1e9f,highest=-1e9f;if(hubPosition[2]<lowest)lowest=hubPosition[2];if(hubPosition[2]>highest)highest=hubPosition[2];
            if(calls%300==0)log("CAMPAIGN HUB_JUMP_TEST role=%u p1_height_range=%.3f",networkRole,highest-lowest);}
        if(networkRole==1){state.auxiliary[4]=actions;campaignHubActions=actions;}
        else if(campaignHubPlayer2){unsigned mine=campaignFixture?((calls/90)%4==1?1u<<16:(calls/90)%4==3?1u<<17:0u):campaignWalkBits(physical);state.auxiliary[4]=mine|actions;campaignHubActions2=actions;}}
    if(kind==3){
        // Hub without per-frame waits (first two-PC test, 2026-10-24: ~3 fps in the hub over the internet). Each game
        // sends its update and keeps running; this player's own walk applies at once, the partner's when it arrives;
        // Accept/Back still need the mutual confirmation below.
        if(networkRole==1)campaignHubMovement=campaignWalkBits(bits);else if(campaignHubPlayer2)campaignHubMovement2=campaignWalkBits(state.auxiliary[4]);
        // An Accept pressed while the previous update travels is held for the next one, if still at a terminal.
        static unsigned latched=0;const unsigned choice=(1u<<10)|(1u<<11);
        if(campaignMenuPending)latched|=bits&choice;
        else{if(networkRole==1&&(state.selection==campaignMissionGraphHash()||state.selection==campaignSidekickTerminal))state.auxiliary[0]|=latched;latched=0;}
        if(!campaignMenuExchange(state,peer)){if(campaignStopping())return false;campaignNeutralMenu();campaignMenuContext=scene;campaignMenuKind=kind;return true;}
        bits=state.auxiliary[0];
    }else peer=campaignExchange(state);
    if(campaignStopping())return false;
    if(state.kind!=peer.kind||state.id!=peer.id||state.content!=peer.content)campaignReject("unexpected active native menu phase");
    unsigned hostBits=networkRole==1?bits:peer.auxiliary[0];
    if(kind==7)hostBits=networkRole==2?bits:peer.auxiliary[0];
    else if(networkRole==1&&peer.auxiliary[0]!=0)campaignReject("peer attempted a host-owned menu choice");
    if(kind==7&&networkRole==2&&peer.auxiliary[0]!=0)campaignReject("host attempted the joiner's sidekick choice");
    // Hub focus follows each seat's own trigger entry; keep sharing movement while
    // focus differs, but never release Accept unless both report the same focus.
    // Hub positions (aux5..7) come from wall-clock frontend movement and drift a
    // little; the hub has no hostile/world state, so the peer follows the host's P1.
    bool hubFocusDiffers=kind==3&&state.selection!=peer.selection;
    auto agreement=coop::CampaignMenuAgreement(state,peer);
    if(kind==3){auto masked=peer;masked.selection=state.selection;masked.auxiliary[4]=state.auxiliary[4];for(unsigned i=5;i<8;i++)masked.auxiliary[i]=state.auxiliary[i];agreement=coop::CampaignMenuAgreement(state,masked);if(hubFocusDiffers)hostBits&=~((1u<<10)|(1u<<11));
        if(networkRole!=1&&hubPositionValid){float host[3];memcpy(host,&peer.auxiliary[5],sizeof(host));float drift=std::hypot(host[0]-hubPosition[0],host[1]-hubPosition[1]);
            if(std::isfinite(drift)&&drift>0.15f&&drift<20.0f){DWORD actor=readWord(readWord(0x6a9900)+0x12c);WORD heading=0;readMemory((void*)(readWord(actor+0x1d0)+12),&heading,2);
                ((void(__thiscall*)(void*,const float*,unsigned,bool))0x459fe4)((void*)actor,host,heading,true);static unsigned corrections=0;if(corrections++%60==0)log("CAMPAIGN HUB_FOLLOW corrections=%u drift=%.4g host=%.4g,%.4g,%.4g",corrections,drift,host[0],host[1],host[2]);}
            else if(std::isfinite(drift)&&drift>=20.0f)campaignReject("hub P1 positions diverged beyond follow range");}
        float mine[3]={};if(networkRole==1&&campaignHubPlayer2&&campaignHubP2Position(mine)){float joiner[3];memcpy(joiner,&peer.auxiliary[5],sizeof(joiner));float drift=std::hypot(joiner[0]-mine[0],joiner[1]-mine[1]);
            if(std::isfinite(drift)&&drift>0.15f&&drift<20.0f){DWORD actor=readWord(readWord(0x6a9900)+0x138);WORD heading=0;readMemory((void*)(readWord(actor+0x1d0)+12),&heading,2);
                ((void(__thiscall*)(void*,const float*,unsigned,bool))0x459fe4)((void*)actor,joiner,heading,true);static unsigned corrections2=0;if(corrections2++%60==0)log("HUBPARTNER P2_FOLLOW corrections=%u drift=%.4g",corrections2,drift);}}}
    // Mission list: each seat's list opens on its own last row (a host that played alone before a rejoin had row 2,
    // the joiner row 0: rejoin-383, mismatch-367). Wait instead of rejecting while the joiner steps its own cursor to
    // the host's row (a host with no row steps onto one first); only an agreed row can be accepted. Rows step with
    // actions 0/1 (up/down); if three steps change nothing, the left/right pair 2/3 is tried.
    unsigned missionNav=0;
    if(kind==4&&state.selection!=peer.selection){
        // Only the row may differ (its name, availability and area follow the row); anything else still rejects.
        auto masked=peer;masked.selection=state.selection;masked.identity=state.identity;masked.auxiliary[5]=state.auxiliary[5];masked.auxiliary[7]=state.auxiliary[7];
        if(coop::CampaignMenuAgreement(state,masked)!=coop::CampaignAgreement::invalid)agreement=coop::CampaignAgreement::waiting;
        static unsigned navPresses=0,navFrom=~0u;unsigned hostRow=networkRole==1?selection:peer.selection;
        if(calls%20==10){bool down=false,step=false;
            if(networkRole==1){if(selection>=4){step=true;down=true;}}
            else if(hostRow<4){step=true;down=selection>=4||selection<hostRow;}
            if(step){if(selection!=navFrom){navFrom=selection;navPresses=0;}bool sideways=(navPresses++/3)%2==1;
                missionNav=1u<<(sideways?(down?3:2):(down?1:0));
                static unsigned navLogs=0;if(navLogs++<60)log("CAMPAIGN MISSION_SYNC role=%u selection=%ld host_row=%ld nav=%08x",networkRole,(LONG)selection,(LONG)hostRow,missionNav);}}
    }
    // Main menu: its cursor follows the keyboard/pad directly, not the shared actions (run menu-nav-016: no applied
    // action moved it), so a host moving the cursor leaves the joiner's row behind. The joiner's row is set to the
    // host's each frame; nothing is accepted until both rows agree.
    if(kind==1&&state.selection!=peer.selection){
        auto masked=peer;masked.selection=state.selection;
        if(coop::CampaignMenuAgreement(state,masked)!=coop::CampaignAgreement::invalid)agreement=coop::CampaignAgreement::waiting;
        if(networkRole==2&&list&&peer.selection<4){nativeListSelect(list,peer.selection);static unsigned rowLogs=0;if(rowLogs++<40)log("CAMPAIGN MAIN_MENU_ROW joiner follows host row=%lu from=%ld",peer.selection,(LONG)selection);}
    }
    if(agreement==coop::CampaignAgreement::waiting){campaignNeutralMenu();
        if(kind==4&&missionNav){coop::Actions nav={};for(unsigned i=0;i<28;i++)if(missionNav&(1u<<i))nav[i]=1;applyNetworkActions(0,nav);applyNetworkActions(1,{});}
        // Feed the slot cursor every frame (released between pulses), as the local drive does: clearing the
        // consumers alone keeps the last pulse "held", so the next one is no new press (run together-103).
        if(kind==2){coop::Actions nav={};for(unsigned i=0;i<28;i++)if(slotNav&(1u<<i))nav[i]=1;applyNetworkActions(0,nav);applyNetworkActions(1,{});
            static unsigned slotLogs=0;if(slotLogs<120&&(slotNav||calls%60==0)){++slotLogs;log("CAMPAIGN SLOT_SYNC role=%u selection=%ld free_row=%u nav=%08x calls=%u own_ready=%u peer_ready=%u",networkRole,(LONG)selection,freeRow,slotNav,calls,state.flags?1u:0u,peer.flags?1u:0u);}}
        campaignMenuContext=scene;campaignMenuKind=kind;return true;}
    if(agreement!=coop::CampaignAgreement::ready){
        log("CAMPAIGN MISMATCH_DETAIL kind=%u sel=%lu/%lu flags=%lu/%lu aux=%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx peer=%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx id_same=%u",kind,state.selection,peer.selection,state.flags,peer.flags,
            state.auxiliary[0],state.auxiliary[1],state.auxiliary[2],state.auxiliary[3],state.auxiliary[4],state.auxiliary[5],state.auxiliary[6],state.auxiliary[7],
            peer.auxiliary[0],peer.auxiliary[1],peer.auxiliary[2],peer.auxiliary[3],peer.auxiliary[4],peer.auxiliary[5],peer.auxiliary[6],peer.auxiliary[7],state.identity==peer.identity?1u:0u);
        campaignReject("native menu row/availability/progress mismatch");}
    if(hostBits&(1u<<10)){
        if(kind==3&&state.selection!=campaignMissionGraphHash()&&state.selection!=campaignSidekickTerminal)campaignReject("hub Accept outside the mission and sidekick terminals");
        if(kind==7&&!campaignSidekickRowAvailable(selection))campaignReject("sidekick row is not unlocked");
        // Main menu: New Game only (saves are per PC). Save slot: any native slot, as long as both seats agree.
        if((kind==1&&selection!=0)||(kind==2&&selection>2)){log("CAMPAIGN REJECT_DETAIL kind=%u selection=%u calls=%u scene_type=%08lx list=%08lx",kind,selection,calls,readWord(scene),list);campaignReject("retail campaign requires New Game and a native save slot");}
        if(kind==4&&(selection>=4||!state.auxiliary[5]))campaignReject("native mission row is not available");
        if(kind==5&&!coop::CampaignSaveAckReady(campaignSaveAckFacts(scene,selection)))campaignReject("native save acknowledgment changed before confirmation");
        if(kind==6&&!coop::CampaignAutosaveNoticeReady(campaignAutosaveNoticeFacts(scene,selection)))campaignReject("native autosave notice changed before confirmation");
    }
    if(hostBits&((1u<<10)|(1u<<11))){
        // Hub positions are follow data, not part of the confirmed choice.
        if(kind==3)for(unsigned i=4;i<8;i++)state.auxiliary[i]=0;
        state.auxiliary[0]=hostBits;state.flags=coop::phaseReady|coop::phaseConfirmed;peer=campaignExchange(state);if(campaignStopping())return false;
        if(!coop::CampaignPhaseEqual(state,peer))campaignReject("host menu choice was not mutually confirmed");
    }
    if(kind==3){if(networkRole!=1){campaignHubMovement=campaignWalkBits(hostBits);campaignHubActions=peer.auxiliary[4]&(campaignHubActionMask|(1u<<10));}++campaignHubPairedCalls;
        if(campaignHubPlayer2&&networkRole==1){campaignHubMovement2=campaignWalkBits(peer.auxiliary[4]&0xf0000u);campaignHubActions2=peer.auxiliary[4]&(campaignHubActionMask|(1u<<10));}}
    coop::Actions shared={},second={};for(unsigned i=0;i<28;i++)if(hostBits&(1u<<i))shared[i]=1;
    if(kind==3&&campaignHubPlayer2)for(unsigned i=16;i<20;i++)if(campaignHubMovement2&(1u<<i))second[i]=1;
    applyNetworkActions(0,shared);applyNetworkActions(1,second);campaignMenuContext=scene;campaignMenuKind=kind;return true;
}
static bool campaignTogetherPending(){return campaignMode&&!campaignTogetherDecided&&((networkRole==1&&campaignTogetherOffered&&campaignJoinPending)||(networkRole==2&&campaignAutoTogether));}
// Receive-only wait for a phase this game already sent (the host's offer), with the partner notice.
static bool campaignWaitReceive(unsigned sequence,coop::PhaseState& peer){
    if(campaignStopping())return false;
    ULONGLONG started=GetTickCount64(),reported=started;struct HideNotice{~HideNotice(){campaignWaitNotice(false);}}hide;
    for(;;){int got=networkWire->TryReceivePhase(sequence,peer);if(got>0)return true;if(got<0)return false;
        if(GetTickCount64()-started>=1500)campaignWaitNotice(true);
        MSG message;while(PeekMessageW(&message,0,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        ULONGLONG now=GetTickCount64();if(now-reported>=10000){reported=now;log("CAMPAIGN WAITING_FOR_PARTNER main menu choice sequence=%u seconds=%llu",sequence,(now-started)/1000);}
        Sleep(5);}
}
// Host picked New Game (row 0) or Continue (row 1) with an offer out: wait for the partner's main menu, then tell it.
static void campaignTogetherDecide(DWORD row){
    coop::PhaseState peer={};
    if(!campaignWaitReceive(campaignOfferSequence,peer)){if(campaignStopping())return;if(campaignPartnerLost(networkWire->PeerStopped()?"partner closed the game":"connection lost in the main menu"))return;if(networkWire->PeerStopped()){campaignCloseFromPeer("phase",campaignOfferSequence);return;}campaignReject("partner did not answer the start-together offer");}
    campaignPhaseSequence=campaignOfferSequence+1;
    auto state=campaignJoinState(0x30210);state.selection=row;auto answer=campaignExchange(state);
    if(!networkReady||networkComplete)return;if(answer.id!=0x30211)campaignReject("partner main menu answer");
    campaignTogetherDecided=true;
    if(row==0){campaignJoinPending=false;campaignAutoTogether=true;log("CAMPAIGN AUTO_TOGETHER host chose New Game; starting together");}
    else log("CAMPAIGN AUTO_TOGETHER host chose Continue; partner joins at the hub");
}
static DWORD campaignMainRow(){DWORD main=sceneObject(2),list=main?readWord(main+0x84):0;return list?readWord(list+0x8d08):~0u;}
static bool __cdecl campaignMainAccept(DWORD* winner){
    if(campaignStopping())return false;
    bool accepted=((bool(__cdecl*)(DWORD*))0x497e78)(winner);
    if(accepted&&campaignTogetherPending()){DWORD row=campaignMainRow();
        if(row>=2)return true;           // Options / Quit: this player's own
        if(networkRole==2)return false;  // the friend waits for the host's choice
        campaignTogetherDecide(row);if(campaignStopping())return false;if(row==0&&campaignAutoTogether)campaignMainAcceptObserved=true;return true;}
    if(accepted&&campaignMode&&campaignJoinPending&&networkRole==2&&!campaignJoinReceived&&campaignMainRow()<2)return false;
    if(accepted&&campaignTogetherJoinerStart){campaignTogetherJoinerStart=false;campaignMainAcceptObserved=true;return true;}
    // A host still playing alone before the join (bot local drive) may Continue.
    if(campaignLive()&&accepted){bool newGame=campaignMenuKind==1&&campaignMenuContext&&readWord(readWord(campaignMenuContext+0x84)+0x8d08)==0;
        if(newGame)campaignMainAcceptObserved=true;else if(!(campaignJoinPending&&networkRole==1&&!campaignJoinSent))campaignReject("first retail slice supports native New Game only");}
    return accepted;
}
static void campaignAssertNewGame(){
    if(campaignStopping())return;
    if(campaignMainAcceptObserved&&!campaignNewGame){if(readWord(0x69eb0c)!=0||readWord(0x58731c)!=0)campaignReject("native New Game result mismatch");campaignNewGame=true;if(networkWire&&networkReady)networkWire->SetTimeout(30000);log("CAMPAIGN ASSERT NEW_GAME operation=0 selection=0");}
}
static void __fastcall campaignMainUpdate(void* scene,void*,float dt){
    if(campaignStopping())return;
    BYTE active=0;readMemory((BYTE*)scene+0x7a,&active,1);
    // Host linked while still in the main menu (nothing loaded alone yet): offer the joiner to start together,
    // without waiting; both keep their own main menu until the host picks New Game or Continue.
    if(campaignMode&&campaignJoinPending&&networkRole==1&&!campaignJoinSent&&!campaignTogetherOffered&&!campaignSoloLoaded&&campaignAutoTogetherEnabled&&active&&networkReady){
        campaignOfferSequence=campaignPhaseSequence;
        if(!networkWire->SendPhase(campaignOfferSequence,campaignJoinState(0x30200))){
            if(networkWire->PeerStopped()){if(!campaignPartnerLost("partner closed the game"))campaignCloseFromPeer("phase",campaignOfferSequence);return;}
            campaignReject("start-together offer send");}
        campaignTogetherOffered=true;campaignJoined=true;log("CAMPAIGN AUTO_TOGETHER host offer from the main menu sequence=%u",campaignOfferSequence);
    }
    if(campaignHostWaitLink&&campaignFixture&&networkDeferred){campaignNeutralMenu();if(!campaignStopping())originalCampaignMain(scene,dt);return;}
    if(campaignTogetherPending()){
        if(networkRole==2&&active&&networkWire->PhaseAvailable(campaignPhaseSequence)){
            auto peer=campaignExchange(campaignJoinState(0x30211));if(campaignStopping())return;if(peer.id!=0x30210)campaignReject("host main menu choice");campaignTogetherDecided=true;
            if(peer.selection==0){DWORD main=sceneObject(2),list=main?readWord(main+0x84):0;if(list)nativeListSelect(list,0);campaignTogetherJoinerStart=true;
                coop::Actions accept={};accept[10]=1;applyNetworkActions(0,accept);log("CAMPAIGN AUTO_TOGETHER joiner: host chose New Game; starting together");}
            else{campaignAutoTogether=false;campaignJoinPending=true;networkWire->SetTimeout(120000);log("CAMPAIGN AUTO_TOGETHER joiner: host chose Continue; joining at the hub");}
        }
        // Test bot host (fixture): picks New Game, or Continue with COOP_PROBE_HOST_CONTINUE, a few seconds in.
        if(networkRole==1&&campaignFixture&&active){static unsigned calls=0;if(++calls%120==60){DWORD main=sceneObject(2),list=main?readWord(main+0x84):0;
            if(campaignHostContinue&&list&&readWord(list+0x8d08)!=1)nativeListSelect(list,1);else{coop::Actions accept={};accept[10]=1;applyNetworkActions(0,accept);}}}
        if(!campaignStopping())originalCampaignMain(scene,dt);campaignAssertNewGame();return;
    }
    if(!campaignLive()){if(!campaignStopping())originalCampaignMain(scene,dt);return;}bool begun=campaignMenuBegin(scene,1,0x84);if(!campaignStopping())originalCampaignMain(scene,dt);
    if(begun)campaignAssertNewGame();campaignNeutralMenu();campaignMenuContext=0;
}
// A game played alone (opened, not yet linked) keeps the player's own menu input.
static void __fastcall campaignProfileUpdate(void* scene,void*,float dt){
    if(campaignStopping())return;
    bool run=true;
    if(campaignLive()){
        if(campaignOverwritePending&&networkRole==2&&!campaignLocalDrive())run=campaignOverwriteMenu(0);
        else if(campaignOverwritePending&&networkRole==1){campaignNeutralMenu();campaignMenuContext=0;}
        else run=campaignMenuBegin(scene,2,0x88);
    }
    if(run&&!campaignStopping())originalCampaignProfile(scene,dt);
    if(campaignLive()){campaignNeutralMenu();campaignMenuContext=0;}
}
// Research (hub second character): script native 34 (dispatcher 4D9065, call 4D92A4) spawns playable character
// <index> from descriptor table 6A8858+index*0x58 at the level marker start_SpiderMan (index 0/6) or start_SideKick
// (StartPosition fallback) and registers it with the character manager 6A9900 (first -> primary +0x128, next ->
// sidekick +0x134). Logged here; COOP_PROBE_HUB_SPAWN=<index> calls it once more after the hub is reached.
static int campaignHubSpawnIndex=-1;static bool campaignHubSpawned=false;
// Hub partner (player 2 in the hub) is on in campaign co-op unless COOP_PROBE_HUB_PARTNER=0.
static bool campaignHubPartner=true,campaignHubPartnerSound=true;
static void campaignLogParty(const char* when){
    DWORD party=readWord(0x6a9900);
    for(unsigned player=0;player<2;player++){DWORD actor=party?readWord(party+0x12c+player*12):0,transform=0;float xyz[3]={};
        if(actor&&readWord(readWord(actor)+0x2f4)==0x45a6c9){transform=readWord(actor+0x1d0);if(transform)readMemory((void*)transform,xyz,12);}
        log("HUBSPAWN %s party_slot=%u actor=%08lx vtable=%08lx xyz=%.4g,%.4g,%.4g",when,player,actor,actor?readWord(actor):0,xyz[0],xyz[1],xyz[2]);}
}
// Manager free list +0x3C/+0x40 and active list +0x34/+0x38: node {next, character object}; object+4 = its fixed slot number.
static void campaignLogSlots(const char* when){
    DWORD manager=readWord(0x6a9900);if(!manager)return;
    for(unsigned list=0;list<2;list++){DWORD node=readWord(manager+(list?0x34:0x3c));char text[512]={};int used=0;
        for(unsigned i=0;node&&i<16&&used<480;i++){DWORD object=readWord(node+4);used+=_snprintf(text+used,512-used," %lu@%08lx",object?readWord(object+4):99,object);node=readWord(node);}
        log("HUBSPAWN %s %s_list=%s",when,list?"active":"free",text[0]?text:" (empty)");}
}
static bool campaignFreeSlot(unsigned slot){DWORD node=readWord(readWord(0x6a9900)+0x3c);for(unsigned i=0;node&&i<16;i++){DWORD object=readWord(node+4);if(object&&readWord(object+4)==slot)return true;node=readWord(node);}return false;}
// Character table 6A8808 + i*0x58: +0x20 per-entry memory owner (load needs it), +0x50 descriptor once loaded.
static void campaignLogCharacterTable(const char* when){
    for(unsigned i=0;i<16;i++){DWORD e=0x6a8808+i*0x58;if(readWord(e+0x20)||readWord(e+0x50))log("HUBSPAWN %s entry=%u owner=%08lx owner_vtable=%08lx size24=%08lx descriptor=%08lx",when,i,readWord(e+0x20),readWord(e+0x20)?readWord(readWord(e+0x20)):0,readWord(e+0x24),readWord(e+0x50));}
}
static void __cdecl campaignLogLoadCharacter(DWORD index,const char* text){
    campaignLogCharacterTable("before_load");
    char copy[64]={};if(text)readMemory((void*)text,copy,sizeof(copy)-1);
    log("HUBSPAWN load_character index=%lu text=%s chosen_sidekick=%lu current=%lu",index,copy,readWord(0x69c624),readWord(0x69c8bc));
}
static DWORD campaignLoadCharacterAddress=0x4e5b00;
static void __declspec(naked) campaignLoadCharacterBridge(){
    __asm{
        pushad
        push dword ptr [esp+36]
        push eax
        call campaignLogLoadCharacter
        add esp,8
        popad
        jmp dword ptr [campaignLoadCharacterAddress]
    }
}
// Load character <index> from art/characters/<name> the way the level scripts do (async; its descriptor
// appears in 6A8858+index*0x58). The entry needs a memory owner at +0x20 (4E5B65 crashed without one).
static void campaignLoadCharacter(DWORD index,const char* name){
    static char text[0x80];memset(text,0,sizeof(text));strncpy(text,name,sizeof(text)-1);
    __asm{
        push offset text
        mov eax,index
        call dword ptr [campaignLoadCharacterAddress]
        add esp,4
    }
}
// The load callback 4E5C86 builds the descriptor (4E4FEB) and then assigns the character's sound banks to fixed
// sound slots by character entry (4E5CE1/4E5DA1: desc+F4/+FC bank files, +F8/+100 slots) and loads them. In the hub
// those slots hold the hub's own sounds (hubspawn-174 crashed in a hub sound lookup); a missing file crashes the
// loader (187). The game treats bank file -1 as "no bank" (4E5D29, 4E5E1B). The 174/263 crashes came from +0x100
// read before it is set (below); with that fixed the sidekick keeps its sounds (slots 5/11 are free in the hub, 256).
// COOP_PROBE_HUB_PARTNER_SOUND=0 loads it silent instead.
using BuildDescriptor=DWORD(__cdecl*)(DWORD,DWORD,DWORD,DWORD);
static DWORD __cdecl campaignBuildDescriptor(DWORD a,DWORD b,DWORD c,DWORD d){
    DWORD descriptor=((BuildDescriptor)0x4e4feb)(a,b,c,d);
    // The second sound slot (+0x100) is only written by the bank-load callback (4E5E1E) but read before that; in the
    // hub's private heap it held leftover bytes (crash 491E20, partner-sound-263). Start it as "no bank" (-1).
    if(descriptor&&campaignHubLoadName[0])*(DWORD*)(descriptor+0x100)=0xffffffffu;
    if(descriptor&&campaignHubLoadName[0]&&!campaignHubPartnerSound){log("HUBSPAWN descriptor %08lx built; sound banks f4=%ld fc=%ld set to none in the hub",descriptor,(LONG)readWord(descriptor+0xf4),(LONG)readWord(descriptor+0xfc));
        *(DWORD*)(descriptor+0xf4)=0xffffffffu;*(DWORD*)(descriptor+0xfc)=0xffffffffu;}
    return descriptor;
}
static void __stdcall campaignSpawnCharacter(DWORD index){
    campaignLogSlots("before_native34");
    char path[33]={};DWORD id=0;bool known=campaignArea(readWord(readWord(0x6a9974)+0x18),path,id);
    DWORD descriptor=index<32?readWord(0x6a8858+index*0x58):0;
    log("HUBSPAWN native34 index=%lu descriptor=%08lx type8c=%lu class90=%lu coop3c=%08lx area=%s flow=%lu",index,descriptor,descriptor?readWord(descriptor+0x8c):0,descriptor?readWord(descriptor+0x90):0,readWord(0x69ee3c),known?path:"-",readWord(readWord(0x69e72c)+8));
    ((void(__stdcall*)(DWORD))0x4d9f45)(index);campaignLogParty("after_native34");
}
static bool campaignOwnEntryLive=false,campaignOwnBlockDirty=false;
// Level memory switch (4E629E via 4E6261): it releases the old level's entries (4E6489) and prepares the new level's.
// Entry 1 prepared by the hub partner is in no level list, so release it here with the game's own release, after the
// hub's characters are gone and before the mission prepares entry 1 again (otherwise its objects stay registered).
static void __cdecl campaignLevelMemory(DWORD type){
    if(campaignOwnEntryLive){DWORD e1=0x6a8808+0x58;__asm{push esi
            mov esi,e1
            mov eax,0x4e6489
            call eax
            pop esi}
        campaignOwnEntryLive=false;log("HUBPARTNER released entry 1 at the level memory switch type=%lu",type);}
    campaignOwnBlockDirty=false;
    ((void(__cdecl*)(DWORD))0x4e629e)(type);
}
// 4E61AB(index) equivalent over a private VirtualAlloc block: heap +4 initialized with 431F16 over the block, the
// memory owner (+0x20/+0x34, 439CF6) allocated from that heap, sizes from table 582BD8 via 4E6045. +0x54 stays 0 so
// the game's release (4E6489) never hands this block to its arena.
static bool campaignPrepareOwnEntry(DWORD index){
    DWORD e=0x6a8808+index*0x58,flag=readWord(0x582be0+index*12),sizeBlock=0,sizeOwner=0,owner=0;float fa=0,fb=0;
    readMemory((void*)(0x582bd8+index*12),&fa,4);readMemory((void*)(0x582bdc+index*12),&fb,4);
    __asm{push ecx
        fld fb
        fstp dword ptr [esp]
        mov eax,0x4e6045
        call eax
        mov sizeOwner,eax
        fld fa
        fstp dword ptr [esp]
        mov eax,0x4e6045
        call eax
        mov sizeBlock,eax
        pop ecx}
    // One private block, reused on later hub visits once the game has released entry 1 from it (campaignLevelMemory);
    // never reused while objects built in it may still be registered (run partner-cycles-234 crashed on that).
    DWORD total=sizeBlock+sizeOwner+0x40000;static BYTE* kept=0;static DWORD keptSize=0;
    // After a sidekick swap inside the hub, game-wide lists (e.g. 69F6E4) still hold nodes in the old block until the next
    // level switch (sidekick-336/345 crashed at 413424 on reuse): use a fresh block then and leave the old one untouched.
    if(!kept||keptSize<total||campaignOwnEntryLive||campaignOwnBlockDirty){kept=(BYTE*)VirtualAlloc(0,total,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);keptSize=kept?total:0;campaignOwnBlockDirty=false;}
    BYTE* memory=kept;if(memory)memset(memory,0,total);
    log("HUBSPAWN own entry %lu sizes block=%lu owner=%lu memory=%p",index,sizeBlock,sizeOwner,memory);if(!memory)return false;
    // Heap +4 keeps "initialized" at +0x10 (entry+0x14); the game's release clears it only when it freed an arena block
    // (4E64A6), which ours never is, so clear it here or 431F16 keeps the old free lists (crash 413492, sidekick-315).
    *(DWORD*)e=index;*(DWORD*)(e+8)=flag;*(DWORD*)(e+0x54)=0;*(BYTE*)(e+0x14)=0;DWORD heap=e+4,block=(DWORD)(ULONG_PTR)memory;
    __asm{push esi
        mov ecx,heap
        mov esi,block
        push total
        mov eax,0x431f16
        call eax
        pop esi}
    DWORD request[2]={0,sizeOwner};
    __asm{mov eax,heap
        mov ecx,0x40dbdd
        call ecx
        push flag
        lea eax,request
        mov ecx,1
        mov edx,0x439cf6
        call edx
        add esp,4
        mov owner,eax
        mov ecx,0x40dbf6
        call ecx}
    *(DWORD*)(e+0x20)=owner;*(BYTE*)(e+0x4b)=0;*(DWORD*)(e+0x34)=owner;campaignOwnEntryLive=owner!=0;
    log("HUBSPAWN own entry %lu owner=%08lx",index,owner);return owner!=0;
}
// Hub partner: in co-op, the campaign's chosen sidekick (id [69C624] -> name at 583520+id*16; 1 = prowler) appears in
// the hub as player 2, driven by the joiner. Missions load the sidekick into character entry 1 with 4E61AB(1); the hub
// prepares only entries 0,8,9,10,12,13 and has no memory to spare (arena int3 431D82, hubspawn-153; entry 13's heap is
// its working heap, 158-170). So entry 1 is prepared over private memory (campaignPrepareOwnEntry), the sidekick loads
// there without sound banks (campaignBuildDescriptor; its sound slots belong to hub sounds, 174/187), gets the free
// hero-size object (4 built at startup, numbers 1<->3 swapped with a busy hub character), spawns with native 34(1),
// stands beside player 1 and takes controller slot 1 (hubspawn-214/220/226 PASS). Both seats start at the same paired
// hub exchange (campaignHubPairedCalls), so they load and spawn in step; the joiner's P2 position is the reference.
static unsigned campaignHubPartnerState=0,campaignHubPartnerFrames=0;
static bool campaignHubPartnerEnabled(){return campaignHubSpawnIndex==1||(campaignHubPartner&&campaignMode&&networkRole);}
static void campaignHubPartnerReset(){campaignHubPartnerState=0;campaignHubPartnerFrames=0;campaignHubPairedCalls=0;campaignHubSpawned=false;campaignHubPlayer2=false;campaignHubMovement2=0;campaignHubLoadName[0]=0;}
static DWORD campaignHubPartnerChosen=0;
// A different sidekick was chosen at the sidekick computer: remove player 2 with the game's own character release
// (45CF83: EAX=handle, EDI=manager; back to the free list), clear the sidekick handle and HUD pointer, release entry 1
// (4E6489), then the normal path loads and spawns the new sidekick.
static void campaignHubPartnerRemove(){
    DWORD party=readWord(0x6a9900),e1=0x6a8808+0x58;
    if(party&&readWord(party+0x138)){DWORD handle=party+0x134;__asm{push esi
            push edi
            push ebx
            mov eax,handle
            mov edi,party
            mov ecx,0x45cf83
            call ecx
            pop ebx
            pop edi
            pop esi}
        memcpy((void*)(party+0x134),(void*)0x58eea4,12);}
    DWORD game=readWord(0x69e6e8);if(game)*(DWORD*)(game+0x7a4)=0;
    // Tear the old descriptor down the way level unload does (4E540B, thiscall, vtable 5788A0 slot): it unregisters its
    // animation data from game-wide lists and unloads its sound banks. Without it the reused block left stale list
    // nodes (crash 413424, sidekick-336).
    DWORD descriptor=readWord(e1+0x50);
    if(descriptor){__asm{mov ecx,descriptor
            mov eax,0x4e540b
            call eax}}
    if(campaignOwnEntryLive){__asm{push esi
            mov esi,e1
            mov eax,0x4e6489
            call eax
            pop esi}
        campaignOwnEntryLive=false;}
    campaignOwnBlockDirty=true;
    campaignHubPartnerState=0;campaignHubPartnerFrames=0;campaignHubPairedCalls=0;campaignHubSpawned=false;campaignHubPlayer2=false;campaignHubMovement2=0;
    log("HUBPARTNER player 2 removed for a new sidekick choice id=%lu",readWord(0x69c624));
}
static void campaignHubPartnerUpdate(){
    if(!campaignHubPartnerEnabled()||!campaignHub||campaignMission||campaignJoinPending)return;
    DWORD e1=0x6a8808+0x58,party=readWord(0x6a9900);
    if(campaignHubPartnerState==2&&readWord(0x69c624)!=campaignHubPartnerChosen&&readWord(readWord(0x6a9940)+0x4e4)!=42)campaignHubPartnerRemove();
    if(campaignHubPartnerState==0&&campaignHubPairedCalls>=60){
        campaignHubPartnerState=3;
        DWORD chosen=readWord(0x69c624);char name[32]={};if(chosen<15)readMemory((void*)readWord(0x583520+chosen*16),name,sizeof(name)-1);
        campaignHubPartnerChosen=chosen;
        if(party&&readWord(party+0x138)){campaignHubPlayer2=true;campaignHubPartnerState=2;log("HUBPARTNER player 2 already present");return;}
        log("HUBPARTNER sidekick id=%lu name=%s entry1_owner=%08lx",chosen,name[0]?name:"-",readWord(e1+0x20));
        {DWORD table=readWord(readWord(readWord(readWord(0x69e6e8)+0x16c)+0x18)+0xc);char text[400]={};int used=0;
            // Bank object (record +0x10): byte +8 = loaded (408BEC returns "not found" when it is 0).
            for(unsigned i=0;i<16&&used<380;i++){DWORD bank=table?readWord(table+i*0x18+0x10):0;BYTE loaded=0;if(bank)readMemory((void*)(bank+8),&loaded,1);used+=_snprintf(text+used,400-used," %u:%u",i,loaded);}
            log("HUBPARTNER sound slots table=%08lx%s",table,text);}
        if(!name[0]||readWord(e1+0x20)||!campaignPrepareOwnEntry(1))return;
        DWORD heap=e1+4;__asm{mov eax,heap
            mov ecx,0x40dbdd
            call ecx}
        strncpy(campaignHubLoadName,name,sizeof(campaignHubLoadName)-1);campaignLoadCharacter(1,name);
        __asm{mov ecx,0x40dbf6
            call ecx}
        campaignHubPartnerState=1;campaignHubPartnerFrames=0;
    }
    if(campaignHubPartnerState!=1)return;
    // The descriptor appears before its linked animations/sound banks finish; 69ED88 is 1 while that load runs and 2
    // when done (4E5B41, 4E5E46). Prowler happened to be complete; Silver Sable was not (null desc+0xEC, 4B28C2).
    DWORD loaded=readWord(0x6a8858+0x58);if(loaded&&readWord(0x69ed88)==1)loaded=0;
    if(!loaded){if(++campaignHubPartnerFrames>=1200){campaignHubPartnerState=3;campaignHubLoadName[0]=0;log("HUBPARTNER sidekick load did not finish");}return;}
    campaignHubPartnerState=2;campaignHubSpawned=true;campaignHubLoadName[0]=0;
    DWORD a1=0,a3=0,node=readWord(party+0x34);for(unsigned i=0;node&&i<16;i++){DWORD o=readWord(node+4);if(o&&readWord(o+4)==1)a1=o;node=readWord(node);}
    node=readWord(party+0x3c);for(unsigned i=0;node&&i<16;i++){DWORD o=readWord(node+4);if(o&&readWord(o+4)==3)a3=o;node=readWord(node);}
    if(!campaignFreeSlot(1)&&a1&&a3){*(DWORD*)(a1+4)=3;*(DWORD*)(a3+4)=1;}
    if(!campaignFreeSlot(1)){log("HUBPARTNER no free sidekick object");return;}
    // 45CE3A takes character object 0 (player 1's) for descriptor+0x90 of 0/1 and object 1 otherwise; Prowler is 9 but
    // Silver Sable is 0 (sidekick-322: no object, crash 4DA04E). Ask for object 1 for this spawn only.
    DWORD sizeClass=readWord(loaded+0x90);if(sizeClass<2)*(DWORD*)(loaded+0x90)=2;
    campaignSpawnCharacter(1);
    *(DWORD*)(loaded+0x90)=sizeClass;
    DWORD p1=readWord(party+0x12c),p2=readWord(party+0x138);
    if(p1&&p2&&readWord(readWord(p2)+0x2f4)==0x45a6c9&&readWord(p1+0x1d0)){
        float at[3]={};readMemory((void*)readWord(p1+0x1d0),at,12);at[0]+=1.5f;WORD heading=0;readMemory((void*)(readWord(p1+0x1d0)+12),&heading,2);
        ((void(__thiscall*)(void*,const float*,unsigned,bool))0x459fe4)((void*)p2,at,heading,true);
        ((void(__thiscall*)(void*,int))0x45666d)((void*)p2,1);campaignHubPlayer2=true;campaignCaptureAt=(LONG)presentCalls;
        log("HUBPARTNER player 2 ready beside player 1 at %.4g,%.4g,%.4g",at[0],at[1],at[2]);}
}
static bool campaignHubP2Position(float* xyz){DWORD party=readWord(0x6a9900),actor=party?readWord(party+0x138):0,transform=actor?readWord(actor+0x1d0):0;return transform&&readWord(readWord(actor)+0x2f4)==0x45a6c9&&readMemory((void*)transform,xyz,12);}
static void __fastcall campaignHubUpdate(void* scene,void*,float dt){
    if(campaignStopping())return;
    campaignHubPartnerUpdate();
    BYTE active=0;readMemory((BYTE*)scene+0x7a,&active,1);
    if(campaignMode&&campaignJoinPending&&networkRole==1&&!campaignJoinSent&&active&&readWord(readWord(0x69e72c)+8)==16){char path[33]={};DWORD id=0;if(campaignArea(readWord(readWord(0x6a9974)+0x18),path,id)&&id==97)campaignHostSendJoin();}if(campaignStopping())return;
    if(campaignLive()&&active&&campaignGameplay){char path[33]={};DWORD id=0;if(campaignArea(readWord(readWord(0x6a9974)+0x18),path,id)&&id==97&&readWord(readWord(0x69e72c)+8)==16){
        log("CAMPAIGN ASSERT RETURN_HUB path=%s area=97 flow=16 tick=%u mission_end=%u",path,networkTick,networkMissionEndInvoked?1u:0u);
        if(networkMissionEndFixture&&!networkMissionEndInvoked)campaignReject("hub returned without the mission-end fixture");
        // Both seats must agree on the post-mission progress bytes before continuing.
        auto state=campaignState(coop::phaseLoad,0x20000+campaignCycle,id);state.identity={};memcpy(state.identity.data(),path,strlen(path));campaignExactBarrier(state);if(campaignStopping())return;
        log("CAMPAIGN ASSERT RETURN_HUB_SYNC progress=%08x tick=%u",state.auxiliary[1],networkTick);
        if(networkMissionEndFixture&&campaignCycle+1>=campaignCycleTarget){networkFinishScript();}
        else{campaignGameplay=false;campaignMission=false;networkMissionEndInvoked=false;campaignHubWalkReset=true;++campaignCycle;log("CAMPAIGN CYCLE_READY cycle=%u",campaignCycle);}}}
    if(campaignLive()&&active&&campaignProfile&&!campaignHub){char path[33]={};DWORD id=0;if(campaignArea(readWord(readWord(0x6a9974)+0x18),path,id)&&id==97&&!strcmp(path,"FrontEndHub/FrontEnd/FrontEndHub")&&readWord(readWord(0x69e72c)+8)==16){campaignHub=true;log("CAMPAIGN ASSERT HUB path=FrontEndHub/FrontEnd/FrontEndHub area=97 flow=16");}}
    if(campaignLive()&&campaignHub){static unsigned hubCalls=0;if(hubCalls++%120==0){DWORD hub=(DWORD)(ULONG_PTR)scene;BYTE hubActive=0;readMemory((void*)(hub+0x7a),&hubActive,1);log("CAMPAIGN HUB_OBSERVE calls=%u active=%u focus=%08lx mission_hash=%08lx menu_calls=%u",hubCalls,hubActive,readWord(hub+0x84),campaignMissionGraphHash(),campaignMenuCalls[3]);
        DWORD party=readWord(0x6a9900);for(unsigned player=0;player<2;player++){DWORD actor=readWord(party+0x12c+player*12),transform=actor?readWord(actor+0x1d0):0;float xyz[3]={};if(transform&&readWord(readWord(actor)+0x2f4)==0x45a6c9&&readMemory((void*)transform,xyz,12))log("CAMPAIGN HUB_PLAYER index=%u xyz=%.6g,%.6g,%.6g",player,xyz[0],xyz[1],xyz[2]);}
        static bool triggersLogged=false;if(!triggersLogged){triggersLogged=true;DWORD pool=readWord(0x6a9970),count=readWord(pool+8),items=readWord(pool+4);log("CAMPAIGN HUB_TRIGGERS count=%lu",count);
            for(unsigned i=0;i<count&&i<256;i++){DWORD trigger=readWord(items+i*4);if(!trigger)continue;char triggerName[33];if(!campaignTriggerName(trigger,triggerName))strcpy(triggerName,"-");log("CAMPAIGN HUB_TRIGGER index=%u name=%s table=%08lx kind=%lu flags=%08lx enabled=%lu enter=%08lx center=%.6g,%.6g,%.6g extents=%.6g,%.6g,%.6g",i,triggerName,readWord(trigger),readWord(trigger+0x80),readWord(trigger+0x74),readWord(trigger+0x78),readWord(readWord(trigger)+0x24),readFloat(trigger+0x50),readFloat(trigger+0x54),readFloat(trigger+0x58),readFloat(trigger+0x60),readFloat(trigger+0x64),readFloat(trigger+0x68));}}}}
    if(campaignLive())campaignMenuBegin(scene,3,0);if(!campaignStopping())originalCampaignHub(scene,dt);if(campaignLive()&&!campaignStopping()){campaignNeutralMenu();campaignMenuContext=0;if(campaignHub&&!campaignMission)campaignApplyHubMovement();}
}
static CampaignUpdate originalCampaignSidekick=0;
// Popup box (scene 14, popupboxsml.fen, update 49F80E; closes on Accept): shown in the hub after a level end (e.g. a new
// sidekick). In co-op the hub clears everyone's keys, so nobody could close it (level-end-445): the host's Accept is
// shared like any campaign menu choice.
static CampaignUpdate originalCampaignPopup=0;
static void __fastcall campaignPopupUpdate(void* scene,void*,float dt){
    if(campaignStopping())return;
    bool shared=campaignLive()&&readWord(readWord(0x69e72c)+8)!=128;
    if(shared)campaignMenuBegin(scene,8,0);
    if(!campaignStopping())originalCampaignPopup(scene,dt);
    if(shared){campaignNeutralMenu();campaignMenuContext=0;}
}
static void __fastcall campaignSidekickUpdate(void* scene,void*,float dt){
    if(campaignStopping())return;if(campaignLive())campaignMenuBegin(scene,7,0x88);if(!campaignStopping())originalCampaignSidekick(scene,dt);if(campaignLive()){campaignNeutralMenu();campaignMenuContext=0;}}
static void __fastcall campaignMissionUpdate(void* scene,void*,float dt){
    if(campaignStopping())return;if(campaignLive())campaignMenuBegin(scene,4,0x84);if(!campaignStopping())originalCampaignMission(scene,dt);if(campaignLive()){campaignNeutralMenu();campaignMenuContext=0;}}
static void __fastcall campaignSaveAckUpdate(void* scene,void*,float dt){
    if(campaignStopping())return;
    // Other native window40 uses (including the earlier profile preparation)
    // retain their own update. Only the observed post-init task9 acknowledgement
    // can receive a shared Accept. Keep updates running while either is unready.
    if(campaignLive()&&campaignProfile)campaignMenuBegin(scene,5,0x8c);
    if(!campaignStopping())originalCampaignSaveAck(scene,dt);if(campaignLive()){campaignNeutralMenu();campaignMenuContext=0;}
}
static bool __cdecl campaignAutosaveAccept(DWORD* winner){
    if(campaignStopping())return false;
    bool accepted=((bool(__cdecl*)(DWORD*))0x497e78)(winner);
    if(campaignLive()&&accepted){
        if(campaignMenuKind==9&&campaignMenuContext&&networkRole==1){
            unsigned row=readWord(readWord(campaignMenuContext+0x8c)+0x8d08);
            if(!campaignOverwriteReady(campaignMenuContext,row)||campaignOverwriteDecision!=row+1)campaignReject("unattested native overwrite Accept");
        }else{
            if(campaignMenuKind!=6||!campaignMenuContext||!coop::CampaignAutosaveNoticeReady(campaignAutosaveNoticeFacts(campaignMenuContext,readWord(readWord(campaignMenuContext+0x8c)+0x8d08))))campaignReject("unattested native generic notice Accept");
            campaignAutosaveAccepted=true;
        }
    }
    return accepted;
}
static void __fastcall campaignAutosaveNoticeUpdate(void* scene,void*,float dt){
    if(campaignStopping())return;
    BYTE active=0;readMemory((BYTE*)scene+0x7a,&active,1);
    bool overwrite=campaignLive()&&campaignOverwritePending&&networkRole==1&&active;
    if(overwrite)campaignOverwriteMenu((DWORD)(ULONG_PTR)scene);
    else if(campaignLive()&&campaignAutosaveArmed)campaignMenuBegin(scene,6,0x8c);
    if(!campaignStopping())originalCampaignAutosaveNotice(scene,dt);
    if(campaignStopping())return;
    if(overwrite&&campaignOverwriteDecision==1)campaignOverwriteReset();
    if(campaignLive()){campaignNeutralMenu();campaignMenuContext=0;if(campaignAutosaveArmed&&campaignAutosaveAccepted){campaignAutosaveArmed=false;log("CAMPAIGN AUTOSAVE_NOTICE accepted=1 option=0");}}
}
static void __cdecl campaignProfileBefore(DWORD scene){
    if(campaignStopping())return;
    if(!campaignLive())return;if(!campaignNewGame||readWord(scene)!=0x56efe8||readWord(readWord(scene+0x88)+0x8d08)>2||readWord(0x69eb0c)!=0)campaignReject("native New Game slot initialization precondition");campaignSlot=readWord(readWord(scene+0x88)+0x8d08);
    if(campaignNativeSlotUsed(scene,campaignSlot)&&(!campaignOverwritePending||!campaignOverwriteArmed||!campaignOverwriteAuthorized||networkRole!=1))campaignReject("native used save slot lacks host overwrite confirmation");
    if(networkRole==2&&(campaignSlot!=campaignFirstFreeSlot()||campaignSaveSlotUsed(campaignSlot)))campaignReject("partner save slot is no longer empty");
    // A game driving its own menus (playing alone, or catching up to join) confirms nothing with its partner.
    if(!campaignLocalDrive())campaignExactBarrier(campaignState(coop::phaseMenu,10,0));
}
static void __cdecl campaignProfileAfter(){
    if(campaignStopping())return;
    if(!campaignLive())return;BYTE valid=0;if(campaignSlot<3)readMemory((void*)(0x6a78d0+campaignSlot),&valid,1);if(campaignProfile||readWord(0x6a78cc)!=campaignSlot||!valid||readWord(0x69eb0c)!=0){log("CAMPAIGN PROFILE_DETAIL already=%u slot=%lu expected=%lu valid=%u operation=%lu",campaignProfile?1u:0u,readWord(0x6a78cc),campaignSlot,valid,readWord(0x69eb0c));campaignReject("native slot initializer result");}
    campaignProfile=true;log("CAMPAIGN ASSERT PROFILE slot=%lu native_init=1 operation=0",campaignSlot);
    campaignOverwriteReset();
}
static DWORD campaignProfileNativeAddress=0x4a582a;
static void __declspec(naked) campaignProfileInitBridge(){
    __asm{
        push ebp
        mov ebp,esp
        pushad
        push edi
        call campaignProfileBefore
        add esp,4
        call campaignStopPending
        test al,al
        jnz profile_cancelled
        popad
        push dword ptr [ebp+8]
        call dword ptr [campaignProfileNativeAddress]
        pushfd
        pushad
        call campaignProfileAfter
        popad
        popfd
        mov esp,ebp
        pop ebp
        ret 4
    profile_cancelled:
        popad
        mov esp,ebp
        pop ebp
        ret 4
    }
}
using CampaignDeactivate=void(__thiscall*)(void*);static CampaignDeactivate originalCampaignDeactivate=0;
static void __fastcall campaignMissionDeactivate(void* scene,void*){
    if(campaignStopping())return;
    BYTE committing=1;readMemory((BYTE*)scene+0x88,&committing,1);originalCampaignDeactivate(scene);
    if(campaignLive()&&!campaignStopping()&&!committing){char path[33]={};DWORD id=0;if(campaignMission||!campaignHub||!campaignArea(readWord(readWord(0x6a9974)+0x1c),path,id)||id==97||readWord(readWord(0x69e72c)+0xc)!=32)campaignReject("native mission selector commit result");
        campaignMission=true;campaignHubPartnerReset();strcpy(campaignMissionPath,path);campaignMissionArea=id;log("CAMPAIGN ASSERT MISSION_COMMIT path=%s area=%lu pending=32",path,id);}
}
static void campaignBeforeLoad(){
    if(campaignStopping())return;
    char path[33]={};DWORD id=0;if(!campaignArea(readWord(readWord(0x6a9974)+0x1c),path,id))campaignReject("unknown requested retail descriptor");
    if(campaignJoinPending){
        if(networkRole==2&&campaignJoinReceived&&id==97){
            // Keep this PC's own settings (+1BC..+1C7) and save timestamp (+1E4..); everything else is the host's campaign.
            BYTE* live=(BYTE*)0x69c470;memcpy(live,campaignJoinProgress,0x1bc);memcpy(live+0x1c8,campaignJoinProgress+0x1c8,0x1e4-0x1c8);
            if(campaignProgress()!=campaignJoinSelected)campaignReject("joined progress differs after copy");
            log("CAMPAIGN JOIN_APPLIED progress=%08x before hub load",campaignJoinSelected);
        }
        return;
    }
    if(levelEndTest&&levelEndByStart(readWord(readWord(0x6a9974)+0x1c))){
        // Runner: a mission already finished in this run is replaced by the next unfinished one in table order (the
        // hub bot always walks to the same terminal), so one run plays every region; then the mission's run area.
        const LevelEndArea* level=levelEndByStart(readWord(readWord(0x6a9974)+0x1c));unsigned index=(unsigned)(level-levelEndAreas);
        if(levelEndDone&(1u<<index)){for(unsigned next=0;next<sizeof(levelEndAreas)/sizeof(levelEndAreas[0]);next++)if(!(levelEndDone&(1u<<next))&&levelEndAreas[next].start){level=&levelEndAreas[next];break;}}
        if(level->run!=readWord(readWord(0x6a9974)+0x1c)){DWORD run=level->run;*(DWORD*)(readWord(0x6a9974)+0x1c)=run;campaignArea(run,path,id);strcpy(campaignMissionPath,path);campaignMissionArea=id;log("LEVEL_END mission start moved to %s",path);}}
    // Before the first mission frame: both games share their measured connection and use the larger input delay, so a
    // slow connection does not stall every frame (fake-internet run net-slow-433: 240 ms round trip, 6-frame buffer,
    // ~48 fps). Two exchanges: values, then a confirmation, so no frame can arrive before both have set it.
    if(id!=97&&networkRole&&!networkAgreedDelay&&networkWire&&networkReady){
        // A few direct round trips (both games are waiting here), the first one skipped: it includes the partner's arrival.
        for(unsigned ping=0;ping<7;ping++){double began=milliseconds();campaignExchange(campaignJoinState(0x30302));if(campaignStopping())return;if(ping)hubRoundSamples[hubRoundCount++%256]=(float)(milliseconds()-began);}
        coop::PhaseState mine=campaignJoinState(0x30300);mine.selection=measuredDelayFrames();auto other=campaignExchange(mine);if(campaignStopping())return;
        if(other.id!=0x30300||other.selection<1||other.selection>16)campaignReject("input delay agreement");
        unsigned agreed=mine.selection>other.selection?mine.selection:other.selection;
        if(!networkWire->SetDelay(agreed))campaignReject("input delay could not be applied before the first frame");
        auto confirm=campaignJoinState(0x30301);confirm.selection=agreed;auto peerConfirm=campaignExchange(confirm);if(campaignStopping())return;
        if(peerConfirm.id!=0x30301||peerConfirm.selection!=agreed)campaignReject("input delay confirmation");
        networkAgreedDelay=agreed;log("NET INPUT_DELAY frames=%u (this PC measured %lu, partner %lu, hub samples %u)",agreed,mine.selection,other.selection,hubRoundCount);
    }
    auto state=campaignState(coop::phaseLoad,++campaignLoadSequence,id);state.identity={};memcpy(state.identity.data(),path,strlen(path));state.auxiliary[5]=id;state.auxiliary[7]=readWord(readWord(readWord(0x6a9974)+0x1c)+0x1c);
    if(!networkReady&&!openNetworkConnection())campaignReject("retail transport open before native load");
    if(campaignLoadSequence==1&&networkRole==2&&campaignDelayMs){log("CAMPAIGN PRELOAD_DELAY delay_ms=%u",campaignDelayMs);ULONGLONG begin=GetTickCount64();while(GetTickCount64()-begin<campaignDelayMs){if(networkWire->PeerStopped()){campaignCloseFromPeer("preload",campaignLoadSequence);return;}Sleep(5);}}
    log("CAMPAIGN PRELOAD_WAIT sequence=%u path=%s area=%lu original_calls=0",campaignLoadSequence,path,id);campaignExactBarrier(state);if(campaignStopping())return;
    if(!campaignPrepared){if(!prepareNetworkPlayers()||!prepareTaskTiming())campaignReject("native controller/task preparation");campaignPrepared=true;}
    if(!networkInputClock&&campaignMission&&id==campaignMissionArea&&!strcmp(path,campaignMissionPath)){
        if(!campaignMission||networkTick||readWord(readWord(0x69e72c)+8)!=64)campaignReject("first retail gameplay clock boundary");
        // Match the proven research ordering: new actors/NIS timers must be
        // constructed under the common clock, after retail frontend unload.
        // Hub/media stay on raw time. Later loads keep the advancing clock.
        networkInputClock=true;for(unsigned logical=0;logical<4;logical++)*(DWORD*)(0x691170+logical*0x11c+0x110)=10000;*(DWORD*)0x6a98dc=10000;
        log("CAMPAIGN CLOCK origin_ms=10000 before_native_load=1");
    }
    // Area loads inside a running mission keep the shared random state: both games already hold the same value, and
    // this barrier is passed between lockstep ticks, one tick apart on the two PCs (level-chain-453 diverged here).
    // In-mission = a later area of the committed mission (not the mission start itself, not the hub); the barrier's own
    // path decides, which both PCs share (the native flow flag flips between ticks: level-regions-460 diverged).
    if(networkTick&&campaignMission&&strcmp(path,campaignMissionPath)&&!strstr(path,"FrontEndHub"))log("CAMPAIGN PRELOAD in-mission area load keeps the shared random state tick=%u",networkTick);
    else{*(DWORD*)0x58cf24=8466;networkAmbientRng=0x84f19a21;}
    log("CAMPAIGN PRELOAD_FORWARD sequence=%u path=%s area=%lu peer_ready=1 original_calls=1",campaignLoadSequence,path,id);
}
static bool campaignStartGameplay(){
    if(campaignStopping())return false;
    if(campaignGameplay)return true;if(!campaignMission||!campaignPrepared)campaignReject("gameplay reached without native campaign mission commit");
    char path[33]={};DWORD id=0;if(!campaignArea(readWord(readWord(0x6a9974)+0x18),path,id)||id!=campaignMissionArea||strcmp(path,campaignMissionPath))campaignReject("committed mission gameplay identity");
    DWORD manager=readWord(0x6a9900);bool partyReady=manager!=0;DWORD actor[2]={},controller[2]={},slot[2]={~0u,~0u};
    for(unsigned p=0;p<2;p++){actor[p]=readWord(manager+0x12c+p*12);controller[p]=readWord(actor[p]+0x2dc);if(controller[p])slot[p]=readWord(controller[p]+0xc0);if(!actor[p]||!controller[p]||slot[p]!=p)partyReady=false;}
    if(!partyReady){static bool waitingLogged=false;if(!waitingLogged){waitingLogged=true;log("CAMPAIGN WAIT_PARTY manager=%08lx actors=%08lx,%08lx controllers=%08lx,%08lx slots=%lu,%lu",manager,actor[0],actor[1],controller[0],controller[1],slot[0],slot[1]);}return false;}
    auto state=campaignState(coop::phaseLoad,0x10000+campaignLoadSequence,id);state.identity={};memcpy(state.identity.data(),path,strlen(path));campaignExactBarrier(state);if(campaignStopping())return false;
    if(!networkInputClock)campaignReject("native gameplay was constructed without common clock");
    campaignGameplay=true;campaignCycleStartTick=networkTick;log("CAMPAIGN ASSERT GAMEPLAY path=%s area=%lu flow=128 sidekick=%lu prepared=1",path,id,readWord(0x69c624));return true;
}
static void observePlayers(){
    DWORD manager=readWord(0x6a9900);if(!manager)return;
    for(unsigned player=0;player<2;player++){
        DWORD handle[3]={0};if(!readMemory((void*)(manager+0x128+player*12),handle,sizeof(handle))||!handle[1]||handle[2]==~0u)continue;
        DWORD header=readWord(handle[1]+8);if(!header||readWord(header+8)!=handle[2])continue;
        DWORD actor=handle[1],table=readWord(actor),controller=readWord(actor+0x2dc);
        DWORD positionGetter=readWord(table+0x2f4);
        log("PLAYER observe index=%u actor=%08lx serial=%08lx vtable=%08lx position_getter=%08lx controller=%08lx logical_slot=%ld active_controller=%08lx",player,actor,handle[2],table,positionGetter,controller,(LONG)readWord(controller+0xc0),readWord(actor+0x2d8));
        float position[3]={0};DWORD transform=readWord(actor+0x1d0);if(positionGetter==0x45a6c9&&transform&&readMemory((void*)transform,position,sizeof(position)))log("PLAYER position index=%u xyz=%.9g,%.9g,%.9g party_count=%lu world_update_depth=%lu",player,position[0],position[1],position[2],readWord(manager+0x48),readWord(0x69e8ec));
    }
    log("INPUT observe association=%ld,%ld selected_device=%08lx,%08lx start_state=%lu,%lu a_state=%lu,%lu",(LONG)readWord(0x583514),(LONG)readWord(0x583518),readWord(0x691284),readWord(0x6913a0),readWord(0x691170+4+5*4),readWord(0x69128c+4+5*4),readWord(0x691170+4+10*4),readWord(0x69128c+4+10*4));
}
// Test bots only (COOP_PROBE_SKIP_NIS=1 with the campaign bot): skip cutscenes the game itself lets the player skip,
// exactly as Start does in NIS mode (4E25CA..4E25DF: allowed flag 69ED66, NIS player 6A9984, skip 4E134B).
static bool campaignSkipNis=false;
static bool campaignMovieSkipTest=false;
static void campaignBotSkipNis(){
    if(!campaignSkipNis||!campaignFixture||readWord(0x69ed24)!=5)return;
    BYTE allowed=0;readMemory((void*)0x69ed66,&allowed,1);DWORD player=readWord(0x6a9984);
    static unsigned skips=0,blocked=0;if(!allowed||!player){if(blocked++%600==0)log("BOT NIS_SKIP not allowed now allowed=%u player=%08lx",allowed,player);return;}
    *(BYTE*)0x69ed66=0;__asm{mov eax,player
        mov ecx,0x4e134b
        call ecx}
    log("BOT NIS_SKIP count=%u",++skips);
}
// Research menu walk (menuFixture): one forced menu action at a time through the game's own forced-action field.
// Logs every scene change; PASS needs Options = scene 11 with the cursor on Audio, Audio = scene 12, the Save Game
// entry ignored, and the return to the main menu (2). Then the game is closed normally.
static void keyboardBindingsLog(bool again=false){
    static bool logged=false;if(again)logged=false;DWORD keyboard=readWord(0x595570),wrapper=readWord(0x595390),count=readWord(0x5955cc),descriptors=readWord(0x5955c8);
    if(logged||!keyboard||!wrapper||readWord(wrapper)<28||!descriptors)return;logged=true;char line[512]="";
    for(unsigned action=0;action<28;action++){DWORD head=readWord(wrapper+0x10)+action*8,node=readWord(head+4);
        for(unsigned traversed=0;node&&node!=head&&traversed<64;traversed++,node=readWord(node+4))if(readWord(node+8)==keyboard){DWORD key=readWord(node+12);if(key<count&&key<256){char item[24];_snprintf_s(item,sizeof(item),_TRUNCATE," %u:%02lx",action,readWord(descriptors+key*0xa4));strcat_s(line,item);}}}
    log("INPUT KEYBOARD_BINDINGS action:scan%s",line);
}
static bool menuFixtureStep(DWORD flow,ULONGLONG elapsed,DWORD forcedAddress){
    static unsigned step=0;static ULONGLONG last=0;static DWORD previous=~0u;static unsigned presentsAtMenu=0;static ULONGLONG menuAt=0;
    DWORD manager=readWord(0x6a9940),scene=manager?readWord(manager+0x4e4):~0u;keyboardBindingsLog();
    if(scene!=previous){log("MENU_FIXTURE scene=%lu step=%u flow=%lu",scene,step,flow?readWord(flow+8):0);previous=scene;last=elapsed;campaignCaptureTag=(LONG)scene;campaignCaptureAt=(LONG)presentCalls;}
    if(!flow||elapsed<4000||elapsed-last<2500||step>=(menuContinueFixture?10u:6u))return false;
    DWORD object=sceneObject(scene),list=object?readWord(object+0x84):0,selection=list?readWord(list+0x8d08):~0u;
    auto press=[&](DWORD raw,const char* what){*(DWORD*)forcedAddress=raw;last=elapsed;log("MENU_FIXTURE press=%s step=%u scene=%lu selection=%ld",what,step,scene,(LONG)selection);return true;};
    // Logos, legal and movies before the main menu: Start, as a player skipping them.
    if(step==0&&scene!=2)return readWord(flow+8)<8?press(5,"skip"):false;
    if(menuNavFixture&&step==0&&scene==2&&list){static unsigned probe=0;static const DWORD candidates[]={1,21,17,25,1,21,17};
        if(probe<7){log("MENU_FIXTURE nav before action=%lu selection=%ld",probe?candidates[probe-1]:0ul,(LONG)selection);menuNavAction=candidates[probe++];last=elapsed;return false;}
        log("MENU_FIXTURE nav final selection=%ld",(LONG)selection);step=6;if(gameWindow)PostMessageW(gameWindow,WM_CLOSE,0,0);return false;}
    if(step==0&&scene==2&&list){if(!menuAt){menuAt=elapsed;presentsAtMenu=presentCalls;keyboardBindingsLog(true);}if(selection==2){step=1;return press(10,"accept main menu Options");}nativeListSelect(list,2);last=elapsed;{char ids[512]="";DWORD count=readWord(manager+0x404);for(DWORD i=0;i<count&&i<128;i++){char item[16];_snprintf_s(item,sizeof(item),_TRUNCATE," %lu",readWord(manager+8+i*8));strcat_s(ids,item);}log("MENU_FIXTURE registry count=%lu ids:%s",count,ids);}log("MENU_FIXTURE cursor set to Options");return false;}
    if(step==1&&scene==0x11){log("MENU_FIXTURE options selection=%lu expected=1",selection);if(selection!=1){log("MENU_FIXTURE FAIL cursor not on Audio");step=99;return false;}step=2;return press(10,"accept Audio");}
    if(step==2&&scene==0x12){if(elapsed-last<4000)return false;step=3;return press(11,"back from Audio");}
    if(step==3&&scene==0x11){if(selection!=0){nativeListSelect(list,0);last=elapsed;log("MENU_FIXTURE cursor set to Save Game");return false;}step=4;return press(10,"accept Save Game (must be ignored)");}
    if(step==4&&scene==0x11){log("MENU_FIXTURE save entry ignored scene=%lu",scene);step=5;return press(11,"back to main menu");}
    if(step==5&&scene==2){ULONGLONG ms=elapsed-menuAt;log("MENU_FIXTURE PASS fps_at_menu=%.1f volumes=%lu/%lu/%lu layout=%lu",ms?(presentCalls-presentsAtMenu)*1000.0/ms:0.0,readWord(0x69c62c),readWord(0x69c630),readWord(0x69c634),readWord(0x69efa4));step=6;if(!menuContinueFixture&&gameWindow)PostMessageW(gameWindow,WM_CLOSE,0,0);return false;}
    if(step==4&&scene==0x27){log("MENU_FIXTURE FAIL Save Game opened from the main menu");step=99;}
    // Continue check (COOP_PROBE_MENU_FIXTURE=2, a save seeded in slot 1): volumes chosen in the main menu must survive
    // loading the save. 2/3/4 stand in for the player's slider changes (the Audio page writes them the same way).
    if(menuContinueFixture&&step==6&&scene==2&&list){*(DWORD*)0x69c62c=2;*(DWORD*)0x69c630=3;*(DWORD*)0x69c634=4;((void(__cdecl*)())0x4d7c00)();log("MENU_FIXTURE volumes set 2/3/4");
        nativeListSelect(list,1);step=7;last=elapsed;return false;}
    if(menuContinueFixture&&step==7&&scene==2){step=8;return press(10,"accept Continue");}
    if(menuContinueFixture&&step==8&&scene==0x27){DWORD slots=object?readWord(object+0x88):0;if(slots&&readWord(slots+0x8d08)!=0){nativeListSelect(slots,0);last=elapsed;return false;}return press(10,"accept saved game");}
    if(menuContinueFixture&&step>=8&&step<10&&scene==3)return press(10,"confirm");
    if(menuContinueFixture&&step>=8&&step<10&&readWord(flow+8)>=16&&scene!=2&&scene!=0x27&&scene!=3){if(step==8){step=9;last=elapsed;return false;}
        DWORD v[3]={readWord(0x69c62c),readWord(0x69c630),readWord(0x69c634)};log("MENU_FIXTURE continue volumes=%lu/%lu/%lu %s",v[0],v[1],v[2],v[0]==2&&v[1]==3&&v[2]==4?"CONTINUE_PASS":"CONTINUE_FAIL");
        step=10;if(gameWindow)PostMessageW(gameWindow,WM_CLOSE,0,0);return false;}
    return false;
}
static void __cdecl inputUpdate(float dt){
    if(campaignStopping()){applyNetworkActions(0,{});applyNetworkActions(1,{});return;}
    campaignBotSkipNis();
    DWORD currentFlow=readWord(0x69e72c);if(networkLockstep()&&currentFlow&&readWord(currentFlow+8)==128)dt=1.0f/60.0f;
    // Launching the owned research scenario must not depend on a connected pad.
    // Use the game's existing forced-action field for one boot-menu frame only;
    // leave physical polling and every in-game input untouched.
    static ULONGLONG bootStarted=0;static DWORD bootPulse=~0u;
    ULONGLONG bootNow=GetTickCount64();if(!bootStarted)bootStarted=bootNow;
    DWORD forcedAddress=0x691170+0x118,savedForced=readWord(forcedAddress);bool bootAction=false;
    if((autoTest||campaignFixture)&&(networkRole||networkDeferred)&&humanInputMode&&!humanInputFixture&&currentFlow&&readWord(currentFlow+8)<8&&bootNow-bootStarted>=4000){
        DWORD pulse=(DWORD)((bootNow-bootStarted-4000)/2500);
        if(pulse!=bootPulse){bootPulse=pulse;*(DWORD*)forcedAddress=pulse%2?5:10;bootAction=true;log("BOOTSTRAP menu action=%lu flow=%lu",pulse%2?5ul:10ul,readWord(currentFlow+8));}
    }
    if(menuFixture&&!bootAction)bootAction=menuFixtureStep(currentFlow,bootNow-bootStarted,forcedAddress);
    ((void(__cdecl*)(float))0x401b6f)(dt);
    if(bootAction)*(DWORD*)forcedAddress=savedForced;
    if(menuNavFixture){coop::Actions a={};if(menuNavAction<28)a[menuNavAction]=1.0f;menuNavAction=~0u;if(readWord(readWord(0x6a9940)+0x4e4)==2)applyNetworkActions(0,a);}
    static ULONGLONG last=0;DWORD flow=readWord(0x69e72c);ULONGLONG now=GetTickCount64();
    campaignHostLeaveMissionForJoin();
    if(campaignMode&&networkDeferred){if(flow&&readWord(flow+8)>=4&&!campaignSoloLoaded){campaignSoloLoaded=true;log("CAMPAIGN SOLO_LOADED flow=%lu (a partner joins at the hub)",readWord(flow+8));}campaignTryConnect();}
    if(campaignLive()&&flow&&readWord(flow+8)!=128){
        campaignPhysicalMenu=captureLocalActions();if(readWord(flow+8)>=8){campaignNeutralMenu();if(campaignHub&&!campaignMission)campaignApplyHubMovement();}
        // Test bots (COOP_PROBE_SKIP_NIS): the movie screen (49FED0) skips on a just-pressed abstract 14 (Start) while
        // its player 6A9980 is playing (+4 == 1); press it on slot 0 after the menu clear, once a second.
        if(campaignSkipNis&&campaignFixture){DWORD movie=readWord(0x6a9980);static unsigned moviePulse=0,movieSkips=0;
            if(movie&&readWord(movie+4)==1&&++moviePulse%60==0){DWORD raw=readWord(readWord(0x69e56c)+14*4);if(raw<28){*(DWORD*)(0x691170+4+raw*4)=3;log("BOT MOVIE_SKIP press=%u raw=%lu",++movieSkips,raw);}}}
        // A player skips a movie on their own PC (owner report 2026-10-24: "the movie doesn't skip"): the menu clear above
        // also removed this player's own Start. Each PC plays movies on its own and the one ahead waits at the next barrier,
        // so this player's just-pressed Start (abstract 14 through the game's own mapping) goes back to the movie screen.
        // Research: COOP_PROBE_MOVIE_SKIP_TEST=1 presses it for a test bot once a second.
        {DWORD movie=readWord(0x6a9980),raw=readWord(readWord(0x69e56c)+14*4);static bool startWasDown=false;static unsigned testPulse=0;
            bool down=raw<28&&campaignPhysicalMenu[raw]>0.5f;if(campaignMovieSkipTest&&movie&&readWord(movie+4)==1)down=++testPulse%60<2;
            if((!campaignFixture||campaignMovieSkipTest)&&movie&&readWord(movie+4)==1&&raw<28&&down&&!startWasDown){*(DWORD*)(0x691170+4+raw*4)=3;log("CAMPAIGN MOVIE_SKIP by this player raw=%lu",raw);}
            startWasDown=down;}
        static DWORD previousScene=~0u;DWORD manager=readWord(0x6a9940),nativeScene=readWord(manager+0x4e4);
        if(manager&&nativeScene!=previousScene){previousScene=nativeScene;DWORD count=readWord(manager+0x404);if(count>128)campaignReject("native scene registry count");for(DWORD i=0;i<count;i++)if(readWord(manager+8+i*8)==nativeScene){DWORD object=readWord(manager+4+i*8),table=readWord(object);log("CAMPAIGN UI_OBSERVE native_scene=%lu table=%08lx update=%08lx operation=%lu",nativeScene,table,readWord(table+0x4c),readWord(0x69eb0c));break;}}
    }
    if(campaignMode&&campaignGameplay){static DWORD previousGameplayScene=~0u;DWORD nativeScene=readWord(readWord(0x6a9940)+0x4e4);if(nativeScene!=previousGameplayScene){previousGameplayScene=nativeScene;log("CAMPAIGN UI_OBSERVE_GAMEPLAY native_scene=%lu flow=%lu tick=%u",nativeScene,flow?readWord(flow+8):0,networkTick);}}
    if(networkLockstep()&&flow&&readWord(flow+8)==128&&(!campaignMode||campaignStartGameplay()))networkInput();
    if(!networkRole&&actionBot&&flow&&readWord(flow+8)==128&&playBotStarted){
        static bool previous[2][28]={0};ULONGLONG age=now-playBotStarted;
        for(unsigned player=0;player<2;player++){
            float actions[28]={0};
            if(age>=20000){
                if((age-20000)%3000<100)actions[10]=1.0f;
                unsigned phase=(unsigned)((age-20000)/2500)%4;
                unsigned direction[4]={19,16,18,17};actions[direction[(phase+player*2)%4]]=0.75f;
                if(age%1200<100)actions[12]=1.0f;
            }
            BYTE* context=(BYTE*)(0x691170+player*0x11c);DWORD* states=(DWORD*)(context+4);float* durations=(float*)(context+0x74);
            for(unsigned action=0;action<28;action++){bool down=actions[action]>0.5f;states[action]=(down?1u:0u)|(down!=previous[player][action]?2u:0u);durations[action]=down==previous[player][action]?durations[action]+dt:0.0f;previous[player][action]=down;}
            memcpy(context+0xf4,context+0xe4,16);float axes[4]={actions[19]-actions[18],actions[16]-actions[17],0,0};memcpy(context+0xe4,axes,sizeof(axes));
        }
    }
    if(flow&&readWord(flow+8)==128&&now-last>=1000){last=now;observePlayers();}
}
static FlowNativeFn originalAutoNative=0;
static void __fastcall autoNative(void* vm,void*,unsigned id){
    // Same flag written by shipped native27 / InGame's interactive mode.
    // Without it, 45249F forces would-be player characters to AI controllers.
    if(id==21&&(playBot||humanInputMode)){*(BYTE*)0x6a8538=1;log("AUTOTEST interactive input enabled via native27 flag before level setup");
        if(networkRole){if(humanInputMode){networkInputClock=true;for(unsigned slot=0;slot<4;slot++)*(DWORD*)(0x691170+slot*0x11c+0x110)=10000;*(DWORD*)0x6a98dc=10000;}
            if(!prepareNetworkPlayers()||!prepareTaskTiming()){networkFailed=true;log("NET FAIL controller or task-clock preparation");}*(DWORD*)0x58cf24=8466;networkAmbientRng=0x84f19a21;log("NET scenario seed set before load seed=8466 path_sync=%u ambient_split=%u",networkPathSync,networkAmbientSplit);}
    }
    bool timeoutQuery=false;
    if(id==13){DWORD* stack=*(DWORD**)((BYTE*)vm+4);timeoutQuery=stack&&stack[-2]&&!strcmp((char*)stack[-2],"/user/test/timeout");}
    DWORD* flow=*(DWORD**)0x69e72c;DWORD* movie=*(DWORD**)0x6a9980;
    bool movieBusy=*(DWORD*)0x69eb00!=0||(movie&&movie[1]==1);
    if(deferredGameplayRequest&&flow&&flow[2]==16&&flow[3]==16&&!movieBusy){flow[3]=32;deferredGameplayRequest=false;log("AUTOTEST deferred request released after movie completion, normal unload=32");}
    if(id==24){
        DWORD* stack=*(DWORD**)((BYTE*)vm+4);
        // The shipped test requests loading directly. A retail frontend owns
        // pooled objects until its normal unload transition (32 -> 64).
        if(stack&&flow&&stack[-1]==64&&flow[2]==16&&flow[3]==16){stack[-1]=movieBusy?16:32;deferredGameplayRequest=movieBusy;log("AUTOTEST transition request 64 routed via movie wait=%u then normal unload=32 movie_queue=%lu movie_state=%lu",movieBusy?1:0,*(DWORD*)0x69eb00,movie?movie[1]:~0u);}
    }
    originalAutoNative(vm,id);
    if(networkRole&&id==26&&flow&&flow[2]==128){DWORD* flags=(DWORD*)((BYTE*)vm+0x24);*flags=(*flags&~6u)|5u;log("AUTOTEST bootstrap finished; timed reload script stopped, gameplay remains active");}
    if(timeoutQuery){float* stack=*(float**)((BYTE*)vm+4);stack[-1]=autoGameSeconds;log("AUTOTEST gameplay observation seconds=%g",autoGameSeconds);}
}
static FlowNativeFn originalFlowNative=0;static unsigned flowNativeCalls=0,lastFlowNative=~0u;
static void __fastcall flowNative(void* vm,void*,unsigned id){
    unsigned n=++flowNativeCalls;if(probeDiagnostics&&(n<=180||id!=lastFlowNative||n%10000==0)){DWORD fields[10]={0},args[4]={0};SIZE_T got=0;
        if(ReadProcessMemory(GetCurrentProcess(),vm,fields,sizeof(fields),&got)&&got==sizeof(fields)&&fields[1]>=16)ReadProcessMemory(GetCurrentProcess(),(void*)(fields[1]-16),args,sizeof(args),&got);
        log("FLOW native count=%u id=%u vm=%p pc=%08lx status=%08lx args=%08lx,%08lx,%08lx,%08lx",n,id,vm,fields[6],fields[9],args[0],args[1],args[2],args[3]);}
    lastFlowNative=id;if(campaignMode&&!networkDeferred&&id==23)campaignBeforeLoad();
    if(campaignStopping()){
        DWORD* flags=(DWORD*)((BYTE*)vm+0x24);*flags=(*flags&~6u)|5u;
        log("CAMPAIGN STOP native dispatcher suppressed id=%u; current VM completed for shutdown",id);return;
    }
    // Flow native 6 loads one FE scene (id on top of the script stack, 4D9121 -> 4E5FFF). The front end loads 17,
    // without the Options/Audio screens the hub has; load those two right after the main menu's own Options (1A).
    // Scenes loaded earlier are drawn on top (run menu-audio-011: Audio loaded after Options stayed hidden under it).
    DWORD loadScene=id==6?readWord(readWord((DWORD)(ULONG_PTR)vm+4)-4):~0u;
    originalFlowNative(vm,id);
    if(mainMenuAudio&&loadScene==0x1a&&!sceneObject(0x11)&&!sceneObject(0x12)){((void(__cdecl*)(DWORD))0x4e5fff)(0x12);((void(__cdecl*)(DWORD))0x4e5fff)(0x11);
        log("OPTIONS front end also loads scenes 11 (Options) and 12 (Audio): %s",sceneObject(0x11)&&sceneObject(0x12)?"ok":"missing");}
}
// 45CB09 builds the 8 character objects once at startup; the first 3 get the large (hero) size. Make it 4, so a
// hero-size object is left free in the hub, where objects 0..2 are all in use (run hubspawn-124).
static bool campaignBigCharacterObjects(){
    BYTE* code=(BYTE*)0x45cb4d;if(code[0]!=0x83||code[1]!=0xf8||code[2]!=0x03){log("FAIL big character object guard");return false;}
    DWORD old=0;if(!VirtualProtect(code,3,PAGE_EXECUTE_READWRITE,&old))return false;code[2]=0x04;DWORD ignored=0;VirtualProtect(code,3,old,&ignored);FlushInstructionCache(GetCurrentProcess(),code,3);
    log("HUBSPAWN startup builds 4 hero-size character objects");return true;
}
static bool patchVerifiedCall(DWORD address,DWORD expected,void* replacement){
    BYTE* at=(BYTE*)address;if(at[0]!=0xe8||(DWORD)(address+5+*(LONG*)(at+1))!=expected){log("FAIL AUTOTEST call signature address=%08lx",address);return false;}
    DWORD old=0;if(!VirtualProtect(at,5,PAGE_EXECUTE_READWRITE,&old))return false;*(DWORD*)(at+1)=(DWORD)(ULONG_PTR)replacement-(address+5);FlushInstructionCache(GetCurrentProcess(),at,5);DWORD ignored=0;bool ok=VirtualProtect(at,5,old,&ignored)!=0;log("AUTOTEST verified call patched address=%08lx original=%08lx",address,expected);return ok;
}
static bool patchIndirectUser32Call(DWORD address,DWORD iat,const char* name,void* replacement){
    BYTE prior[6]={};DWORD import=0;FARPROC expected=GetProcAddress(GetModuleHandleW(L"user32.dll"),name);
    if(!expected||!readMemory((void*)address,prior,6)||prior[0]!=0xff||prior[1]!=0x15||memcmp(prior+2,&iat,4)||!readMemory((void*)iat,&import,4)||import!=(DWORD)(ULONG_PTR)expected){
        log("FAIL key-name call/IAT signature address=%08lx name=%s",address,name);return false;}
    BYTE patch[6]={0xe8,0,0,0,0,0x90};DWORD delta=(DWORD)(ULONG_PTR)replacement-(address+5);memcpy(patch+1,&delta,4);
    DWORD old=0;if(!VirtualProtect((void*)address,6,PAGE_EXECUTE_READWRITE,&old))return false;
    memcpy((void*)address,patch,6);bool flushed=FlushInstructionCache(GetCurrentProcess(),(void*)address,6)!=0;DWORD ignored=0;bool restored=VirtualProtect((void*)address,6,old,&ignored)!=0;
    log("INPUT verified key-name call patched address=%08lx name=%s protection_restored=%u",address,name,restored?1u:0u);return flushed&&restored;
}
static bool patchKeyboardLayoutCall(){
    if(!patchIndirectUser32Call(0x52671f,0x5562a4,"MapVirtualKeyExW",(void*)keyNameMapVirtualKey)||
       !patchIndirectUser32Call(0x52673a,0x556234,"ToUnicodeEx",(void*)keyNameToUnicode))return false;
    // Exact-build FF15 [GetKeyboardLayout IAT] at52653C; E8+nop keeps all6
    // bytes and its stdcall argument/return contract. No process-wide IAT edit.
    const DWORD address=0x52653c,iat=0x5562a8;BYTE prior[6]={};DWORD import=0;
    FARPROC expected=GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetKeyboardLayout");
    if(!expected||!readMemory((void*)address,prior,6)||prior[0]!=0xff||prior[1]!=0x15||
       memcmp(prior+2,&iat,4)||!readMemory((void*)iat,&import,4)||import!=(DWORD)(ULONG_PTR)expected){
        log("FAIL canonical keyboard layout call/IAT signature");return false;
    }
    BYTE patch[6]={0xe8,0,0,0,0,0x90};DWORD delta=(DWORD)(ULONG_PTR)canonicalKeyboardLayout-(address+5);
    memcpy(patch+1,&delta,4);DWORD old=0;if(!VirtualProtect((void*)address,6,PAGE_EXECUTE_READWRITE,&old))return false;
    memcpy((void*)address,patch,6);bool flushed=FlushInstructionCache(GetCurrentProcess(),(void*)address,6)!=0;
    DWORD ignored=0;bool restored=VirtualProtect((void*)address,6,old,&ignored)!=0;
    log("INPUT verified initializer layout call patched address=%08lx original_iat=%08lx protection_restored=%u",address,iat,restored);
    return flushed&&restored;
}
// Audio settings in the main menu (owner request; the retail main menu has none). The main menu's Options entry
// (49A554 push 1A, main_menu_options.fen: video and controller only) opens scene 11 instead, the Options screen the
// hub uses (options.fen: game, audio, video, controller); flowNative loads it and the Audio screen (12) in the front
// end. Its first entry opens scene 16 (Save Game), which needs a loaded game and is not loaded in the front end, so
// it does nothing while that screen was opened from the main menu (scene 2, choosegame.fen, update 49A4B8), and the
// cursor starts on Audio there. Runs menu-audio-012 (screens) and 014 (Continue keeps the volumes).
static bool optionsFromMainMenu(){DWORD manager=readWord(0x6a9940),depth=readWord(manager+0x4e0);return manager&&depth&&depth<=8&&readWord(manager+0x4e4)==0x11&&readWord(manager+0x4c0+(depth-1)*4)==2;}
static bool __cdecl optionsAccept(DWORD* winner){
    bool accepted=((bool(__cdecl*)(DWORD*))0x497e78)(winner);DWORD scene=sceneObject(0x11),list=scene?readWord(scene+0x84):0;
    if(accepted&&list&&readWord(list+0x8d08)==0&&optionsFromMainMenu()){log("OPTIONS main menu: Save Game entry ignored without a loaded game");return false;}
    return accepted;
}
typedef void (__fastcall* SceneActivateFn)(void*,void*);static SceneActivateFn originalOptionsActivate=0;
static void __fastcall optionsActivate(void* scene,void* edx){
    originalOptionsActivate(scene,edx);DWORD list=readWord((DWORD)(ULONG_PTR)scene+0x84);if(!list||!optionsFromMainMenu())return;
    nativeListSelect(list,1);log("OPTIONS main menu: full Options screen, cursor on Audio");
}
// Continue (save slot list, 4A5149): the save's own option block replaces the current options (4D7AE0 copies, applies
// and writes the registry). Keep this PC's latest volumes (registry VolumeDialogue/Music/Sfx at options+1BC..1C4,
// which the Audio page writes, now also from the main menu) instead of the volumes stored in the save.
static void __cdecl continueOptions(const BYTE* saved){
    BYTE copy[0x1fc];memcpy(copy,saved,sizeof(copy));memcpy(copy+0x1bc,(const void*)0x69c62c,12);
    log("OPTIONS continue keeps this PC's volumes %lu/%lu/%lu (save had %lu/%lu/%lu)",readWord(0x69c62c),readWord(0x69c630),readWord(0x69c634),*(const DWORD*)(saved+0x1bc),*(const DWORD*)(saved+0x1c0),*(const DWORD*)(saved+0x1c4));
    ((void(__cdecl*)(const BYTE*))0x4d7ae0)(copy);
}
static bool installMainMenuAudio(){
    BYTE* push=(BYTE*)0x49a554;void** table=(void**)0x56d698;
    if(push[0]!=0x6a||push[1]!=0x1a||push[2]!=0xeb||table[9]!=(void*)0x4a0823||table[19]!=(void*)0x4a0702){log("FAIL main menu options guard");return false;}
    DWORD old=0;if(!VirtualProtect(push+1,1,PAGE_EXECUTE_READWRITE,&old))return false;push[1]=0x11;DWORD ignored=0;VirtualProtect(push+1,1,old,&ignored);FlushInstructionCache(GetCurrentProcess(),push,2);
    if(!patchComSlot(table,9,(void*)optionsActivate,(void**)&originalOptionsActivate)||!patchVerifiedCall(0x4a0734,0x497e78,(void*)optionsAccept)||
       !patchVerifiedCall(0x4a5149,0x4d7ae0,(void*)continueOptions))return false;
    mainMenuAudio=true;log("OPTIONS main menu opens the full Options screen (audio included)");return true;
}
static bool installCampaign(){
    struct Entry{DWORD table,callback;void* hook;CampaignUpdate* original;};
    Entry entries[]={{0x56c0d0,0x49a4b8,(void*)campaignMainUpdate,&originalCampaignMain},
        {0x56efe8,0x4a502d,(void*)campaignProfileUpdate,&originalCampaignProfile},
        {0x56ca40,0x49be94,(void*)campaignHubUpdate,&originalCampaignHub},
        {0x56d120,0x49eda6,(void*)campaignMissionUpdate,&originalCampaignMission},
        {0x56f090,0x4a5e8a,(void*)campaignSaveAckUpdate,&originalCampaignSaveAck},
        {0x56c1c0,0x49a6db,(void*)campaignAutosaveNoticeUpdate,&originalCampaignAutosaveNotice},
        {0x56f298,0x4a6c6f,(void*)campaignSidekickUpdate,&originalCampaignSidekick},
        {0x56d3d8,0x49f80e,(void*)campaignPopupUpdate,&originalCampaignPopup}};
    if(readWord(0x57e6d8+0x18)!=0x51a6cc||readWord(0x57e6d8+0x1c)!=0x4337ec){log("FAIL CAMPAIGN native save task callback guard");return false;}
    for(const auto& entry:entries){void** table=(void**)entry.table;if(table[19]!=(void*)entry.callback||!patchComSlot(table,19,entry.hook,(void**)entry.original)){log("FAIL CAMPAIGN native update vtable guard table=%08lx",entry.table);return false;}}
    void** mission=(void**)0x56d120;if(mission[14]!=(void*)0x49f107||!patchComSlot(mission,14,(void*)campaignMissionDeactivate,(void**)&originalCampaignDeactivate))return false;
    return patchVerifiedCall(0x49a4e8,0x497e78,(void*)campaignMainAccept)&&
        patchVerifiedCall(0x4a522f,0x49a5cd,(void*)campaignSlotPromptBridge)&&
        patchVerifiedCall(0x4a554d,0x49a5cd,(void*)campaignAutosaveConfigureBridge)&&patchVerifiedCall(0x49a76a,0x497e78,(void*)campaignAutosaveAccept)&&
        patchVerifiedCall(0x4a523f,0x4a582a,(void*)campaignProfileInitBridge)&&patchVerifiedCall(0x4a546b,0x4a582a,(void*)campaignProfileInitBridge)&&
        patchVerifiedCall(0x525c05,0x528bc0,(void*)campaignMouseUpdate)&&
        // Research only (COOP_PROBE_HUB_SPAWN): ordinary play never routes the spawn native through the probe.
        (campaignHubSpawnIndex<0||(patchVerifiedCall(0x4d92a4,0x4d9f45,(void*)campaignSpawnCharacter)&&patchVerifiedCall(0x4d90eb,0x4e5b00,(void*)campaignLoadCharacterBridge)))&&
        ((!campaignHubPartner&&campaignHubSpawnIndex<0)||(patchVerifiedCall(0x4e5cb8,0x4e4feb,(void*)campaignBuildDescriptor)&&patchVerifiedCall(0x4e6261,0x4e629e,(void*)campaignLevelMemory)&&campaignBigCharacterObjects()));
}
static bool install() {
    BYTE* base=(BYTE*)GetModuleHandleW(0);auto dos=(IMAGE_DOS_HEADER*)base;auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    if(!selfTest){const BYTE expected[]={0xe8,3,4,0,0,0xe9,0x35,0xfd,0xff,0xff};if((ULONG_PTR)base!=0x400000||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||nt->OptionalHeader.AddressOfEntryPoint!=0x12fd2c){log("FAIL game PE guard");return false;}
        // Entrypoint: the original instructions (the d3d9 proxy loads this probe at the entry and restores them first),
        // or the first two bytes still being the older injection launcher's EB FE gate.
        BYTE* ep=base+nt->OptionalHeader.AddressOfEntryPoint;bool gated=ep[0]==0xeb&&ep[1]==0xfe;if((!gated&&memcmp(ep,expected,2))||memcmp(ep+2,expected+2,sizeof(expected)-2)){log("FAIL entry signature");return false;}}
    auto desc=(IMAGE_IMPORT_DESCRIPTOR*)(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for(;desc->Name;desc++){auto names=desc->OriginalFirstThunk?(IMAGE_THUNK_DATA*)(base+desc->OriginalFirstThunk):0;auto slots=(IMAGE_THUNK_DATA*)(base+desc->FirstThunk);const char* dllName=(const char*)(base+desc->Name);HMODULE dependency=GetModuleHandleA(dllName);if(!dependency){log("FAIL dependency absent %s",dllName);return false;}
        log("imports module=%s names=%s",dllName,names?"OFT":"resolved-IAT-export-match");
        for(;slots->u1.Function;slots++){const char* name=0;if(names){if(!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))name=(const char*)((IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData))->Name;names++;}
            for(auto&h:hooks){bool match=name?!strcmp(name,h.name):(!desc->OriginalFirstThunk&&(ULONG_PTR)GetProcAddress(dependency,h.name)==slots->u1.Function);if(match){DWORD old;if(!VirtualProtect(&slots->u1.Function,sizeof(void*),PAGE_READWRITE,&old))return false;if(h.original)*h.original=(void*)(ULONG_PTR)slots->u1.Function;slots->u1.Function=(ULONG_PTR)h.replacement;DWORD ignore;if(!VirtualProtect(&slots->u1.Function,sizeof(void*),old,&ignore))return false;h.found=true;log("IAT hooked %s",h.name);break;}}}}
    for(auto&h:hooks)if(!h.found){
        bool required=!selfTest || !strcmp(h.name,"SHGetFolderPathW") || !strcmp(h.name,"RegCreateKeyW") || !strcmp(h.name,"RegSetValueExW") || !strcmp(h.name,"RegQueryValueExW") || !strcmp(h.name,"RegCloseKey") || !strcmp(h.name,"CreateFileW") || !strcmp(h.name,"WriteFile");
        if(required){log("FAIL required IAT missing %s",h.name);return false;}
    }
    if(!selfTest&&autoTest){if(!patchVerifiedCall(0x4dc0e0,0x43754f,(void*)testEnabled)||!patchVerifiedCall(0x41118b,0x437d6b,(void*)testNameExists)||!patchVerifiedCall(0x411196,0x437515,(void*)testNameValue)||!patchVerifiedCall(0x4d870f,0x437515,(void*)testNameValue))return false;
        void** table=(void**)0x57713c;if(table[2]!=(void*)0x4d84bb||!patchComSlot(table,2,(void*)autoNative,(void**)&originalAutoNative)){log("FAIL automated test dispatcher guard");return false;}
    }
    if(!selfTest){void** table=(void**)0x5772a0;if(table[2]!=(void*)0x4d9065||!patchComSlot(table,2,(void*)flowNative,(void**)&originalFlowNative)){log("FAIL flow dispatcher guard");return false;}}
    if(!selfTest&&!patchVerifiedCall(0x4dbab1,0x401b6f,(void*)inputUpdate))return false;
    if(!selfTest&&(!installLaserFix()||!installDrawListFix()))return false;
    if(!selfTest&&(networkRole||networkDeferred)){void** slot=(void**)0x577e20;if(slot[0]!=(void*)0x4dbc39||!patchComSlot(slot,0,(void*)simulation,(void**)&originalSimulation)){log("FAIL simulation callback guard");return false;}}
    if(!selfTest&&(networkRole||networkDeferred))for(DWORD address:rngCallSites)if(!patchVerifiedCall(address,0x4318f5,(void*)randomEntry))return false;
    if(!selfTest&&humanKeyboardFixture&&!patchVerifiedCall(0x525c00,0x526240,(void*)keyboardPollFixture))return false;
    if(!selfTest&&humanInputMode)for(DWORD address:deviceCallSites)if(!patchVerifiedCall(address,0x40182d,(void*)reassignLocalDevice))return false;
    if(!selfTest&&humanInputMode&&(!patchKeyboardLayoutCall()||!patchVerifiedCall(0x52a770,0x525c30,(void*)guardedKeyLookup)))return false;
    if(campaignMode&&!installCampaign())return false;
    if(!selfTest&&!autoTest&&!installMainMenuAudio())return false;
    if(campaignMode&&(networkRole||networkDeferred)&&(!installNisGate()||!installExitGate()||!installNisStopGate()||!installStageLog()))return false;
    return true;
}
// Research: a hardware write watch on character entry 1's memory owner (6A8808+0x58+0x20 = 6A8880) on the game
// thread, to find what prepares the sidekick entry in a mission. Armed by raising 0xE0C0FFEE from the hub hook.
static LONG campaignWatchHits=0;
static LONG CALLBACK exception(EXCEPTION_POINTERS* p){
    if(campaignHubSpawnIndex>=0&&p&&p->ExceptionRecord&&p->ContextRecord){
        auto e=p->ExceptionRecord;auto c=p->ContextRecord;
        if(e->ExceptionCode==0xE0C0FFEE){c->Dr0=0x6a8880;c->Dr7=(c->Dr7&~0xf0003ul)|1ul|(1ul<<16)|(3ul<<18);c->ContextFlags|=CONTEXT_DEBUG_REGISTERS;log("HUBSPAWN watch armed on entry 1 owner thread=%lu",GetCurrentThreadId());return EXCEPTION_CONTINUE_EXECUTION;}
        if(e->ExceptionCode==EXCEPTION_SINGLE_STEP&&(c->Dr6&1)){c->Dr6=0;LONG n=InterlockedIncrement(&campaignWatchHits);
            if(n<=24){DWORD ret0=0,ret1=0,ret2=0;readMemory((void*)c->Esp,&ret0,4);readMemory((void*)(c->Esp+4),&ret1,4);readMemory((void*)(c->Esp+8),&ret2,4);
                log("HUBSPAWN watch hit=%ld eip=%08lx value=%08lx eax=%08lx ecx=%08lx esi=%08lx edi=%08lx stack=%08lx,%08lx,%08lx",n,c->Eip,readWord(0x6a8880),c->Eax,c->Ecx,c->Esi,c->Edi,ret0,ret1,ret2);}
            return EXCEPTION_CONTINUE_EXECUTION;}
    }
    LONG n=InterlockedIncrement(&exceptionCount);if(n<=5&&p&&p->ExceptionRecord&&p->ContextRecord){auto e=p->ExceptionRecord;auto c=p->ContextRecord;MEMORY_BASIC_INFORMATION m={};char module[MAX_PATH]={0};if(VirtualQuery((LPCVOID)c->Eip,&m,sizeof(m))&&m.AllocationBase)GetModuleFileNameA((HMODULE)m.AllocationBase,module,MAX_PATH);
        log("EXCEPTION count=%ld code=%08lx flags=%08lx ip=%08lx module=%s modulebase=%p relative=%08lx params=%lu accesskind=%08lx faultaddress=%08lx EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx ESI=%08lx EDI=%08lx EBP=%08lx ESP=%08lx",n,e->ExceptionCode,e->ExceptionFlags,c->Eip,module,m.AllocationBase,c->Eip-(DWORD)(ULONG_PTR)m.AllocationBase,e->NumberParameters,e->NumberParameters>0?(DWORD)e->ExceptionInformation[0]:0,e->NumberParameters>1?(DWORD)e->ExceptionInformation[1]:0,c->Eax,c->Ebx,c->Ecx,c->Edx,c->Esi,c->Edi,c->Ebp,c->Esp);
        DWORD stack[16]={0};SIZE_T got=0;if(ReadProcessMemory(GetCurrentProcess(),(LPCVOID)c->Esp,stack,sizeof(stack),&got)&&got==sizeof(stack)){
            log("EXCEPTION stack %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",stack[0],stack[1],stack[2],stack[3],stack[4],stack[5],stack[6],stack[7],stack[8],stack[9],stack[10],stack[11],stack[12],stack[13],stack[14],stack[15]);
        }
    }return EXCEPTION_CONTINUE_SEARCH;
}
BOOL WINAPI DllMain(HINSTANCE h,DWORD why,LPVOID){if(why!=DLL_PROCESS_ATTACH)return TRUE;DisableThreadLibraryCalls(h);
    wchar_t flag[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_SELFTEST",flag,8);selfTest=!wcscmp(flag,L"1");
    wchar_t campaignOption[8]={0};DWORD campaignLength=GetEnvironmentVariableW(L"COOP_PROBE_CAMPAIGN",campaignOption,8);
    if(campaignLength){if(campaignLength>=8||(wcscmp(campaignOption,L"0")&&wcscmp(campaignOption,L"1")))return FALSE;campaignMode=!wcscmp(campaignOption,L"1");}
    wchar_t campaignBot[8]={0};DWORD campaignBotLength=GetEnvironmentVariableW(L"COOP_PROBE_CAMPAIGN_FIXTURE",campaignBot,8);
    if(campaignBotLength){if(campaignBotLength>=8||(wcscmp(campaignBot,L"0")&&wcscmp(campaignBot,L"1")))return FALSE;campaignFixture=!wcscmp(campaignBot,L"1");}
    wchar_t testMode[32]={0};GetEnvironmentVariableW(L"COOP_PROBE_AUTOTEST",testMode,32);autoTest=!wcscmp(testMode,L"InGame")||!wcscmp(testMode,L"GoThroughFrontEnd")||!wcscmp(testMode,L"ExecutionPath");if(autoTest)WideCharToMultiByte(CP_ACP,0,testMode,-1,autoTestName,sizeof(autoTestName),0,0);
    wchar_t botMode[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_INPUTBOT",botMode,8);inputBot=!selfTest&&!wcscmp(botMode,L"1");
    wchar_t playMode[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_PLAYBOT",playMode,8);playBot=inputBot&&!wcscmp(playMode,L"1");
    wchar_t actionMode[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_ACTIONBOT",actionMode,8);actionBot=playBot&&!wcscmp(actionMode,L"1");
    wchar_t humanMode[8]={0},fixtureMode[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_HUMAN",humanMode,8);humanInputMode=!selfTest&&!wcscmp(humanMode,L"1");GetEnvironmentVariableW(L"COOP_PROBE_HUMAN_FIXTURE",fixtureMode,8);humanInputFixture=humanInputMode&&!wcscmp(fixtureMode,L"1");
    wchar_t disconnectOption[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_DISCONNECT_FIXTURE",disconnectOption,8);humanDisconnectFixture=humanInputFixture&&!wcscmp(disconnectOption,L"1");
    wchar_t keyboardOption[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_KEYBOARD_FIXTURE",keyboardOption,8);humanKeyboardFixture=humanInputFixture&&!wcscmp(keyboardOption,L"1");
    wchar_t jitterOption[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_JITTER_FIXTURE",jitterOption,8);networkJitterFixture=!wcscmp(jitterOption,L"1");
    wchar_t mutateOption[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_MUTATE_FIXTURE",mutateOption,8);networkMutateFixture=!wcscmp(mutateOption,L"1");
    wchar_t transitionOption[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_TRANSITION_FIXTURE",transitionOption,8);networkTransitionFixture=!wcscmp(transitionOption,L"1");
    wchar_t missionEndOption[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_MISSION_END_FIXTURE",missionEndOption,8);networkMissionEndFixture=!wcscmp(missionEndOption,L"1");
    wchar_t displayOption[16]={0},resolutionOption[32]={0};DWORD displayLength=GetEnvironmentVariableW(L"COOP_PROBE_DISPLAY_MODE",displayOption,16),resolutionLength=GetEnvironmentVariableW(L"COOP_PROBE_RESOLUTION",resolutionOption,32);
    if(displayLength){if(displayLength>=16)return FALSE;if(!wcscmp(displayOption,L"borderless"))displayMode=1;else if(!wcscmp(displayOption,L"window"))displayMode=2;
        else if(!wcscmp(displayOption,L"original")){displayMode=3;
            // Read-only: the resolution the player saved in the game's own options on this PC (none = game default).
            DWORD x=0,y=0,size=4;displayWidth=displayHeight=0;
            if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Activision\\Spider-Man FoF\\Settings",L"ResolutionX",RRF_RT_REG_DWORD,0,&x,&size)==ERROR_SUCCESS&&(size=4,RegGetValueW(HKEY_CURRENT_USER,L"Software\\Activision\\Spider-Man FoF\\Settings",L"ResolutionY",RRF_RT_REG_DWORD,0,&y,&size)==ERROR_SUCCESS)&&x>=640&&x<=7680&&y>=480&&y<=4320){displayWidth=x;displayHeight=y;}
        }else return FALSE;
        if(displayMode!=3){
        // The game renders at the real pixel size; without this, Windows display scaling (e.g. 125%)
        // reports a smaller monitor and bitmap-stretches the window, which blurs it.
        SetProcessDPIAware();displayWidth=GetSystemMetrics(SM_CXSCREEN);displayHeight=GetSystemMetrics(SM_CYSCREEN);}}
    if(resolutionLength){wchar_t* end=0;if(resolutionLength>=32||!displayMode||displayMode==3)return FALSE;unsigned long width=wcstoul(resolutionOption,&end,10);if(!end||*end!=L'x')return FALSE;unsigned long height=wcstoul(end+1,&end,10);if(!end||*end)return FALSE;displayWidth=width;displayHeight=height;}
    if(!(displayMode==3&&!displayWidth&&!displayHeight)&&(displayWidth<640||displayWidth>7680||displayHeight<480||displayHeight>4320))return FALSE;
    wchar_t cyclesOption[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_MISSION_CYCLES",cyclesOption,8);{unsigned cycles=wcstoul(cyclesOption,0,10);if(cycles>=1&&cycles<=4)campaignCycleTarget=cycles;}
    wchar_t transitionCount[16]={0};if(GetEnvironmentVariableW(L"COOP_PROBE_TRANSITION_COUNT",transitionCount,16)){wchar_t* end=0;unsigned long value=wcstoul(transitionCount,&end,10);if(!end||*end||value<1||value>4)return FALSE;networkTransitionTarget=value;}
    char contentOption[66]={0};DWORD contentLength=GetEnvironmentVariableA("COOP_PROBE_CONTENT_SHA256",contentOption,sizeof(contentOption));if(contentLength){if(contentLength!=64)return FALSE;for(unsigned i=0;i<32;i++){char pair[3]={contentOption[i*2],contentOption[i*2+1],0};if(!isxdigit((unsigned char)pair[0])||!isxdigit((unsigned char)pair[1]))return FALSE;unsigned value=0;sscanf(pair,"%2x",&value);networkContent[i]=(BYTE)value;}networkContentVerified=true;}
    char areaOption[128]={0};DWORD areaLength=GetEnvironmentVariableA("COOP_PROBE_AREA",areaOption,sizeof(areaOption));if(areaLength){if(areaLength>=sizeof(areaOption)||strstr(areaOption,".."))return FALSE;for(const char* c=areaOption;*c;c++)if(!isalnum((unsigned char)*c)&&*c!='/'&&*c!='_')return FALSE;strcpy(networkArea,areaOption);}
    wchar_t networkMode[16]={0};GetEnvironmentVariableW(L"COOP_PROBE_NET_MODE",networkMode,16);if((autoTest||campaignMode)&&(actionBot||humanInputMode)){if(!wcscmp(networkMode,L"host"))networkRole=1;if(!wcscmp(networkMode,L"join"))networkRole=2;
        // Provisional joiner role installs the networked hooks; nothing is exchanged before the real role is known.
        if(!wcscmp(networkMode,L"deferred")&&campaignMode&&!humanInputFixture)networkDeferred=true;}
    {wchar_t partner[4]={0};if(GetEnvironmentVariableW(L"COOP_PROBE_HUB_PARTNER",partner,4)&&!wcscmp(partner,L"0"))campaignHubPartner=false;}
    {wchar_t test[4]={0};campaignSidekickTest=GetEnvironmentVariableW(L"COOP_PROBE_HUB_SIDEKICK_TEST",test,4)&&!wcscmp(test,L"1");}
    {wchar_t test[4]={0};swapFixture=GetEnvironmentVariableW(L"COOP_PROBE_SWAP_FIXTURE",test,4)&&!wcscmp(test,L"1");if(swapFixture&&(!humanInputFixture||networkRole!=1))return FALSE;}
    // Research: COOP_PROBE_AUTO_TOGETHER=0 keeps the plain join-at-hub route even from the main menu (its bot tests).
    {wchar_t test[4]={0};if(GetEnvironmentVariableW(L"COOP_PROBE_AUTO_TOGETHER",test,4)&&!wcscmp(test,L"0"))campaignAutoTogetherEnabled=false;}
    {wchar_t test[4]={0};campaignHubWalkDpad=GetEnvironmentVariableW(L"COOP_PROBE_HUB_WALK_DPAD",test,4)&&!wcscmp(test,L"1");if(campaignHubWalkDpad&&!campaignFixture)return FALSE;}
    {wchar_t test[4]={0};campaignHostContinue=GetEnvironmentVariableW(L"COOP_PROBE_HOST_CONTINUE",test,4)&&!wcscmp(test,L"1");}
    {wchar_t test[4]={0};campaignHostWaitLink=GetEnvironmentVariableW(L"COOP_PROBE_HOST_WAIT_LINK",test,4)&&!wcscmp(test,L"1");if(campaignHostWaitLink&&!campaignFixture)return FALSE;if(campaignHostContinue&&!campaignHostWaitLink)return FALSE;}
    {wchar_t row[8]={0};DWORD length=GetEnvironmentVariableW(L"COOP_PROBE_SLOT_FIXTURE",row,8);if(length){if(length!=1||row[0]<L'0'||row[0]>L'2'||!campaignFixture)return FALSE;campaignSlotFixture=(unsigned)(row[0]-L'0');}}
    {wchar_t answer[8]={0};DWORD length=GetEnvironmentVariableW(L"COOP_PROBE_OVERWRITE_FIXTURE_NO",answer,8);if(length){if(length!=1||answer[0]!=L'1'||!campaignFixture)return FALSE;campaignOverwriteFixtureNo=true;}}
    {wchar_t test[4]={0};menuFixture=GetEnvironmentVariableW(L"COOP_PROBE_MENU_FIXTURE",test,4)&&(!wcscmp(test,L"1")||!wcscmp(test,L"2")||!wcscmp(test,L"3"));menuContinueFixture=menuFixture&&!wcscmp(test,L"2");menuNavFixture=menuFixture&&!wcscmp(test,L"3");if(menuFixture&&(!humanInputMode||campaignFixture||networkRole))return FALSE;}
    {wchar_t sound[4]={0};if(GetEnvironmentVariableW(L"COOP_PROBE_HUB_PARTNER_SOUND",sound,4)&&!wcscmp(sound,L"0"))campaignHubPartnerSound=false;}
    {wchar_t skip[4]={0};campaignSkipNis=GetEnvironmentVariableW(L"COOP_PROBE_SKIP_NIS",skip,4)&&!wcscmp(skip,L"1");}
    {wchar_t test[4]={0};levelEndTest=GetEnvironmentVariableW(L"COOP_PROBE_LEVEL_END_TEST",test,4)&&!wcscmp(test,L"1");{wchar_t mask[16]={0};if(levelEndTest&&GetEnvironmentVariableW(L"COOP_PROBE_LEVEL_END_DONE",mask,16))levelEndDone=wcstoul(mask,0,16);if(levelEndTest&&GetEnvironmentVariableW(L"COOP_PROBE_LEVEL_END_BOSS_WAIT",mask,16))levelEndBossWait=wcstoul(mask,0,10);}if(levelEndTest&&!campaignFixture)return FALSE;}
    {wchar_t test[4]={0};networkNisGateTest=GetEnvironmentVariableW(L"COOP_PROBE_NIS_GATE_TEST",test,4)&&!wcscmp(test,L"1");if(networkNisGateTest&&!campaignFixture)return FALSE;}
    {wchar_t test[4]={0};campaignHubJumpTest=GetEnvironmentVariableW(L"COOP_PROBE_HUB_JUMP_TEST",test,4)&&!wcscmp(test,L"1");if(campaignHubJumpTest&&!campaignFixture)return FALSE;}
    {wchar_t skip[4]={0};campaignMovieSkipTest=GetEnvironmentVariableW(L"COOP_PROBE_MOVIE_SKIP_TEST",skip,4)&&!wcscmp(skip,L"1");if(campaignMovieSkipTest&&!campaignFixture)return FALSE;}
    {wchar_t spawn[8]={0};if(GetEnvironmentVariableW(L"COOP_PROBE_HUB_SPAWN",spawn,8)){wchar_t* end=0;long v=wcstol(spawn,&end,10);if(!end||*end||v<0||(v>31&&v!=99))return FALSE;campaignHubSpawnIndex=(int)v;}}
    {wchar_t profile[32768]={0};DWORD length=GetEnvironmentVariableW(L"COOP_PROBE_PROFILE",profile,32768);
        if(length){if(length>=4096||!fullpath(profile,profileRoot))return FALSE;mkdirs(profileRoot);DWORD a=GetFileAttributesW(profileRoot);if(a==INVALID_FILE_ATTRIBUTES||(a&FILE_ATTRIBUTE_REPARSE_POINT))return FALSE;}}
    {DWORD length=GetEnvironmentVariableW(L"COOP_PROBE_SETTINGS",settingsPath,4096);if(length>=4096||selfTest)settingsPath[0]=0;if(length&&length<4096&&!selfTest)loadSettings();}
    wchar_t pauseOption[8]={0};DWORD pauseLength=GetEnvironmentVariableW(L"COOP_PROBE_PAUSE_FIXTURE",pauseOption,8);if(pauseLength){
        if(pauseLength>=8||(wcscmp(pauseOption,L"0")&&wcscmp(pauseOption,L"1")))return FALSE;
        networkPauseFixture=!wcscmp(pauseOption,L"1");if(networkPauseFixture&&(!humanInputFixture||!networkRole))return FALSE;
    }
    wchar_t pathMode[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_PATHSYNC",pathMode,8);networkPathSync=(networkRole||networkDeferred)&&!wcscmp(pathMode,L"1");
    wchar_t ambientMode[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_AMBIENT_SPLIT",ambientMode,8);networkAmbientSplit=(networkRole||networkDeferred)&&!wcscmp(ambientMode,L"1");
    char addressOption[64]={0};DWORD addressLength=GetEnvironmentVariableA("COOP_PROBE_NET_ADDRESS",addressOption,sizeof(addressOption));if(addressLength){if(addressLength>=sizeof(addressOption))return FALSE;strcpy(networkAddress,addressOption);}
    wchar_t portOption[16]={0};if(GetEnvironmentVariableW(L"COOP_PROBE_NET_PORT",portOption,16)){wchar_t* end=0;unsigned long value=wcstoul(portOption,&end,10);if(!end||*end||value<1024||value>65535)return FALSE;networkPort=(unsigned short)value;}
    wchar_t frameOption[16]={0};if(GetEnvironmentVariableW(L"COOP_PROBE_NET_FRAMES",frameOption,16)){wchar_t* end=0;unsigned long value=wcstoul(frameOption,&end,10);if(!end||*end||(value&&value<60)||value>1000000)return FALSE;networkFrameLimit=value;}
    if(networkPauseFixture&&networkFrameLimit&&networkFrameLimit<=430)return FALSE;
    wchar_t campaignDelay[16]={0};DWORD delayLength=GetEnvironmentVariableW(L"COOP_PROBE_CAMPAIGN_DELAY_MS",campaignDelay,16);
    if(delayLength){wchar_t* end=0;if(delayLength>=16)return FALSE;unsigned long value=wcstoul(campaignDelay,&end,10);if(!end||*end||value>2000)return FALSE;campaignDelayMs=value;}
    if(campaignMode&&(selfTest||autoTest||!humanInputMode||!(networkRole||networkDeferred)||!networkContentVerified||inputBot||playBot||actionBot||areaLength||networkPauseFixture||networkMutateFixture||humanDisconnectFixture))return FALSE;
    // Campaign bot: either a finite scripted pair, or (launcher play-mode check) unlimited play
    // where the bot drives only host menus/hub and gameplay input stays physical.
    if(campaignFixture&&(!campaignMode||(humanInputFixture?(networkFrameLimit<600||networkFrameLimit>12000):networkFrameLimit!=0)))return FALSE;
    if(campaignMode&&!campaignFixture&&humanInputFixture)return FALSE;
    if(campaignDelayMs&&(!campaignMode||networkRole!=2))return FALSE;
    wchar_t tutorialOption[8]={};DWORD tutorialLength=GetEnvironmentVariableW(L"COOP_PROBE_TUTORIAL_UI_TEST",tutorialOption,8);
    if(tutorialLength){if(tutorialLength>=8||(wcscmp(tutorialOption,L"0")&&wcscmp(tutorialOption,L"1")))return FALSE;tutorialUiTest=!wcscmp(tutorialOption,L"1");}
    if(tutorialUiTest&&(!campaignMode||!campaignFixture||levelEndTest||!humanInputMode))return FALSE;
    wchar_t soloOption[8]={0};DWORD soloLength=GetEnvironmentVariableW(L"COOP_PROBE_SOLO",soloOption,8);if(soloLength){if(soloLength>=8||wcscmp(soloOption,L"1"))return FALSE;soloMode=true;}
    if(soloMode&&(selfTest||autoTest||campaignMode||networkRole||humanInputMode||inputBot||playBot||actionBot||areaLength||networkTransitionFixture||networkPauseFixture||networkMutateFixture||networkMissionEndFixture||!displayMode))return FALSE;
    probeDiagnostics=!(humanInputMode&&!humanInputFixture&&!networkFrameLimit)&&!soloMode;wchar_t diagnostics[8]={0};DWORD diagnosticsLength=GetEnvironmentVariableW(L"COOP_PROBE_DIAGNOSTICS",diagnostics,8);if(diagnosticsLength){if(diagnosticsLength>=8||(wcscmp(diagnostics,L"0")&&wcscmp(diagnostics,L"1")))return FALSE;probeDiagnostics=!wcscmp(diagnostics,L"1");}
    wchar_t exitOption[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_CLEAN_EXIT",exitOption,8);networkCleanExit=wcscmp(exitOption,L"0")!=0;
    wchar_t seconds[16]={0};GetEnvironmentVariableW(L"COOP_PROBE_GAME_SECONDS",seconds,16);unsigned secondsValue=wcstoul(seconds,0,10);if(secondsValue>=10&&secondsValue<=90)autoGameSeconds=(float)secondsValue;
    DWORD a=GetEnvironmentVariableW(L"COOP_PROBE_ROOT",sandbox,32768),b=GetEnvironmentVariableW(L"COOP_PROBE_GAME_ROOT",gameRoot,32768);if(!a||a>=32768||!b||b>=32768)return FALSE;
    wchar_t normalized[32768];if(!fullpath(sandbox,normalized))return FALSE;lstrcpyW(sandbox,normalized);if(!fullpath(gameRoot,normalized))return FALSE;lstrcpyW(gameRoot,normalized);
    while(wcslen(sandbox)>3&&sandbox[wcslen(sandbox)-1]=='\\')sandbox[wcslen(sandbox)-1]=0;while(wcslen(gameRoot)>3&&gameRoot[wcslen(gameRoot)-1]=='\\')gameRoot[wcslen(gameRoot)-1]=0;
    mkdirs(sandbox);wchar_t path[32768];_snwprintf(path,32768,L"%s\\probe.log",sandbox);logHandle=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);if(logHandle==INVALID_HANDLE_VALUE)return FALSE;
#ifdef FOF_PLAYER_BUILD
    // Player edition (FOF_PLAYER_EDITION): only the retail campaign runs. Test bots, scripted scenarios and
    // fault fixtures cannot be switched on; asking for one refuses the launch instead of running it.
    if(autoTest||inputBot||playBot||actionBot||campaignFixture||humanInputFixture||humanDisconnectFixture||humanKeyboardFixture||
       networkJitterFixture||networkMutateFixture||networkTransitionFixture||networkMissionEndFixture||networkPauseFixture||campaignSidekickTest||menuFixture||swapFixture||levelEndTest||networkNisGateTest||campaignMovieSkipTest||campaignHubJumpTest||campaignHubWalkDpad||campaignHostWaitLink||campaignHostContinue||campaignSlotFixture!=~0u||campaignOverwriteFixtureNo||
       (!campaignMode&&!selfTest))return FALSE;
    log("FOF_PLAYER_EDITION bots and test fixtures disabled");
#endif
    log("probe attach pid=%lu selftest=%u",GetCurrentProcessId(),selfTest?1:0);if(campaignMode)log("CAMPAIGN CONFIG enabled=1 fixture=%u research_autotest=0",campaignFixture?1u:0u);originalErrno=(decltype(originalErrno))GetProcAddress(GetModuleHandleW(L"MSVCR80.dll"),"_errno");if(!install()){log("FAIL installation; launcher must terminate");return FALSE;}if(!AddVectoredExceptionHandler(1,exception)){log("FAIL exception diagnostic registration");return FALSE;}log("READY pre-entry hooks installed; registry entirely virtual; raw input and timing preserved");return TRUE;
}
