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
    v->used=true;lstrcpyW(v->path,path);lstrcpyW(v->name,name);v->type=type;v->size=n;if(n)memcpy(v->data,data,n);log("registry set virtualized name=%ls bytes=%lu",name,n);return ERROR_SUCCESS;
}
static LSTATUS WINAPI regQuery(HKEY k,LPCWSTR name,LPDWORD,LPDWORD type,LPBYTE data,LPDWORD n) {
    wchar_t path[1024];if(!n||!name||!keyPath(k,path))return ERROR_ACCESS_DENIED;
    // Known native settings only. No guessed fullscreen or language key values.
    DWORD forced=0;bool known=false;
    if(!_wcsicmp(name,L"ResolutionX")){forced=displayWidth;known=true;}if(!_wcsicmp(name,L"ResolutionY")){forced=displayHeight;known=true;}
    if(known&&displayMode==3){for(auto&v:values)if(v.used&&!_wcsicmp(path,v.path)&&!_wcsicmp(name,v.name))known=false;if(!displayWidth||!displayHeight)known=false;}
    if(known){if(type)*type=REG_DWORD;if(data&&*n<4){*n=4;return ERROR_MORE_DATA;}if(data)memcpy(data,&forced,4);*n=4;log("registry known setting name=%ls value=%lu",name,forced);return ERROR_SUCCESS;}
    for(auto&v:values)if(v.used&&!_wcsicmp(path,v.path)&&!_wcsicmp(name,v.name)){if(type)*type=v.type;DWORD capacity=*n;*n=v.size;if(data&&capacity<v.size)return ERROR_MORE_DATA;if(data)memcpy(data,v.data,v.size);return ERROR_SUCCESS;}
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
static bool campaignLocalStop=false;
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
static HRESULT WINAPI reset(IDirect3DDevice9* p,D3DPRESENT_PARAMETERS* params){logParams("D3D Reset ORIGINAL",params);windowParams(params);log("D3D Reset forced windowed; native format preserved");return originalReset(p,params);}
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
static volatile LONG campaignCaptureAt=0;
static HRESULT WINAPI present(IDirect3DDevice9*p,const RECT*a,const RECT*b,HWND c,const RGNDATA*d){
    static ULONGLONG started=0;static unsigned lastCapture=0;
    ULONGLONG now=GetTickCount64();if(!started)started=now;unsigned seconds=(unsigned)((now-started)/1000);
    unsigned n=++presentCalls;
    // Research: screenshots shortly after a requested moment (frame numbers 300000+offset).
    if(campaignCaptureAt){LONG offset=(LONG)n-campaignCaptureAt;if(offset==30||offset==90||offset==180||offset==300)capture(p,300000+offset);if(offset>300)campaignCaptureAt=0;}if(n==1||n%6000==0){log("D3D Present count=%u elapsed_s=%u",n,seconds);if(!selfTest)log("read-only state rng=%08lx sim_mode=%lu fixed_sim_fps=%g",*(volatile DWORD*)0x58cf24,*(volatile DWORD*)0x69ed24,*(volatile float*)0x591df8);}
    if(n==120)capture(p,n);if(seconds>=5&&seconds/5>lastCapture){lastCapture=seconds/5;capture(p,200000+seconds);}
    if(playBot||networkRole){
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
    if(!coop::SelectLoadedUSLayout(handles,(std::size_t)count,selected)){
        log("INPUT KEY_LAYOUT actual=%p canonical=unavailable loaded_count=%d; no layout activated or installed",inputActualLayout,count);
        startupInputFailure("standard US keyboard layout is not already loaded");
    }
    HKL canonical=(HKL)(ULONG_PTR)selected;
    struct Translation{UINT scan;wchar_t expected;};
    const Translation translations[]={{0x11,L'w'},{0x1e,L'a'},{0x1f,L's'},{0x20,L'd'},
        {0x15,L'y'},{0x2c,L'z'},{2,L'1'},{0x0c,L'-'},{0x27,L';'},{0x28,L'\''},
        {0x29,L'`'},{0x2b,L'\\'},{0x1a,L'['},{0x1b,L']'},{0x33,L','},{0x34,L'.'},{0x35,L'/'}};
    const BYTE state[256]={};
    for(const auto& test:translations){
        UINT vk=MapVirtualKeyExW(test.scan,3,canonical);wchar_t output[4]={};
        if(!vk||ToUnicodeEx(vk,test.scan,state,output,4,0,canonical)!=1||output[0]!=test.expected)
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
        DWORD mode[3]={};if(captureGameMode(mode)&&mode[0]==1){if(networkRole==1&&networkTick%60<2)actions[10]=1.0f;return actions;}
        unsigned direction=((networkTick/120+networkRole)%2)?18:19;actions[direction]=0.65f;return actions;}
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
    bool ambient=networkLockstep()&&networkAmbientSplit&&state==(DWORD*)0x58cf24&&(outerCaller==0x49115a||outerCaller==0x49233f);
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
    if(probeDiagnostics&&tracked&&InterlockedIncrement(&networkRngCalls)<=12000){DWORD actor=readWord(0x69e9f8),header=readWord(actor+8),serial=header?readWord(header+8):~0u;log("NET RNG call=%ld tick=%u simulation=%u thread=%lu caller=%08lx outer=%08lx bound=%lu old=%08lx new=%08lx result=%lu common=%u actor=%lu state=%p",networkRngCalls,networkTick,networkInSimulation&&GetCurrentThreadId()==networkSimulationThread,GetCurrentThreadId(),caller,outerCaller,limit,old,limit?*state:0,result,state==(DWORD*)0x58cf24,serial,state);}
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
static bool openNetworkConnection(){
    if(networkReady)return true;
    {
        coop::Digest build={},scenario={};const char* digest="ebd30047d7e58c95bf7dc4bd1fb4a5780e16aa472b61c25a8c421632662083db";
        for(unsigned i=0;i<32;i++){unsigned byte=0;sscanf(digest+i*2,"%2x",&byte);build[i]=(BYTE)byte;}
        if(networkContentVerified)build=networkContent;
        // Private scenario tag: Tokyo1/1, seed8466,60Hz,delay3,combat bot2.
        scenario[0]=12;scenario[1]=0x21;scenario[2]=0x12;scenario[3]=60;scenario[4]=3;scenario[5]=networkPathSync?1:0;scenario[6]=networkAmbientSplit?1:0;scenario[7]=humanInputMode?1:0;scenario[8]=humanInputFixture?1:0;
        memcpy(scenario.data()+12,&networkFrameLimit,sizeof(networkFrameLimit));
        unsigned areaHash=0;if(!campaignMode){areaHash=2166136261u;for(const char* c=networkArea;*c;c++)areaHash=(areaHash^(BYTE)*c)*16777619u;}memcpy(scenario.data()+16,&areaHash,sizeof(areaHash));
        scenario[20]=campaignMode?1:0;scenario[21]=campaignFixture?1:0;scenario[22]=campaignJoinPending?1:0;
        scenario[9]=networkTransitionFixture?1:0;
        scenario[10]=(BYTE)networkTransitionTarget;scenario[11]=networkPauseFixture?1:0;
        networkWire=new coop::InputWire;
        log("NET opening role=%u address=%s port=%u frame_limit=%u content_preflight=%u scenario=%s jitter_fixture=%u",networkRole,networkAddress,networkPort,networkFrameLimit,networkContentVerified,campaignMode?"retail-campaign":"research",networkJitterFixture);
        // A person's game meets its partner at the main menu whenever its own startup videos end (or are
        // skipped), so a human campaign waits up to 120s; it narrows to 30s once New Game is chosen.
        networkReady=networkWire->Open(networkRole==1,networkAddress,networkPort,build,scenario,3,campaignMode&&!campaignFixture?120000:30000);
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
static bool campaignPartnerLost(const char* why){
    if(!campaignJoined||networkRole!=1)return false;
    log("NET PARTNER_LOST %s; host continues alone and can Host again",why);
    networkWire->Close();networkReady=networkFailed=networkComplete=false;networkRole=0;networkDeferred=true;networkTick=0;networkInputClock=false;
    campaignJoinPending=campaignJoinSent=campaignJoinReceived=campaignJoined=false;campaignMission=campaignGameplay=false;
    campaignPhaseSequence=0;campaignLoadSequence=0;campaignCycle=0;campaignHubPartnerReset();applyNetworkActions(0,{});applyNetworkActions(1,{});return true;
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
static void networkInput(){
    if(networkFailed||networkComplete){applyNetworkActions(0,{});applyNetworkActions(1,{});if(networkFailed&&gameWindow&&!networkCloseRequested){networkCloseRequested=true;log("NET failure requests normal close of owned game window");PostMessageW(gameWindow,WM_CLOSE,0,0);}return;}
    if(!networkReady&&!openNetworkConnection())return;
    if(networkFrameLimit&&networkTick>=networkFrameLimit&&(!networkMissionEndFixture||(campaignCycleTarget>1&&campaignCycle+1>=campaignCycleTarget))){networkFinishScript();return;}
    static coop::Actions history[32];coop::Actions local=humanInputMode?captureLocalActions():networkBotActions(networkRole==1?0:1,networkTick),remote={};
    history[networkTick%32]=local;
    verifyNativeAxes(local);if(networkFailed)return;
    static std::uint64_t states[32]={};states[networkTick%32]=networkState;std::uint64_t remoteState=0;
    if(networkJitterFixture){if((networkTick+networkRole*7)%43==0)Sleep(45+networkRole*10);if(networkTick==1500&&networkRole==2)Sleep(500);}
    if(!networkWire->Step(networkTick,local,remote,networkState,&remoteState)){
        if(campaignPartnerLost(networkWire->PeerStopped()?"partner closed the game":"connection lost during play"))return;
        if(networkWire->PeerStopped()){networkComplete=true;applyNetworkActions(0,{});applyNetworkActions(1,{});log("NET peer ended session normally frame=%u; no co-op QA awarded",networkTick);
            if(!gameWindow||!PostMessageW(gameWindow,WM_CLOSE,0,0)){networkFailed=true;log("NET FAIL peer stop close error=%lu",GetLastError());}return;}
        networkFailed=true;log("NET FAIL frame=%u %s",networkTick,networkWire->Error().c_str());applyNetworkActions(0,{});applyNetworkActions(1,{});return;}
    if(networkTick>=3&&remoteState!=states[(networkTick-3)%32]){networkFailed=true;log("NET FAIL shared-state divergence observed_tick=%ld own=%016llx peer=%016llx",(LONG)networkTick-4,states[(networkTick-3)%32],remoteState);return;}
    coop::Actions delayed=networkTick<3?coop::Actions{}:history[(networkTick-3)%32];
    applyNetworkActions(networkRole==1?0:1,delayed);applyNetworkActions(networkRole==1?1:0,remote);++networkTick;
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
    if(networkLockstep()&&flow&&readWord(flow+8)==128){if(!networkReady||networkFailed||networkComplete||!networkTick)return;if(readWord(0x69ed24)!=0){networkFailed=true;log("NET FAIL unexpected simulation mode");return;}networkSimulationThread=GetCurrentThreadId();networkInSimulation=true;BYTE previousPathFlag=*(BYTE*)0x590718;if(networkPathSync)*(BYTE*)0x590718=1;transitionFixture(networkTick-1);missionEndFixture(networkTick-1);if(!networkFailed)originalSimulation(task,dt);if(networkPathSync)*(BYTE*)0x590718=previousPathFlag;networkInSimulation=false;if(!networkFailed)observeNetworkState(networkTick-1);}
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
    state.auxiliary[1]=campaignProgress();state.auxiliary[2]=readWord(0x69c624);state.auxiliary[3]=readWord(0x69eb0c);state.auxiliary[4]=0;
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
        if(!n||n>=512)wcscpy_s(campaignNoticeText,L"Waiting for your partner…\nThey may still be watching a cutscene. The game continues as soon as they catch up.");}
    RECT game={0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};if(gameWindow)GetWindowRect(gameWindow,&game);
    int width=520,height=120,x=game.left+(game.right-game.left-width)/2,y=game.top+(game.bottom-game.top)/3-height/2;
    campaignNoticeWindow=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"FofCoopWaitNotice",L"Friend or Foe co-op",WS_POPUP|WS_BORDER,x,y,width,height,gameWindow,0,instance,0);
    if(campaignNoticeWindow){ShowWindow(campaignNoticeWindow,SW_SHOWNOACTIVATE);UpdateWindow(campaignNoticeWindow);log("CAMPAIGN WAIT_NOTICE shown");}
}
static bool campaignWaitPhase(unsigned sequence,const coop::PhaseState& state,coop::PhaseState& peer){
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
static coop::PhaseState campaignExchange(const coop::PhaseState& state){
    if(!networkReady&&!openNetworkConnection())campaignReject("transport open");
    coop::PhaseState peer={};unsigned sequence=campaignPhaseSequence++;
    if(!campaignWaitPhase(sequence,state,peer)){
        // This player closed the game while waiting (the wait keeps handling window messages): a normal end.
        if(campaignLocalStop){networkComplete=true;log("NET local close during a partner wait phase=%u; normal end",sequence);return state;}
        // The caller sees its own state echoed; from the next frame on the host is an ordinary solo game.
        if(campaignPartnerLost(networkWire->PeerStopped()?"partner closed the game":"connection lost in a menu"))return state;
        if(networkWire->PeerStopped()){networkComplete=true;log("NET peer ended session normally phase=%u",sequence);ExitProcess(0);}
        log("NET FAIL CAMPAIGN phase transport=%s",networkWire->Error().c_str());campaignReject("phase transport failure or timeout");
    }
    if(!coop::CampaignPhaseValid(peer))campaignReject("invalid phase metadata");return peer;
}
// Menu/hub sync without blocking: send this side's state once, then poll every frame while the game keeps running
// with neutral input. A partner still watching a cutscene this player skipped (each PC plays cutscenes on its own)
// is waited for without a time limit; a dropped link still ends the wait at once.
static bool campaignMenuPending=false;static coop::PhaseState campaignMenuSent;
static bool campaignMenuExchange(coop::PhaseState& state,coop::PhaseState& peer){
    if(!networkReady&&!openNetworkConnection())campaignReject("transport open");
    unsigned sequence=campaignPhaseSequence;int got=0;
    if(!campaignMenuPending){
        if(!networkWire->SendPhase(sequence,state))got=-1;else{campaignMenuPending=true;campaignMenuSent=state;}
    }
    if(got==0)got=networkWire->TryReceivePhase(sequence,peer);
    if(got==0){static unsigned waits=0;if(waits++%600==0)log("CAMPAIGN MENU_WAIT sequence=%u kind=%lu (partner not there yet)",sequence,campaignMenuSent.id);return false;}
    if(got<0){
        campaignMenuPending=false;
        if(campaignPartnerLost(networkWire->PeerStopped()?"partner closed the game":"connection lost in a menu"))return false;
        if(networkWire->PeerStopped()){networkComplete=true;log("NET peer ended session normally phase=%u",sequence);ExitProcess(0);}
        log("NET FAIL CAMPAIGN phase transport=%s",networkWire->Error().c_str());campaignReject("phase transport failure or timeout");
    }
    campaignMenuPending=false;campaignPhaseSequence++;state=campaignMenuSent;
    if(!coop::CampaignPhaseValid(peer))campaignReject("invalid phase metadata");return true;
}
static void campaignExactBarrier(coop::PhaseState state){
    auto peer=campaignExchange(state);if(!coop::CampaignPhaseEqual(state,peer))campaignReject("phase identity/selection/progress mismatch");
    state.flags=coop::phaseReady|coop::phaseConfirmed;peer=campaignExchange(state);
    if(!coop::CampaignPhaseEqual(state,peer))campaignReject("phase confirmation mismatch");
}
// Hub walking is read by the character controller outside the hub scene update,
// so the last mutually exchanged host stick bits are re-applied after each clear.
static unsigned campaignHubMovement=0,campaignHubMovement2=0,campaignHubPairedCalls=0;static bool campaignHubPlayer2=false;
static bool campaignHubP2Position(float* xyz);
// P1 walks with the host's exchanged stick bits; with the hub sidekick (research) P2 walks with the joiner's.
static void campaignApplyHubMovement(){coop::Actions shared={},second={};for(unsigned i=16;i<20;i++){if(campaignHubMovement&(1u<<i))shared[i]=1;if(campaignHubPlayer2&&(campaignHubMovement2&(1u<<i)))second[i]=1;}applyNetworkActions(0,shared);applyNetworkActions(1,second);}
static void campaignNeutralMenu(){
    // Clear native consumers without changing replay's private held-key history.
    for(unsigned slot=0;slot<2;slot++){
        memset((void*)(0x691170+slot*0x11c+4),0,28*4);memset((void*)(0x691170+slot*0x11c+0xe4),0,8*4);
        DWORD wrapper=readWord(0x595390+slot*4),mapped=readWord(wrapper+0x3c);if(wrapper&&mapped&&readWord(wrapper)>=28)memset((void*)mapped,0,sizeof(float)*28);
    }
}
static unsigned campaignMenuBits(const coop::Actions& actions){unsigned bits=0;for(unsigned i=0;i<28;i++)if(actions[i]>0.5f)bits|=1u<<i;return bits;}
static DWORD campaignMenuContext=0;static unsigned campaignMenuKind=0,campaignMenuCalls[8]={};
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
static unsigned campaignHubWalkBits(unsigned focus,unsigned calls){
    static unsigned phase=0,phaseStart=0,tried=0,targetStart=0,arrived=0,bestAt=0,detourUntil=0,detours=0;static int target=-1;static float start[3],axisX[2],axisY[2],best=1e9f;
    if(campaignHubWalkReset){campaignHubWalkReset=false;tried=0;target=-1;arrived=0;}
    float p[3]={};if(!campaignHubPlayer(p))return 0;
    if(focus==campaignMissionGraphHash())return calls%120==60?1u<<10:0;
    if(phase==0){memcpy(start,p,sizeof(start));phase=1;phaseStart=calls;}
    if(phase==1){if(calls-phaseStart<30)return 1u<<19;axisX[0]=p[0]-start[0];axisX[1]=p[1]-start[1];memcpy(start,p,sizeof(start));phase=2;phaseStart=calls;}
    if(phase==2){if(calls-phaseStart<30)return 1u<<16;axisY[0]=p[0]-start[0];axisY[1]=p[1]-start[1];phase=3;
        log("CAMPAIGN HUB_WALK calibrated right=%.4g,%.4g up=%.4g,%.4g",axisX[0],axisX[1],axisY[0],axisY[1]);
        if(std::hypot(axisX[0],axisX[1])<0.05f||std::hypot(axisY[0],axisY[1])<0.05f){log("CAMPAIGN HUB_WALK stick did not move P1");phase=9;}}
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
    if(focus){char name[33];campaignTriggerName(trigger,name);log("CAMPAIGN HUB_WALK focus=%08x at target=%s is not the mission terminal",focus,name);tried|=1u<<target;return 0;}
    float d[2]={readFloat(trigger+0x50)-p[0],readFloat(trigger+0x54)-p[1]},distance=std::hypot(d[0],d[1]);
    if(calls-targetStart>1800){log("CAMPAIGN HUB_WALK timeout target=%d distance=%.4g detours=%u",target,distance,detours);tried|=1u<<target;return 0;}
    float feet[3]={p[0],p[1],p[2]+readFloat(0x55b520)};
    if(triggerContains(trigger,feet)||distance<0.3f){if(!arrived){arrived=calls;log("CAMPAIGN HUB_WALK inside target=%d distance=%.4g",target,distance);}if(calls-arrived>120){log("CAMPAIGN HUB_WALK no focus at target=%d",target);tried|=1u<<target;}return 0;}
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
static void __cdecl campaignMouseUpdate(){
    ((void(__cdecl*)())0x528bc0)();if(!campaignLive())return;
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
// Host, in the hub: send the live progress block; the joiner answers each part from its main menu.
static void campaignHostSendJoin(){
    BYTE progress[0x1fc]={};if(!readMemory((void*)0x69c470,progress,sizeof(progress)))campaignReject("host progress read");
    for(unsigned part=0;part<8;part++){auto state=campaignJoinState(0x30000+part);campaignJoinPayload(state,part,progress);auto peer=campaignExchange(state);if(peer.id!=state.id)campaignReject("join progress part mismatch");}
    auto done=campaignJoinState(0x30008);done.auxiliary[1]=campaignBytesHash(progress,sizeof(progress));done.auxiliary[2]=campaignProgress();auto peer=campaignExchange(done);
    if(peer.id!=done.id||peer.auxiliary[1]!=done.auxiliary[1])campaignReject("join progress transfer checksum");
    campaignJoinSent=true;campaignSlot=readWord(0x6a78cc);campaignNewGame=campaignProfile=campaignHub=true;campaignMission=campaignGameplay=false;campaignJoined=true;campaignHubWalkReset=true;
    log("CAMPAIGN JOIN_SENT progress=%08x bytes=508",done.auxiliary[2]);
}
// Joiner, at its main menu: receive the host's progress once the host has started sending it.
static bool campaignJoinerReceive(){
    if(campaignJoinReceived)return true;if(!networkWire->PhaseAvailable(campaignPhaseSequence))return false;
    BYTE progress[0x1fc]={};
    for(unsigned part=0;part<8;part++){auto peer=campaignExchange(campaignJoinState(0x30000+part));if(peer.id!=0x30000+part)campaignReject("join progress part order");campaignJoinUnpack(peer,part,progress);}
    auto done=campaignJoinState(0x30008);done.auxiliary[1]=campaignBytesHash(progress,sizeof(progress));auto peer=campaignExchange(done);
    if(peer.id!=done.id||peer.auxiliary[1]!=done.auxiliary[1])campaignReject("join progress transfer checksum");
    memcpy(campaignJoinProgress,progress,sizeof(progress));campaignJoinSelected=peer.auxiliary[2];campaignJoinReceived=true;campaignJoined=true;
    log("CAMPAIGN JOIN_RECEIVED progress=%08x; starting New Game locally",campaignJoinSelected);return true;
}
// Both in the hub with equal progress: from here on every choice is exchanged again.
static void campaignJoinReady(){
    auto state=campaignJoinState(0x30100);state.auxiliary[1]=campaignProgress();state.auxiliary[2]=readWord(0x69c624);auto peer=campaignExchange(state);
    if(peer.id!=state.id||peer.auxiliary[1]!=state.auxiliary[1]||peer.auxiliary[2]!=state.auxiliary[2])campaignReject("joined hub progress mismatch");
    campaignJoinPending=false;networkWire->SetTimeout(30000);log("CAMPAIGN ASSERT JOIN_COMPLETE progress=%08x role=%u",state.auxiliary[1],networkRole);
}
static bool campaignSaveSlotUsed(unsigned row){
    wchar_t path[32768];_snwprintf(path,32768,L"%s\\Documents\\Activision\\Spider-Man FoF\\Saved Games\\SavedGame%u.SFOFSaveGame",profileRoot[0]?profileRoot:sandbox,row+1);
    DWORD attributes=GetFileAttributesW(path);return attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_DIRECTORY);
}
static unsigned campaignFirstFreeSlot(){for(unsigned row=0;row<3;row++)if(!campaignSaveSlotUsed(row))return row;return 0;}
static bool campaignMenuBegin(void* object,unsigned kind,DWORD listOffset){
    // A menu over the hub (mission list, sidekick screen, ...) must not see the walking sticks: the hub keeps applying
    // them every frame and the joiner's P2 walk moved the mission list cursor differently per seat (mismatch-367:
    // row 0 vs 2). Stop both walks while any other menu is up.
    if(kind!=3){campaignHubMovement=0;campaignHubMovement2=0;}
    // A finished session closes its window next; no further phase exchanges.
    if(networkComplete){campaignNeutralMenu();return false;}
    if(campaignJoinPending){
        if(networkRole==2&&kind==1&&!campaignJoinReceived&&!campaignJoinerReceive()){campaignNeutralMenu();return true;}
        if(kind==3){
            // Host waits frozen in the hub; the joiner arrives there after its local New Game.
            if(networkRole==1&&!networkWire->PhaseAvailable(campaignPhaseSequence)){campaignNeutralMenu();return true;}
            if(networkRole==2&&!campaignHub){campaignNeutralMenu();return true;}
            campaignJoinReady();
        }
    }
    DWORD scene=(DWORD)(ULONG_PTR)object;BYTE active=0;if(!readMemory((void*)(scene+0x7a),&active,1)||!active)return false;
    unsigned selection=0xffffffffu,available=1;DWORD list=0;
    if(listOffset){list=readWord(scene+listOffset);selection=list?readWord(list+0x8d08):0xffffffffu;if(kind==4&&selection<4){BYTE enabled=0;readMemory((void*)(list+selection*0x234+0xb5),&enabled,1);available=enabled?1:0;}
        if(kind==7)available=campaignSidekickRowAvailable(selection)?1:0;}
    else selection=readWord(scene+0x84);
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
            if(calls>=60&&calls%120==60&&(kind==1||kind==2||kind==4||kind==5||kind==6))bits=1u<<10;
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
            if(kind==3&&calls>=60)bits=campaignHubWalkBits(selection,calls);
        }else{
            bits=campaignMenuBits(campaignPhysicalMenu);
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
        }
        if(kind==2||kind==5||kind==6)bits&=1u<<10;
        if(kind==7)bits=0;
    }
    // The joiner drives the Sidekick Select screen (its own character): bot steps left to the first other unlocked
    // sidekick and accepts it; a person's input passes, except Accept on a locked row.
    if(kind==7&&networkRole==2){
        if(campaignFixture){static unsigned want=0xffffffffu;if(want==0xffffffffu){for(unsigned r=0;r<13;r++)if(r!=selection&&campaignSidekickRowAvailable(r)){want=r;break;}log("CAMPAIGN SIDEKICK_SELECT bot target row=%u from=%u",want,selection);}
            bits=0;if(calls>=60&&calls%40==20)bits=want>=13?1u<<11:selection==want?1u<<10:selection>want?1u<<2:1u<<3;}
        else{bits=campaignMenuBits(campaignPhysicalMenu);if(!available)bits&=~(1u<<10);}
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
        static unsigned localLogs=0;if(localLogs<200&&(local||calls%60==0)){++localLogs;log("CAMPAIGN LOCAL_DRIVE kind=%u selection=%ld calls=%u press=%08x",kind,(LONG)selection,calls,local);}
        coop::Actions shared={};for(unsigned i=0;i<28;i++)if(local&(1u<<i))shared[i]=1;applyNetworkActions(0,shared);applyNetworkActions(1,{});campaignMenuContext=scene;campaignMenuKind=kind;return true;
    }
    state.auxiliary[0]=bits;coop::PhaseState peer={};
    // Hub with a second character: the joiner's stick bits travel in aux4 (P2 movement); the bot joiner walks a little.
    if(kind==3&&campaignHubPlayer2&&networkRole==2){unsigned mine=campaignFixture?((calls/90)%4==1?1u<<16:(calls/90)%4==3?1u<<17:0u):campaignMenuBits(campaignPhysicalMenu)&0xf0000u;state.auxiliary[4]=mine;}
    peer=campaignExchange(state);
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
        if(kind==3&&selection!=campaignMissionGraphHash()&&selection!=campaignSidekickTerminal)campaignReject("hub Accept outside the mission and sidekick terminals");
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
        state.auxiliary[0]=hostBits;state.flags=coop::phaseReady|coop::phaseConfirmed;peer=campaignExchange(state);
        if(!coop::CampaignPhaseEqual(state,peer))campaignReject("host menu choice was not mutually confirmed");
    }
    if(kind==3){campaignHubMovement=hostBits&0xf0000u;++campaignHubPairedCalls;if(campaignHubPlayer2)campaignHubMovement2=(networkRole==1?peer.auxiliary[4]:state.auxiliary[4])&0xf0000u;}
    coop::Actions shared={},second={};for(unsigned i=0;i<28;i++)if(hostBits&(1u<<i))shared[i]=1;
    if(kind==3&&campaignHubPlayer2)for(unsigned i=16;i<20;i++)if(campaignHubMovement2&(1u<<i))second[i]=1;
    applyNetworkActions(0,shared);applyNetworkActions(1,second);campaignMenuContext=scene;campaignMenuKind=kind;return true;
}
static bool __cdecl campaignMainAccept(DWORD* winner){
    bool accepted=((bool(__cdecl*)(DWORD*))0x497e78)(winner);
    if(campaignLive()&&accepted){if(campaignMenuKind!=1||!campaignMenuContext||readWord(readWord(campaignMenuContext+0x84)+0x8d08)!=0)campaignReject("first retail slice supports native New Game only");campaignMainAcceptObserved=true;}
    return accepted;
}
static void __fastcall campaignMainUpdate(void* scene,void*,float dt){
    if(!campaignLive()){originalCampaignMain(scene,dt);return;}bool active=campaignMenuBegin(scene,1,0x84);originalCampaignMain(scene,dt);
    if(active&&campaignMainAcceptObserved&&!campaignNewGame){if(readWord(0x69eb0c)!=0||readWord(0x58731c)!=0)campaignReject("native New Game result mismatch");campaignNewGame=true;if(networkWire&&networkReady)networkWire->SetTimeout(30000);log("CAMPAIGN ASSERT NEW_GAME operation=0 selection=0");}campaignNeutralMenu();campaignMenuContext=0;
}
// A game played alone (opened, not yet linked) keeps the player's own menu input.
static void __fastcall campaignProfileUpdate(void* scene,void*,float dt){if(campaignLive())campaignMenuBegin(scene,2,0x88);originalCampaignProfile(scene,dt);if(campaignLive()){campaignNeutralMenu();campaignMenuContext=0;}}
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
    campaignHubPartnerUpdate();
    BYTE active=0;readMemory((BYTE*)scene+0x7a,&active,1);
    if(campaignMode&&campaignJoinPending&&networkRole==1&&!campaignJoinSent&&active&&readWord(readWord(0x69e72c)+8)==16){char path[33]={};DWORD id=0;if(campaignArea(readWord(readWord(0x6a9974)+0x18),path,id)&&id==97)campaignHostSendJoin();}
    if(campaignLive()&&active&&campaignGameplay){char path[33]={};DWORD id=0;if(campaignArea(readWord(readWord(0x6a9974)+0x18),path,id)&&id==97&&readWord(readWord(0x69e72c)+8)==16){
        log("CAMPAIGN ASSERT RETURN_HUB path=%s area=97 flow=16 tick=%u mission_end=%u",path,networkTick,networkMissionEndInvoked?1u:0u);
        if(networkMissionEndFixture&&!networkMissionEndInvoked)campaignReject("hub returned without the mission-end fixture");
        // Both seats must agree on the post-mission progress bytes before continuing.
        auto state=campaignState(coop::phaseLoad,0x20000+campaignCycle,id);state.identity={};memcpy(state.identity.data(),path,strlen(path));campaignExactBarrier(state);
        log("CAMPAIGN ASSERT RETURN_HUB_SYNC progress=%08x tick=%u",state.auxiliary[1],networkTick);
        if(networkMissionEndFixture&&campaignCycle+1>=campaignCycleTarget){networkFinishScript();}
        else{campaignGameplay=false;campaignMission=false;networkMissionEndInvoked=false;campaignHubWalkReset=true;++campaignCycle;log("CAMPAIGN CYCLE_READY cycle=%u",campaignCycle);}}}
    if(campaignLive()&&active&&campaignProfile&&!campaignHub){char path[33]={};DWORD id=0;if(campaignArea(readWord(readWord(0x6a9974)+0x18),path,id)&&id==97&&!strcmp(path,"FrontEndHub/FrontEnd/FrontEndHub")&&readWord(readWord(0x69e72c)+8)==16){campaignHub=true;log("CAMPAIGN ASSERT HUB path=FrontEndHub/FrontEnd/FrontEndHub area=97 flow=16");}}
    if(campaignLive()&&campaignHub){static unsigned hubCalls=0;if(hubCalls++%120==0){DWORD hub=(DWORD)(ULONG_PTR)scene;BYTE hubActive=0;readMemory((void*)(hub+0x7a),&hubActive,1);log("CAMPAIGN HUB_OBSERVE calls=%u active=%u focus=%08lx mission_hash=%08lx menu_calls=%u",hubCalls,hubActive,readWord(hub+0x84),campaignMissionGraphHash(),campaignMenuCalls[3]);
        DWORD party=readWord(0x6a9900);for(unsigned player=0;player<2;player++){DWORD actor=readWord(party+0x12c+player*12),transform=actor?readWord(actor+0x1d0):0;float xyz[3]={};if(transform&&readWord(readWord(actor)+0x2f4)==0x45a6c9&&readMemory((void*)transform,xyz,12))log("CAMPAIGN HUB_PLAYER index=%u xyz=%.6g,%.6g,%.6g",player,xyz[0],xyz[1],xyz[2]);}
        static bool triggersLogged=false;if(!triggersLogged){triggersLogged=true;DWORD pool=readWord(0x6a9970),count=readWord(pool+8),items=readWord(pool+4);log("CAMPAIGN HUB_TRIGGERS count=%lu",count);
            for(unsigned i=0;i<count&&i<256;i++){DWORD trigger=readWord(items+i*4);if(!trigger)continue;char triggerName[33];if(!campaignTriggerName(trigger,triggerName))strcpy(triggerName,"-");log("CAMPAIGN HUB_TRIGGER index=%u name=%s table=%08lx kind=%lu flags=%08lx enabled=%lu enter=%08lx center=%.6g,%.6g,%.6g extents=%.6g,%.6g,%.6g",i,triggerName,readWord(trigger),readWord(trigger+0x80),readWord(trigger+0x74),readWord(trigger+0x78),readWord(readWord(trigger)+0x24),readFloat(trigger+0x50),readFloat(trigger+0x54),readFloat(trigger+0x58),readFloat(trigger+0x60),readFloat(trigger+0x64),readFloat(trigger+0x68));}}}}
    if(campaignLive())campaignMenuBegin(scene,3,0);originalCampaignHub(scene,dt);if(campaignLive()){campaignNeutralMenu();campaignMenuContext=0;if(campaignHub&&!campaignMission)campaignApplyHubMovement();}
}
static CampaignUpdate originalCampaignSidekick=0;
static void __fastcall campaignSidekickUpdate(void* scene,void*,float dt){if(campaignLive())campaignMenuBegin(scene,7,0x88);originalCampaignSidekick(scene,dt);if(campaignLive()){campaignNeutralMenu();campaignMenuContext=0;}}
static void __fastcall campaignMissionUpdate(void* scene,void*,float dt){if(campaignLive())campaignMenuBegin(scene,4,0x84);originalCampaignMission(scene,dt);if(campaignLive()){campaignNeutralMenu();campaignMenuContext=0;}}
static void __fastcall campaignSaveAckUpdate(void* scene,void*,float dt){
    // Other native window40 uses (including the earlier profile preparation)
    // retain their own update. Only the observed post-init task9 acknowledgement
    // can receive a shared Accept. Keep updates running while either is unready.
    if(campaignLive()&&campaignProfile)campaignMenuBegin(scene,5,0x8c);
    originalCampaignSaveAck(scene,dt);if(campaignLive()){campaignNeutralMenu();campaignMenuContext=0;}
}
static bool __cdecl campaignAutosaveAccept(DWORD* winner){
    bool accepted=((bool(__cdecl*)(DWORD*))0x497e78)(winner);
    if(campaignLive()&&accepted){if(campaignMenuKind!=6||!campaignMenuContext||!coop::CampaignAutosaveNoticeReady(campaignAutosaveNoticeFacts(campaignMenuContext,readWord(readWord(campaignMenuContext+0x8c)+0x8d08))))campaignReject("unattested native generic notice Accept");campaignAutosaveAccepted=true;}
    return accepted;
}
static void __fastcall campaignAutosaveNoticeUpdate(void* scene,void*,float dt){
    if(campaignLive()&&campaignAutosaveArmed)campaignMenuBegin(scene,6,0x8c);
    originalCampaignAutosaveNotice(scene,dt);
    if(campaignLive()){campaignNeutralMenu();campaignMenuContext=0;if(campaignAutosaveArmed&&campaignAutosaveAccepted){campaignAutosaveArmed=false;log("CAMPAIGN AUTOSAVE_NOTICE accepted=1 option=0");}}
}
static void __cdecl campaignProfileBefore(DWORD scene){
    if(!campaignLive())return;if(!campaignNewGame||readWord(scene)!=0x56efe8||readWord(readWord(scene+0x88)+0x8d08)>2||readWord(0x69eb0c)!=0)campaignReject("native New Game slot initialization precondition");campaignSlot=readWord(readWord(scene+0x88)+0x8d08);
    // A game driving its own menus (playing alone, or catching up to join) confirms nothing with its partner.
    if(!campaignLocalDrive())campaignExactBarrier(campaignState(coop::phaseMenu,10,0));
}
static void __cdecl campaignProfileAfter(){
    if(!campaignLive())return;BYTE valid=0;readMemory((void*)0x6a78d0,&valid,1);if(campaignProfile||readWord(0x6a78cc)!=campaignSlot||!valid||readWord(0x69eb0c)!=0){log("CAMPAIGN PROFILE_DETAIL already=%u slot=%lu expected=%lu valid=%u operation=%lu",campaignProfile?1u:0u,readWord(0x6a78cc),campaignSlot,valid,readWord(0x69eb0c));campaignReject("native slot initializer result");}
    campaignProfile=true;log("CAMPAIGN ASSERT PROFILE slot=%lu native_init=1 operation=0",campaignSlot);
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
    }
}
using CampaignDeactivate=void(__thiscall*)(void*);static CampaignDeactivate originalCampaignDeactivate=0;
static void __fastcall campaignMissionDeactivate(void* scene,void*){
    BYTE committing=1;readMemory((BYTE*)scene+0x88,&committing,1);originalCampaignDeactivate(scene);
    if(campaignLive()&&!committing){char path[33]={};DWORD id=0;if(campaignMission||!campaignHub||!campaignArea(readWord(readWord(0x6a9974)+0x1c),path,id)||id==97||readWord(readWord(0x69e72c)+0xc)!=32)campaignReject("native mission selector commit result");
        campaignMission=true;campaignHubPartnerReset();strcpy(campaignMissionPath,path);campaignMissionArea=id;log("CAMPAIGN ASSERT MISSION_COMMIT path=%s area=%lu pending=32",path,id);}
}
static void campaignBeforeLoad(){
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
    auto state=campaignState(coop::phaseLoad,++campaignLoadSequence,id);state.identity={};memcpy(state.identity.data(),path,strlen(path));state.auxiliary[5]=id;state.auxiliary[7]=readWord(readWord(readWord(0x6a9974)+0x1c)+0x1c);
    if(!networkReady&&!openNetworkConnection())campaignReject("retail transport open before native load");
    if(campaignLoadSequence==1&&networkRole==2&&campaignDelayMs){log("CAMPAIGN PRELOAD_DELAY delay_ms=%u",campaignDelayMs);ULONGLONG begin=GetTickCount64();while(GetTickCount64()-begin<campaignDelayMs){if(networkWire->PeerStopped()){log("NET peer ended session normally before delayed native load");ExitProcess(0);}Sleep(5);}}
    log("CAMPAIGN PRELOAD_WAIT sequence=%u path=%s area=%lu original_calls=0",campaignLoadSequence,path,id);campaignExactBarrier(state);
    if(!campaignPrepared){if(!prepareNetworkPlayers()||!prepareTaskTiming())campaignReject("native controller/task preparation");campaignPrepared=true;}
    if(!networkInputClock&&campaignMission&&id==campaignMissionArea&&!strcmp(path,campaignMissionPath)){
        if(!campaignMission||networkTick||readWord(readWord(0x69e72c)+8)!=64)campaignReject("first retail gameplay clock boundary");
        // Match the proven research ordering: new actors/NIS timers must be
        // constructed under the common clock, after retail frontend unload.
        // Hub/media stay on raw time. Later loads keep the advancing clock.
        networkInputClock=true;for(unsigned logical=0;logical<4;logical++)*(DWORD*)(0x691170+logical*0x11c+0x110)=10000;*(DWORD*)0x6a98dc=10000;
        log("CAMPAIGN CLOCK origin_ms=10000 before_native_load=1");
    }
    *(DWORD*)0x58cf24=8466;networkAmbientRng=0x84f19a21;
    log("CAMPAIGN PRELOAD_FORWARD sequence=%u path=%s area=%lu peer_ready=1 original_calls=1",campaignLoadSequence,path,id);
}
static bool campaignStartGameplay(){
    if(campaignGameplay)return true;if(!campaignMission||!campaignPrepared)campaignReject("gameplay reached without native campaign mission commit");
    char path[33]={};DWORD id=0;if(!campaignArea(readWord(readWord(0x6a9974)+0x18),path,id)||id!=campaignMissionArea||strcmp(path,campaignMissionPath))campaignReject("committed mission gameplay identity");
    DWORD manager=readWord(0x6a9900);bool partyReady=manager!=0;DWORD actor[2]={},controller[2]={},slot[2]={~0u,~0u};
    for(unsigned p=0;p<2;p++){actor[p]=readWord(manager+0x12c+p*12);controller[p]=readWord(actor[p]+0x2dc);if(controller[p])slot[p]=readWord(controller[p]+0xc0);if(!actor[p]||!controller[p]||slot[p]!=p)partyReady=false;}
    if(!partyReady){static bool waitingLogged=false;if(!waitingLogged){waitingLogged=true;log("CAMPAIGN WAIT_PARTY manager=%08lx actors=%08lx,%08lx controllers=%08lx,%08lx slots=%lu,%lu",manager,actor[0],actor[1],controller[0],controller[1],slot[0],slot[1]);}return false;}
    auto state=campaignState(coop::phaseLoad,0x10000+campaignLoadSequence,id);state.identity={};memcpy(state.identity.data(),path,strlen(path));campaignExactBarrier(state);
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
static void campaignBotSkipNis(){
    if(!campaignSkipNis||!campaignFixture||readWord(0x69ed24)!=5)return;
    BYTE allowed=0;readMemory((void*)0x69ed66,&allowed,1);DWORD player=readWord(0x6a9984);
    static unsigned skips=0,blocked=0;if(!allowed||!player){if(blocked++%600==0)log("BOT NIS_SKIP not allowed now allowed=%u player=%08lx",allowed,player);return;}
    *(BYTE*)0x69ed66=0;__asm{mov eax,player
        mov ecx,0x4e134b
        call ecx}
    log("BOT NIS_SKIP count=%u",++skips);
}
static void __cdecl inputUpdate(float dt){
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
    ((void(__cdecl*)(float))0x401b6f)(dt);
    if(bootAction)*(DWORD*)forcedAddress=savedForced;
    static ULONGLONG last=0;DWORD flow=readWord(0x69e72c);ULONGLONG now=GetTickCount64();
    if(campaignMode&&networkDeferred)campaignTryConnect();
    if(campaignLive()&&flow&&readWord(flow+8)!=128){
        campaignPhysicalMenu=captureLocalActions();if(readWord(flow+8)>=8){campaignNeutralMenu();if(campaignHub&&!campaignMission)campaignApplyHubMovement();}
        // Test bots (COOP_PROBE_SKIP_NIS): the movie screen (49FED0) skips on a just-pressed abstract 14 (Start) while
        // its player 6A9980 is playing (+4 == 1); press it on slot 0 after the menu clear, once a second.
        if(campaignSkipNis&&campaignFixture){DWORD movie=readWord(0x6a9980);static unsigned moviePulse=0,movieSkips=0;
            if(movie&&readWord(movie+4)==1&&++moviePulse%60==0){DWORD raw=readWord(readWord(0x69e56c)+14*4);if(raw<28){*(DWORD*)(0x691170+4+raw*4)=3;log("BOT MOVIE_SKIP press=%u raw=%lu",++movieSkips,raw);}}}
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
    lastFlowNative=id;if(campaignMode&&!networkDeferred&&id==23)campaignBeforeLoad();originalFlowNative(vm,id);
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
static bool patchKeyboardLayoutCall(){
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
static bool installCampaign(){
    struct Entry{DWORD table,callback;void* hook;CampaignUpdate* original;};
    Entry entries[]={{0x56c0d0,0x49a4b8,(void*)campaignMainUpdate,&originalCampaignMain},
        {0x56efe8,0x4a502d,(void*)campaignProfileUpdate,&originalCampaignProfile},
        {0x56ca40,0x49be94,(void*)campaignHubUpdate,&originalCampaignHub},
        {0x56d120,0x49eda6,(void*)campaignMissionUpdate,&originalCampaignMission},
        {0x56f090,0x4a5e8a,(void*)campaignSaveAckUpdate,&originalCampaignSaveAck},
        {0x56c1c0,0x49a6db,(void*)campaignAutosaveNoticeUpdate,&originalCampaignAutosaveNotice},
        {0x56f298,0x4a6c6f,(void*)campaignSidekickUpdate,&originalCampaignSidekick}};
    if(readWord(0x57e6d8+0x18)!=0x51a6cc||readWord(0x57e6d8+0x1c)!=0x4337ec){log("FAIL CAMPAIGN native save task callback guard");return false;}
    for(const auto& entry:entries){void** table=(void**)entry.table;if(table[19]!=(void*)entry.callback||!patchComSlot(table,19,entry.hook,(void**)entry.original)){log("FAIL CAMPAIGN native update vtable guard table=%08lx",entry.table);return false;}}
    void** mission=(void**)0x56d120;if(mission[14]!=(void*)0x49f107||!patchComSlot(mission,14,(void*)campaignMissionDeactivate,(void**)&originalCampaignDeactivate))return false;
    return patchVerifiedCall(0x49a4e8,0x497e78,(void*)campaignMainAccept)&&
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
    if(!selfTest&&(networkRole||networkDeferred)){void** slot=(void**)0x577e20;if(slot[0]!=(void*)0x4dbc39||!patchComSlot(slot,0,(void*)simulation,(void**)&originalSimulation)){log("FAIL simulation callback guard");return false;}}
    if(!selfTest&&(networkRole||networkDeferred))for(DWORD address:rngCallSites)if(!patchVerifiedCall(address,0x4318f5,(void*)randomEntry))return false;
    if(!selfTest&&humanKeyboardFixture&&!patchVerifiedCall(0x525c00,0x526240,(void*)keyboardPollFixture))return false;
    if(!selfTest&&humanInputMode)for(DWORD address:deviceCallSites)if(!patchVerifiedCall(address,0x40182d,(void*)reassignLocalDevice))return false;
    if(!selfTest&&humanInputMode&&(!patchKeyboardLayoutCall()||!patchVerifiedCall(0x52a770,0x525c30,(void*)guardedKeyLookup)))return false;
    if(campaignMode&&!installCampaign())return false;
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
    {wchar_t sound[4]={0};if(GetEnvironmentVariableW(L"COOP_PROBE_HUB_PARTNER_SOUND",sound,4)&&!wcscmp(sound,L"0"))campaignHubPartnerSound=false;}
    {wchar_t skip[4]={0};campaignSkipNis=GetEnvironmentVariableW(L"COOP_PROBE_SKIP_NIS",skip,4)&&!wcscmp(skip,L"1");}
    {wchar_t spawn[8]={0};if(GetEnvironmentVariableW(L"COOP_PROBE_HUB_SPAWN",spawn,8)){wchar_t* end=0;long v=wcstol(spawn,&end,10);if(!end||*end||v<0||(v>31&&v!=99))return FALSE;campaignHubSpawnIndex=(int)v;}}
    {wchar_t profile[32768]={0};DWORD length=GetEnvironmentVariableW(L"COOP_PROBE_PROFILE",profile,32768);
        if(length){if(length>=4096||!fullpath(profile,profileRoot))return FALSE;mkdirs(profileRoot);DWORD a=GetFileAttributesW(profileRoot);if(a==INVALID_FILE_ATTRIBUTES||(a&FILE_ATTRIBUTE_REPARSE_POINT))return FALSE;}}
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
    wchar_t soloOption[8]={0};DWORD soloLength=GetEnvironmentVariableW(L"COOP_PROBE_SOLO",soloOption,8);if(soloLength){if(soloLength>=8||wcscmp(soloOption,L"1"))return FALSE;soloMode=true;}
    if(soloMode&&(selfTest||autoTest||campaignMode||networkRole||humanInputMode||inputBot||playBot||actionBot||areaLength||networkTransitionFixture||networkPauseFixture||networkMutateFixture||networkMissionEndFixture||!displayMode))return FALSE;
    probeDiagnostics=!(humanInputMode&&!humanInputFixture&&!networkFrameLimit)&&!soloMode;wchar_t diagnostics[8]={0};DWORD diagnosticsLength=GetEnvironmentVariableW(L"COOP_PROBE_DIAGNOSTICS",diagnostics,8);if(diagnosticsLength){if(diagnosticsLength>=8||(wcscmp(diagnostics,L"0")&&wcscmp(diagnostics,L"1")))return FALSE;probeDiagnostics=!wcscmp(diagnostics,L"1");}
    wchar_t exitOption[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_CLEAN_EXIT",exitOption,8);networkCleanExit=wcscmp(exitOption,L"0")!=0;
    wchar_t seconds[16]={0};GetEnvironmentVariableW(L"COOP_PROBE_GAME_SECONDS",seconds,16);unsigned secondsValue=wcstoul(seconds,0,10);if(secondsValue>=10&&secondsValue<=90)autoGameSeconds=(float)secondsValue;
    DWORD a=GetEnvironmentVariableW(L"COOP_PROBE_ROOT",sandbox,32768),b=GetEnvironmentVariableW(L"COOP_PROBE_GAME_ROOT",gameRoot,32768);if(!a||a>=32768||!b||b>=32768)return FALSE;
    wchar_t normalized[32768];if(!fullpath(sandbox,normalized))return FALSE;lstrcpyW(sandbox,normalized);if(!fullpath(gameRoot,normalized))return FALSE;lstrcpyW(gameRoot,normalized);
    while(wcslen(sandbox)>3&&sandbox[wcslen(sandbox)-1]=='\\')sandbox[wcslen(sandbox)-1]=0;while(wcslen(gameRoot)>3&&gameRoot[wcslen(gameRoot)-1]=='\\')gameRoot[wcslen(gameRoot)-1]=0;
    mkdirs(sandbox);wchar_t path[32768];_snwprintf(path,32768,L"%s\\probe.log",sandbox);logHandle=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);if(logHandle==INVALID_HANDLE_VALUE)return FALSE;
    log("probe attach pid=%lu selftest=%u",GetCurrentProcessId(),selfTest?1:0);if(campaignMode)log("CAMPAIGN CONFIG enabled=1 fixture=%u research_autotest=0",campaignFixture?1u:0u);originalErrno=(decltype(originalErrno))GetProcAddress(GetModuleHandleW(L"MSVCR80.dll"),"_errno");if(!install()){log("FAIL installation; launcher must terminate");return FALSE;}if(!AddVectoredExceptionHandler(1,exception)){log("FAIL exception diagnostic registration");return FALSE;}log("READY pre-entry hooks installed; registry entirely virtual; raw input and timing preserved");return TRUE;
}
