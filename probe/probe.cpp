#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
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

// Own research code. Hooks the guarded executable only; this is not an OS sandbox.
static wchar_t sandbox[32768], gameRoot[32768];
static HANDLE logHandle=INVALID_HANDLE_VALUE;
static DWORD logLock=0;
static unsigned inputCalls=0,timeCalls=0,presentCalls=0,sceneCalls=0;
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
static bool networkPathSync=false;
static bool networkAmbientSplit=false;
static DWORD networkAmbientRng=0x84f19a21;
static bool humanInputMode=false,humanInputFixture=false,virtualPadSelected=false;
static bool probeDiagnostics=true;
static bool humanDisconnectFixture=false;
static bool humanKeyboardFixture=false;
static DWORD virtualPadIndex=3;
static bool networkInputClock=false;
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
    wchar_t q[32768]; if(!fullpath(p,q) || !(prefix(q,sandbox)||(!selfTest&&prefix(q,gameRoot))))return false;
    // No reparse-point traversal within writable roots.
    for(wchar_t* c=q+3;*c;c++) if(*c=='\\') {wchar_t old=*c;*c=0;DWORD a=GetFileAttributesW(q);*c=old;if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT))return false;}
    DWORD a=GetFileAttributesW(q); return a==INVALID_FILE_ATTRIBUTES||!(a&FILE_ATTRIBUTE_REPARSE_POINT);
}
static bool asciiWide(const char* in,wchar_t* out) {return in&&MultiByteToWideChar(CP_ACP,0,in,-1,out,32768)>0;}
static void mkdirs(const wchar_t* path) {
    wchar_t b[32768]; lstrcpynW(b,path,32768);
    for(wchar_t* p=b+3;*p;p++)if(*p=='\\'){*p=0;CreateDirectoryW(b,0);*p='\\';} CreateDirectoryW(b,0);
}
static HRESULT WINAPI folder(HWND,int id,HANDLE,DWORD,wchar_t* out) {
    const wchar_t* leaf=0; switch(id&0xff){case 5:leaf=L"Documents";break;case 0x1a:leaf=L"AppData\\Roaming";break;case 0x1c:leaf=L"AppData\\Local";break;case 0x23:leaf=L"ProgramData";break;case 0x25:{wchar_t b[MAX_PATH];GetSystemDirectoryW(b,MAX_PATH);lstrcpynW(out,b,MAX_PATH);log("folder system id=%x real system allowed",id);return S_OK;}default:log("BLOCK unknown folder id=%x",id);SetLastError(ERROR_ACCESS_DENIED);return E_ACCESSDENIED;}
    wchar_t p[32768];_snwprintf(p,32768,L"%s\\%s",sandbox,leaf);if(wcslen(p)>=MAX_PATH)return E_FAIL;mkdirs(p);lstrcpyW(out,p);log("folder id=%x redirected",id);return S_OK;
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
    if(!_wcsicmp(name,L"ResolutionX")){forced=640;known=true;}if(!_wcsicmp(name,L"ResolutionY")){forced=480;known=true;}
    if(known){if(type)*type=REG_DWORD;if(data&&*n<4){*n=4;return ERROR_MORE_DATA;}if(data)memcpy(data,&forced,4);*n=4;log("registry known setting name=%ls value=%lu",name,forced);return ERROR_SUCCESS;}
    for(auto&v:values)if(v.used&&!_wcsicmp(path,v.path)&&!_wcsicmp(name,v.name)){if(type)*type=v.type;DWORD capacity=*n;*n=v.size;if(data&&capacity<v.size)return ERROR_MORE_DATA;if(data)memcpy(data,v.data,v.size);return ERROR_SUCCESS;}
    log("registry query missing name=%ls",name);return ERROR_FILE_NOT_FOUND;
}
static LSTATUS WINAPI regQueryA(HKEY k,LPCSTR name,LPDWORD reserved,LPDWORD type,LPBYTE data,LPDWORD n){wchar_t w[256];if(!name||!MultiByteToWideChar(CP_ACP,0,name,-1,w,256))return ERROR_ACCESS_DENIED;return regQuery(k,w,reserved,type,data,n);}

static HANDLE (WINAPI* originalCreateW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
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
static HANDLE WINAPI createW(LPCWSTR p,DWORD a,DWORD share,LPSECURITY_ATTRIBUTES sa,DWORD disp,DWORD flags,HANDLE t){if((writing(a,disp)||(flags&FILE_FLAG_DELETE_ON_CLOSE))&&!allowed(p)){log("BLOCK CreateFileW write outside sandbox");SetLastError(ERROR_ACCESS_DENIED);return INVALID_HANDLE_VALUE;}if(!writing(a,disp))fileRead(p);HANDLE h=originalCreateW(p,a,share,sa,disp,flags,t);DWORD error=GetLastError();if(h==INVALID_HANDLE_VALUE&&failedFileLogs++<60)log("FILE FAILED W path=%ls error=%lu access=%08lx disposition=%lu",p,error,a,disp);SetLastError(error);return h;}
static HANDLE WINAPI createA(LPCSTR p,DWORD a,DWORD share,LPSECURITY_ATTRIBUTES sa,DWORD disp,DWORD flags,HANDLE t){wchar_t w[32768];if((writing(a,disp)||(flags&FILE_FLAG_DELETE_ON_CLOSE))&&(!asciiWide(p,w)||!allowed(w))){log("BLOCK CreateFileA write outside sandbox");SetLastError(ERROR_ACCESS_DENIED);return INVALID_HANDLE_VALUE;}if(!writing(a,disp)&&asciiWide(p,w))fileRead(w);HANDLE h=originalCreateA(p,a,share,sa,disp,flags,t);DWORD error=GetLastError();if(h==INVALID_HANDLE_VALUE&&failedFileLogs++<60)log("FILE FAILED A path=%s error=%lu access=%08lx disposition=%lu",p,error,a,disp);SetLastError(error);return h;}
static BOOL WINAPI write(HANDLE h,LPCVOID b,DWORD n,LPDWORD done,LPOVERLAPPED o){wchar_t p[32768];DWORD c=GetFinalPathNameByHandleW(h,p,32768,FILE_NAME_NORMALIZED);if(!c||c>=32768||!allowed(p)){log("BLOCK WriteFile unknown/outside handle");if(done)*done=0;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}return originalWrite(h,b,n,done,o);}
static FILE* __cdecl wfopenHook(const wchar_t* p,const wchar_t* mode){if(!mode)return 0;if(wcspbrk(mode,L"wa+")&&!allowed(p)){log("BLOCK _wfopen write outside sandbox");return 0;}if(!wcspbrk(mode,L"wa+"))fileRead(p);return originalWfopen(p,mode);}
static FILE* __cdecl fopenHook(const char* p,const char* mode){wchar_t w[32768];if(!mode)return 0;if(strpbrk(mode,"wa+")&&(!asciiWide(p,w)||!allowed(w))){log("BLOCK fopen write outside sandbox");return 0;}if(!strpbrk(mode,"wa+")&&asciiWide(p,w))fileRead(w);return originalFopen(p,mode);}
static int __cdecl mkdirHook(const wchar_t*p){if(!allowed(p)){wchar_t normalized[32768];bool ancestor=false;if(p&&fullpath(p,normalized)){DWORD attrs=GetFileAttributesW(normalized);ancestor=attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_DIRECTORY)&&(prefix(sandbox,normalized)||(!selfTest&&prefix(gameRoot,normalized)));}if(ancestor){if(originalErrno)*originalErrno()=EEXIST;SetLastError(ERROR_ALREADY_EXISTS);log("_wmkdir EXISTING ancestor, no mutation path=%ls",p);return -1;}if(originalErrno)*originalErrno()=EACCES;SetLastError(ERROR_ACCESS_DENIED);log("BLOCK _wmkdir outside sandbox path=%ls",p?p:L"<null>");return -1;}log("_wmkdir allowed path=%ls",p);return originalMkdir(p);}
static int __cdecl removeHook(const wchar_t*p){if(!allowed(p)){log("BLOCK _wremove outside sandbox");return -1;}return originalRemove(p);}
static HINSTANCE WINAPI shell(HWND,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR,INT){log("BLOCK ShellExecuteW");SetLastError(ERROR_ACCESS_DENIED);return (HINSTANCE)SE_ERR_ACCESSDENIED;}
static BOOL WINAPI clip(const RECT*){log("cursor ClipCursor suppressed");return TRUE;}
static int WINAPI cursor(BOOL){return 0;}
static HWND (WINAPI* originalWindow)(DWORD,LPCSTR,LPCSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,LPVOID);
static BOOL (WINAPI* originalShow)(HWND,int);
static WNDPROC originalGameWindowProc=0;
static LRESULT CALLBACK gameWindowProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam){
    if(message==WM_CLOSE&&hwnd==gameWindow&&networkRole&&networkReady&&!networkComplete&&!networkFailed&&networkWire){
        bool sent=networkWire->RequestStop();log("NET user stop requested sent=%u",sent?1:0);
    }
    return CallWindowProcA(originalGameWindowProc,hwnd,message,wparam,lparam);
}
static HWND WINAPI window(DWORD ex,LPCSTR cl,LPCSTR title,DWORD,int,int,int,int,HWND parent,HMENU menu,HINSTANCE instance,LPVOID param){bool interactive=humanInputMode&&!humanInputFixture;log("window 640x480 interactive=%u",interactive);HWND result=originalWindow(interactive?ex:ex|WS_EX_NOACTIVATE,cl,title,WS_OVERLAPPEDWINDOW,0,0,660,520,parent,menu,instance,param);if(result&&!parent){gameWindow=result;
        if(networkRole&&!selfTest){WNDPROC previous=(WNDPROC)GetWindowLongPtrA(result,GWLP_WNDPROC);
            if(previous!=(WNDPROC)0x402d5c){log("FAIL game window procedure guard original=%p",previous);DestroyWindow(result);return 0;}
            originalGameWindowProc=previous;SetLastError(0);
            if((WNDPROC)SetWindowLongPtrA(result,GWLP_WNDPROC,(LONG_PTR)gameWindowProc)!=previous){log("FAIL game window procedure installation error=%lu",GetLastError());DestroyWindow(result);return 0;}
            log("WINDOW owned close hook installed original=%p",previous);
        }
    }return result;}
static BOOL WINAPI show(HWND h,int){return originalShow(h,humanInputMode&&!humanInputFixture?SW_SHOWNORMAL:SW_SHOWNOACTIVATE);}

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
static void windowParams(D3DPRESENT_PARAMETERS* p){p->Windowed=TRUE;p->FullScreen_RefreshRateInHz=0;p->BackBufferWidth=640;p->BackBufferHeight=480;}
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
static HRESULT WINAPI present(IDirect3DDevice9*p,const RECT*a,const RECT*b,HWND c,const RGNDATA*d){
    static ULONGLONG started=0;static unsigned lastCapture=0;
    ULONGLONG now=GetTickCount64();if(!started)started=now;unsigned seconds=(unsigned)((now-started)/1000);
    unsigned n=++presentCalls;if(n==1||n%6000==0){log("D3D Present count=%u elapsed_s=%u",n,seconds);if(!selfTest)log("read-only state rng=%08lx sim_mode=%lu fixed_sim_fps=%g",*(volatile DWORD*)0x58cf24,*(volatile DWORD*)0x69ed24,*(volatile float*)0x591df8);}
    if(n==120)capture(p,n);if(seconds>=5&&seconds/5>lastCapture){lastCapture=seconds/5;capture(p,200000+seconds);}
    if(playBot||networkRole){
        static LARGE_INTEGER frequency={0},next={0};LARGE_INTEGER counter;
        if(!frequency.QuadPart)QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&counter);
        if(!next.QuadPart||counter.QuadPart>next.QuadPart+frequency.QuadPart/20)next.QuadPart=counter.QuadPart;
        while(counter.QuadPart<next.QuadPart){Sleep(1);QueryPerformanceCounter(&counter);}next.QuadPart+=frequency.QuadPart/60;
    }
    return originalPresent(p,a,b,c,d);
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
static Hook hooks[]={H("SHGetFolderPathW",folder),H("RegCreateKeyW",regCreate),H("RegOpenKeyExA",regOpenA),H("RegSetValueExW",regSet),H("RegQueryValueExW",regQuery),H("RegQueryValueExA",regQueryA),H("RegCloseKey",regClose),O("CreateFileW",createW,originalCreateW),O("CreateFileA",createA,originalCreateA),O("WriteFile",write,originalWrite),O("_wfopen",wfopenHook,originalWfopen),O("fopen",fopenHook,originalFopen),O("_wmkdir",mkdirHook,originalMkdir),O("_wremove",removeHook,originalRemove),H("ShellExecuteW",shell),H("ClipCursor",clip),H("ShowCursor",cursor),O("CreateWindowExA",window,originalWindow),O("ShowWindow",show,originalShow),O("Direct3DCreate9",d3d,originalD3D),O("GetProcAddress",proc,originalProc),O("timeGetTime",timeHook,originalTime)};
static bool __cdecl testEnabled(const char* path,bool fallback){if(path&&!strcmp(path,"/user/test/enable")){log("AUTOTEST enable request observed, returning true");return true;}return ((bool(__cdecl*)(const char*,bool))0x43754f)(path,fallback);}
static void* __stdcall testNameExists(const char* path){if(path&&!strcmp(path,"/user/test/testname")){log("AUTOTEST name existence request observed");return (void*)1;}return ((void*(__stdcall*)(const char*))0x437d6b)(path);}
static ULONG_PTR __cdecl testNameValue(const char* path,ULONG_PTR fallback){if(path&&!strcmp(path,"/user/test/testname")){log("AUTOTEST selected shipped %s function",autoTestName);return (ULONG_PTR)autoTestName;}if(path&&!strcmp(path,"/user/test/area_name")){log("AUTOTEST selected area %s",networkArea);return (ULONG_PTR)networkArea;}if(path&&!strcmp(path,"/user/test/sidekick_name")){static const char sidekick[]="rhino";log("AUTOTEST selected sidekick %s",sidekick);return (ULONG_PTR)sidekick;}return ((ULONG_PTR(__cdecl*)(const char*,ULONG_PTR))0x437515)(path,fallback);}
typedef void (__thiscall* FlowNativeFn)(void*,unsigned);
static bool readMemory(const void* address,void* output,SIZE_T bytes){SIZE_T got=0;return ReadProcessMemory(GetCurrentProcess(),address,output,bytes,&got)&&got==bytes;}
static DWORD readWord(ULONG_PTR address){DWORD word=0;readMemory((void*)address,&word,sizeof(word));return word;}
static float readFloat(ULONG_PTR address){float value=0;readMemory((void*)address,&value,sizeof(value));return value;}
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
static coop::Actions physicalFixtureActions(bool analog){
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
        if(elapsed>=4000&&(current<8||(current==16&&readWord(movie+4)==1))&&(elapsed-4000)%2500<120)state->Gamepad.wButtons=((elapsed-4000)/2500)%2?XINPUT_GAMEPAD_START:XINPUT_GAMEPAD_A;
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
    else if(elapsed>=4000&&(current<8||(current==16&&readWord(movie+4)==1))&&(elapsed-4000)%2500<120){actions[5]=1;actions[10]=1;}
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
    bool ambient=networkRole&&networkAmbientSplit&&state==(DWORD*)0x58cf24&&(outerCaller==0x49115a||outerCaller==0x49233f);
    if(ambient)state=&networkAmbientRng;
    bool tracked=networkRole&&networkReady&&!networkFailed&&!networkComplete&&(state==(DWORD*)0x58cf24||(networkInSimulation&&GetCurrentThreadId()==networkSimulationThread));
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
static void networkInput(){
    if(networkFailed||networkComplete){applyNetworkActions(0,{});applyNetworkActions(1,{});if(networkFailed&&gameWindow&&!networkCloseRequested){networkCloseRequested=true;log("NET failure requests normal close of owned game window");PostMessageW(gameWindow,WM_CLOSE,0,0);}return;}
    if(!networkReady){
        coop::Digest build={},scenario={};const char* digest="ebd30047d7e58c95bf7dc4bd1fb4a5780e16aa472b61c25a8c421632662083db";
        for(unsigned i=0;i<32;i++){unsigned byte=0;sscanf(digest+i*2,"%2x",&byte);build[i]=(BYTE)byte;}
        if(networkContentVerified)build=networkContent;
        // Private scenario tag: Tokyo1/1, seed8466,60Hz,delay3,combat bot2.
        scenario[0]=10;scenario[1]=0x21;scenario[2]=0x12;scenario[3]=60;scenario[4]=3;scenario[5]=networkPathSync?1:0;scenario[6]=networkAmbientSplit?1:0;scenario[7]=humanInputMode?1:0;scenario[8]=humanInputFixture?1:0;
        memcpy(scenario.data()+12,&networkFrameLimit,sizeof(networkFrameLimit));
        unsigned areaHash=2166136261u;for(const char* c=networkArea;*c;c++)areaHash=(areaHash^(BYTE)*c)*16777619u;memcpy(scenario.data()+16,&areaHash,sizeof(areaHash));
        scenario[9]=networkTransitionFixture?1:0;
        scenario[10]=(BYTE)networkTransitionTarget;scenario[11]=networkPauseFixture?1:0;
        networkWire=new coop::InputWire;
        log("NET opening role=%u address=%s port=%u frame_limit=%u content_preflight=%u area=%s jitter_fixture=%u",networkRole,networkAddress,networkPort,networkFrameLimit,networkContentVerified,networkArea,networkJitterFixture);
        networkReady=networkWire->Open(networkRole==1,networkAddress,networkPort,build,scenario,3,30000);
        if(!networkReady){networkFailed=true;log("NET FAIL opening %s",networkWire->Error().c_str());return;}
        log("NET READY role=%u",networkRole);
    }
    if(networkFrameLimit&&networkTick>=networkFrameLimit){
        if(!networkWire->Finish()){networkFailed=true;log("NET FAIL finish %s",networkWire->Error().c_str());return;}
        networkComplete=true;log("NET SCRIPT_COMPLETE frames=%u; peer completion confirmed; no co-op QA awarded",networkTick);
        if(networkCleanExit){applyNetworkActions(0,{});applyNetworkActions(1,{});if(!gameWindow||!PostMessageW(gameWindow,WM_CLOSE,0,0)){networkFailed=true;log("NET FAIL clean close request error=%lu",GetLastError());}else log("NET clean WM_CLOSE posted to owned game window");return;}
        wchar_t path[32768];_snwprintf(path,32768,L"%s\\probe-finished.signal",sandbox);HANDLE marker=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,0,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,0);
        if(marker==INVALID_HANDLE_VALUE){networkFailed=true;log("NET FAIL completion marker error=%lu",GetLastError());return;}
        const char message[]="Paired bot frame limit completed. Transport and state logs require review.\r\n";DWORD written=0;BOOL saved=WriteFile(marker,message,sizeof(message)-1,&written,0);FlushFileBuffers(marker);CloseHandle(marker);if(!saved||written!=sizeof(message)-1){networkFailed=true;log("NET FAIL completion marker write");}
        applyNetworkActions(0,{});applyNetworkActions(1,{});return;
    }
    static coop::Actions history[32];coop::Actions local=humanInputMode?captureLocalActions():networkBotActions(networkRole==1?0:1,networkTick),remote={};
    history[networkTick%32]=local;
    verifyNativeAxes(local);if(networkFailed)return;
    static std::uint64_t states[32]={};states[networkTick%32]=networkState;std::uint64_t remoteState=0;
    if(networkJitterFixture){if((networkTick+networkRole*7)%43==0)Sleep(45+networkRole*10);if(networkTick==1500&&networkRole==2)Sleep(500);}
    if(!networkWire->Step(networkTick,local,remote,networkState,&remoteState)){
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
static void transitionFixture(unsigned tick){
    if(!networkTransitionFixture||tick<networkTransitionNextTick)return;
    DWORD areas=readWord(0x6a9974),currentArea=readWord(areas+0x18),slot=readWord(areas+0x14);
    if(tick%120==0){DWORD transition=readWord(0x6a9988);log("TRANSITION STATUS tick=%u slot=%lu stage=%lu exit_kind=%lu area_id=%08lx next_link=%08lx preload_id=%08lx script_flags=%08lx",tick,slot,readWord(transition+0x2c),readWord(transition+0x34),readWord(currentArea),readWord(readWord(currentArea+0x10)),readWord(readWord(areas+0x1c)),readWord(readWord(0x69ed1c)+0x24));}
    if(networkTransitionWarped){if(!networkTransitionChanged&&currentArea&&currentArea!=networkTransitionStartArea){networkTransitionChanged=true;++networkTransitionCount;char areaName[64]={0};DWORD name=readWord(currentArea);for(unsigned i=0;i<sizeof(areaName)-1;i++){char c=0;if(!readMemory((void*)(name+i),&c,1)||!c)break;if((c<'A'||c>'Z')&&(c<'a'||c>'z')&&(c<'0'||c>'9')&&c!='_')break;areaName[i]=c;}log("TRANSITION AREA_CHANGED tick=%u old_slot=%lu new_slot=%lu count=%u area=%s",tick,networkTransitionStartSlot,slot,networkTransitionCount,areaName);if(networkTransitionCount<networkTransitionTarget){networkTransitionWarped=false;networkTransitionNextTick=tick+600;}}return;}
    if(networkTransitionCount>=networkTransitionTarget)return;
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
static void __fastcall simulation(void* task,void*,float dt){
    DWORD flow=readWord(0x69e72c);
    if(networkRole&&flow&&readWord(flow+8)==128){if(!networkReady||networkFailed||networkComplete||!networkTick)return;if(readWord(0x69ed24)!=0){networkFailed=true;log("NET FAIL unexpected simulation mode");return;}networkSimulationThread=GetCurrentThreadId();networkInSimulation=true;BYTE previousPathFlag=*(BYTE*)0x590718;if(networkPathSync)*(BYTE*)0x590718=1;transitionFixture(networkTick-1);if(!networkFailed)originalSimulation(task,dt);if(networkPathSync)*(BYTE*)0x590718=previousPathFlag;networkInSimulation=false;if(!networkFailed)observeNetworkState(networkTick-1);}
    else originalSimulation(task,dt);
}
struct TaskTimingHook {void** table;SimulationFn original;};
static TaskTimingHook taskTimingHooks[64]={};static unsigned taskTimingCount=0;
static void __fastcall fixedTask(void* task,void*,float dt){
    void** table=*(void***)task;SimulationFn original=0;
    for(unsigned i=0;i<taskTimingCount;i++)if(taskTimingHooks[i].table==table){original=taskTimingHooks[i].original;break;}
    if(!original){networkFailed=true;log("NET FAIL unknown task timing callback");return;}
    DWORD flow=readWord(0x69e72c);
    if(networkRole&&flow&&readWord(flow+8)==128){dt=1.0f/60.0f;*(float*)(flow+0x18)=dt;if(*((BYTE*)task+0x1c))dt*=*(float*)flow;*(float*)(flow+0x14)=dt;}
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
static void __cdecl inputUpdate(float dt){
    DWORD currentFlow=readWord(0x69e72c);if(networkRole&&currentFlow&&readWord(currentFlow+8)==128)dt=1.0f/60.0f;
    // Launching the owned research scenario must not depend on a connected pad.
    // Use the game's existing forced-action field for one boot-menu frame only;
    // leave physical polling and every in-game input untouched.
    static ULONGLONG bootStarted=0;static DWORD bootPulse=~0u;
    ULONGLONG bootNow=GetTickCount64();if(!bootStarted)bootStarted=bootNow;
    DWORD forcedAddress=0x691170+0x118,savedForced=readWord(forcedAddress);bool bootAction=false;
    if(autoTest&&networkRole&&humanInputMode&&!humanInputFixture&&currentFlow&&readWord(currentFlow+8)<8&&bootNow-bootStarted>=4000){
        DWORD pulse=(DWORD)((bootNow-bootStarted-4000)/2500);
        if(pulse!=bootPulse){bootPulse=pulse;*(DWORD*)forcedAddress=pulse%2?5:10;bootAction=true;log("BOOTSTRAP menu action=%lu flow=%lu",pulse%2?5ul:10ul,readWord(currentFlow+8));}
    }
    ((void(__cdecl*)(float))0x401b6f)(dt);
    if(bootAction)*(DWORD*)forcedAddress=savedForced;
    static ULONGLONG last=0;DWORD flow=readWord(0x69e72c);ULONGLONG now=GetTickCount64();
    if(networkRole&&flow&&readWord(flow+8)==128)networkInput();
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
    lastFlowNative=id;originalFlowNative(vm,id);
}
static bool patchVerifiedCall(DWORD address,DWORD expected,void* replacement){
    BYTE* at=(BYTE*)address;if(at[0]!=0xe8||(DWORD)(address+5+*(LONG*)(at+1))!=expected){log("FAIL AUTOTEST call signature address=%08lx",address);return false;}
    DWORD old=0;if(!VirtualProtect(at,5,PAGE_EXECUTE_READWRITE,&old))return false;*(DWORD*)(at+1)=(DWORD)(ULONG_PTR)replacement-(address+5);FlushInstructionCache(GetCurrentProcess(),at,5);DWORD ignored=0;bool ok=VirtualProtect(at,5,old,&ignored)!=0;log("AUTOTEST verified call patched address=%08lx original=%08lx",address,expected);return ok;
}
static bool install() {
    BYTE* base=(BYTE*)GetModuleHandleW(0);auto dos=(IMAGE_DOS_HEADER*)base;auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    if(!selfTest){const BYTE expected[]={0xe8,3,4,0,0,0xe9,0x35,0xfd,0xff,0xff};if((ULONG_PTR)base!=0x400000||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||nt->OptionalHeader.AddressOfEntryPoint!=0x12fd2c){log("FAIL game PE guard");return false;}
        // Entrypoint first two bytes are the launcher's intentional EB FE gate.
        BYTE* ep=base+nt->OptionalHeader.AddressOfEntryPoint;if(ep[0]!=0xeb||ep[1]!=0xfe||memcmp(ep+2,expected+2,sizeof(expected)-2)){log("FAIL entry gate signature");return false;}}
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
    if(!selfTest&&networkRole){void** slot=(void**)0x577e20;if(slot[0]!=(void*)0x4dbc39||!patchComSlot(slot,0,(void*)simulation,(void**)&originalSimulation)){log("FAIL simulation callback guard");return false;}}
    if(!selfTest&&networkRole)for(DWORD address:rngCallSites)if(!patchVerifiedCall(address,0x4318f5,(void*)randomEntry))return false;
    if(!selfTest&&humanKeyboardFixture&&!patchVerifiedCall(0x525c00,0x526240,(void*)keyboardPollFixture))return false;
    if(!selfTest&&humanInputMode)for(DWORD address:deviceCallSites)if(!patchVerifiedCall(address,0x40182d,(void*)reassignLocalDevice))return false;
    return true;
}
static LONG CALLBACK exception(EXCEPTION_POINTERS* p){
    LONG n=InterlockedIncrement(&exceptionCount);if(n<=5&&p&&p->ExceptionRecord&&p->ContextRecord){auto e=p->ExceptionRecord;auto c=p->ContextRecord;MEMORY_BASIC_INFORMATION m={};char module[MAX_PATH]={0};if(VirtualQuery((LPCVOID)c->Eip,&m,sizeof(m))&&m.AllocationBase)GetModuleFileNameA((HMODULE)m.AllocationBase,module,MAX_PATH);
        log("EXCEPTION count=%ld code=%08lx flags=%08lx ip=%08lx module=%s modulebase=%p relative=%08lx params=%lu accesskind=%08lx faultaddress=%08lx EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx ESI=%08lx EDI=%08lx EBP=%08lx ESP=%08lx",n,e->ExceptionCode,e->ExceptionFlags,c->Eip,module,m.AllocationBase,c->Eip-(DWORD)(ULONG_PTR)m.AllocationBase,e->NumberParameters,e->NumberParameters>0?(DWORD)e->ExceptionInformation[0]:0,e->NumberParameters>1?(DWORD)e->ExceptionInformation[1]:0,c->Eax,c->Ebx,c->Ecx,c->Edx,c->Esi,c->Edi,c->Ebp,c->Esp);
        DWORD stack[16]={0};SIZE_T got=0;if(ReadProcessMemory(GetCurrentProcess(),(LPCVOID)c->Esp,stack,sizeof(stack),&got)&&got==sizeof(stack)){
            log("EXCEPTION stack %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",stack[0],stack[1],stack[2],stack[3],stack[4],stack[5],stack[6],stack[7],stack[8],stack[9],stack[10],stack[11],stack[12],stack[13],stack[14],stack[15]);
        }
    }return EXCEPTION_CONTINUE_SEARCH;
}
BOOL WINAPI DllMain(HINSTANCE h,DWORD why,LPVOID){if(why!=DLL_PROCESS_ATTACH)return TRUE;DisableThreadLibraryCalls(h);
    wchar_t flag[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_SELFTEST",flag,8);selfTest=!wcscmp(flag,L"1");
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
    wchar_t transitionCount[16]={0};if(GetEnvironmentVariableW(L"COOP_PROBE_TRANSITION_COUNT",transitionCount,16)){wchar_t* end=0;unsigned long value=wcstoul(transitionCount,&end,10);if(!end||*end||value<1||value>4)return FALSE;networkTransitionTarget=value;}
    char contentOption[66]={0};DWORD contentLength=GetEnvironmentVariableA("COOP_PROBE_CONTENT_SHA256",contentOption,sizeof(contentOption));if(contentLength){if(contentLength!=64)return FALSE;for(unsigned i=0;i<32;i++){char pair[3]={contentOption[i*2],contentOption[i*2+1],0};if(!isxdigit((unsigned char)pair[0])||!isxdigit((unsigned char)pair[1]))return FALSE;unsigned value=0;sscanf(pair,"%2x",&value);networkContent[i]=(BYTE)value;}networkContentVerified=true;}
    char areaOption[128]={0};DWORD areaLength=GetEnvironmentVariableA("COOP_PROBE_AREA",areaOption,sizeof(areaOption));if(areaLength){if(areaLength>=sizeof(areaOption)||strstr(areaOption,".."))return FALSE;for(const char* c=areaOption;*c;c++)if(!isalnum((unsigned char)*c)&&*c!='/'&&*c!='_')return FALSE;strcpy(networkArea,areaOption);}
    wchar_t networkMode[16]={0};GetEnvironmentVariableW(L"COOP_PROBE_NET_MODE",networkMode,16);if(autoTest&&(actionBot||humanInputMode)){if(!wcscmp(networkMode,L"host"))networkRole=1;if(!wcscmp(networkMode,L"join"))networkRole=2;}
    wchar_t pauseOption[8]={0};DWORD pauseLength=GetEnvironmentVariableW(L"COOP_PROBE_PAUSE_FIXTURE",pauseOption,8);if(pauseLength){
        if(pauseLength>=8||(wcscmp(pauseOption,L"0")&&wcscmp(pauseOption,L"1")))return FALSE;
        networkPauseFixture=!wcscmp(pauseOption,L"1");if(networkPauseFixture&&(!humanInputFixture||!networkRole))return FALSE;
    }
    wchar_t pathMode[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_PATHSYNC",pathMode,8);networkPathSync=networkRole&&!wcscmp(pathMode,L"1");
    wchar_t ambientMode[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_AMBIENT_SPLIT",ambientMode,8);networkAmbientSplit=networkRole&&!wcscmp(ambientMode,L"1");
    char addressOption[64]={0};DWORD addressLength=GetEnvironmentVariableA("COOP_PROBE_NET_ADDRESS",addressOption,sizeof(addressOption));if(addressLength){if(addressLength>=sizeof(addressOption))return FALSE;strcpy(networkAddress,addressOption);}
    wchar_t portOption[16]={0};if(GetEnvironmentVariableW(L"COOP_PROBE_NET_PORT",portOption,16)){wchar_t* end=0;unsigned long value=wcstoul(portOption,&end,10);if(!end||*end||value<1024||value>65535)return FALSE;networkPort=(unsigned short)value;}
    wchar_t frameOption[16]={0};if(GetEnvironmentVariableW(L"COOP_PROBE_NET_FRAMES",frameOption,16)){wchar_t* end=0;unsigned long value=wcstoul(frameOption,&end,10);if(!end||*end||(value&&value<60)||value>1000000)return FALSE;networkFrameLimit=value;}
    if(networkPauseFixture&&networkFrameLimit&&networkFrameLimit<=430)return FALSE;
    probeDiagnostics=!(humanInputMode&&!humanInputFixture&&!networkFrameLimit);wchar_t diagnostics[8]={0};DWORD diagnosticsLength=GetEnvironmentVariableW(L"COOP_PROBE_DIAGNOSTICS",diagnostics,8);if(diagnosticsLength){if(diagnosticsLength>=8||(wcscmp(diagnostics,L"0")&&wcscmp(diagnostics,L"1")))return FALSE;probeDiagnostics=!wcscmp(diagnostics,L"1");}
    wchar_t exitOption[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_CLEAN_EXIT",exitOption,8);networkCleanExit=wcscmp(exitOption,L"0")!=0;
    wchar_t seconds[16]={0};GetEnvironmentVariableW(L"COOP_PROBE_GAME_SECONDS",seconds,16);unsigned secondsValue=wcstoul(seconds,0,10);if(secondsValue>=10&&secondsValue<=90)autoGameSeconds=(float)secondsValue;
    DWORD a=GetEnvironmentVariableW(L"COOP_PROBE_ROOT",sandbox,32768),b=GetEnvironmentVariableW(L"COOP_PROBE_GAME_ROOT",gameRoot,32768);if(!a||a>=32768||!b||b>=32768)return FALSE;
    wchar_t normalized[32768];if(!fullpath(sandbox,normalized))return FALSE;lstrcpyW(sandbox,normalized);if(!fullpath(gameRoot,normalized))return FALSE;lstrcpyW(gameRoot,normalized);
    while(wcslen(sandbox)>3&&sandbox[wcslen(sandbox)-1]=='\\')sandbox[wcslen(sandbox)-1]=0;while(wcslen(gameRoot)>3&&gameRoot[wcslen(gameRoot)-1]=='\\')gameRoot[wcslen(gameRoot)-1]=0;
    mkdirs(sandbox);wchar_t path[32768];_snwprintf(path,32768,L"%s\\probe.log",sandbox);logHandle=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);if(logHandle==INVALID_HANDLE_VALUE)return FALSE;
    log("probe attach pid=%lu selftest=%u",GetCurrentProcessId(),selfTest?1:0);originalErrno=(decltype(originalErrno))GetProcAddress(GetModuleHandleW(L"MSVCR80.dll"),"_errno");if(!install()){log("FAIL installation; launcher must terminate");return FALSE;}if(!AddVectoredExceptionHandler(1,exception)){log("FAIL exception diagnostic registration");return FALSE;}log("READY pre-entry hooks installed; registry entirely virtual; raw input and timing preserved");return TRUE;
}
