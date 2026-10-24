#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <stdio.h>
#include <wchar.h>
#pragma comment(lib,"bcrypt.lib")

static const char expectedHash[]="ebd30047d7e58c95bf7dc4bd1fb4a5780e16aa472b61c25a8c421632662083db";
static bool sha(const wchar_t* path,char* result){HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,0,0);if(file==INVALID_HANDLE_VALUE)return false;BCRYPT_ALG_HANDLE alg=0;BCRYPT_HASH_HANDLE hash=0;bool ok=BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,0,0)>=0;DWORD n=0,needed=0;BYTE* object=0;BYTE digest[32],data[65536];if(ok)ok=BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,(BYTE*)&n,sizeof(n),&needed,0)>=0;if(ok){object=(BYTE*)HeapAlloc(GetProcessHeap(),0,n);ok=object&&BCryptCreateHash(alg,&hash,object,n,0,0,0)>=0;}DWORD got=0;while(ok){if(!ReadFile(file,data,sizeof(data),&got,0)){ok=false;break;}if(!got)break;ok=BCryptHashData(hash,data,got,0)>=0;}if(ok)ok=BCryptFinishHash(hash,digest,32,0)>=0;for(unsigned i=0;ok&&i<32;i++)sprintf(result+i*2,"%02x",digest[i]);if(hash)BCryptDestroyHash(hash);if(alg)BCryptCloseAlgorithmProvider(alg,0);if(object)HeapFree(GetProcessHeap(),0,object);CloseHandle(file);return ok;}
static bool pe(const wchar_t* path,DWORD& entry){HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,0,0);if(h==INVALID_HANDLE_VALUE)return false;BYTE data[4096];DWORD n;bool ok=ReadFile(h,data,sizeof(data),&n,0)&&n==sizeof(data);CloseHandle(h);if(!ok)return false;auto dos=(IMAGE_DOS_HEADER*)data;if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<0||dos->e_lfanew>2048)return false;auto nt=(IMAGE_NT_HEADERS*)(data+dos->e_lfanew);if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC||nt->OptionalHeader.ImageBase!=0x400000||nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size)return false;entry=0x400000+nt->OptionalHeader.AddressOfEntryPoint;return true;}
static void parentDir(wchar_t* p){wchar_t* s=wcsrchr(p,L'\\');if(s)*s=0;}
static bool absolute(const wchar_t* p,wchar_t* out){DWORD n=GetFullPathNameW(p,32768,out,0);return n&&n<32768;}
static bool within(const wchar_t* p,const wchar_t* root){size_t n=wcslen(root);return !_wcsnicmp(p,root,n)&&(p[n]==0||p[n]=='\\');}
static bool noReparse(const wchar_t* path){wchar_t b[32768];lstrcpyW(b,path);for(wchar_t* p=b+3;*p;p++)if(*p=='\\'){*p=0;DWORD a=GetFileAttributesW(b);*p='\\';if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT))return false;}DWORD a=GetFileAttributesW(b);return a==INVALID_FILE_ATTRIBUTES||!(a&FILE_ATTRIBUTE_REPARSE_POINT);}
// 0 = proxy has not reached the game's entry yet, 1 = probe loaded, 2 = probe failed to load.
static int proxyState(const wchar_t* path){HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);if(file==INVALID_HANDLE_VALUE)return 0;char text[4096]={};DWORD read=0;BOOL ok=ReadFile(file,text,sizeof(text)-1,&read,0);CloseHandle(file);if(!ok)return 0;const char* entry=strstr(text,"entry reached");if(!entry)return 0;const char* handle=strstr(entry,"handle=");return handle&&strncmp(handle,"handle=00000000",15)&&strncmp(handle,"handle=0000000000000000",23)?1:2;}
// The probe's own log also proves it loaded (its READY line is written only after all pre-entry hooks are installed);
// used when the proxy's line is missing, e.g. a scanner held proxy.log (tester session 20261024T142800896).
static bool probeReady(const wchar_t* sandbox){wchar_t path[32768];_snwprintf(path,32768,L"%s\\probe.log",sandbox);
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);if(file==INVALID_HANDLE_VALUE)return false;
    static char text[65536];DWORD read=0;BOOL ok=ReadFile(file,text,sizeof(text)-1,&read,0);CloseHandle(file);if(!ok)return false;text[read]=0;
    for(DWORD i=0;i<read;i++)if(!text[i])text[i]=' ';return strstr(text,"READY pre-entry hooks installed")!=0;}
// Every build of proxy/proxy.cpp reads this variable name, so its UTF-16 text identifies an older build of the add-on's own proxy.
static bool ownProxyBuild(const wchar_t* path){
    HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);if(f==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size={};bool found=false;
    if(GetFileSizeEx(f,&size)&&size.QuadPart>0&&size.QuadPart<=8*1024*1024){DWORD n=(DWORD)size.QuadPart,read=0;BYTE* data=(BYTE*)HeapAlloc(GetProcessHeap(),0,n);
        if(data&&ReadFile(f,data,n,&read,0)&&read==n){const wchar_t marker[]=L"COOP_PROXY_PROBE";DWORD m=sizeof(marker)-sizeof(wchar_t);for(DWORD i=0;i+m<=n&&!found;i++)found=!memcmp(data+i,marker,m);}
        if(data)HeapFree(GetProcessHeap(),0,data);}
    CloseHandle(f);return found;
}
int wmain(int argc,wchar_t** argv){bool self=argc>=2&&!wcscmp(argv[1],L"--selftest"),play=argc>=2&&!wcscmp(argv[1],L"--play");if(argc<3||(play&&argc!=4)){fwprintf(stderr,L"Usage: native-launcher.exe <test Game.exe> <new sandbox dir> [seconds=15]\n       native-launcher.exe --selftest <new sandbox dir> [seconds=15]\n       native-launcher.exe --play <test Game.exe> <new sandbox dir>\n");return 2;}
    if(play){wchar_t human[8]={0},fixture[8]={0},frames[16]={0};GetEnvironmentVariableW(L"COOP_PROBE_HUMAN",human,8);GetEnvironmentVariableW(L"COOP_PROBE_HUMAN_FIXTURE",fixture,8);GetEnvironmentVariableW(L"COOP_PROBE_NET_FRAMES",frames,16);wchar_t solo[8]={0};GetEnvironmentVariableW(L"COOP_PROBE_SOLO",solo,8);/* Solo play (one player, no network) is the other unlimited mode; the probe validates its exclusions. */if(wcscmp(solo,L"1")&&(wcscmp(human,L"1")||!wcscmp(fixture,L"1")||wcscmp(frames,L"0"))){fprintf(stderr,"Play mode requires real human input and unlimited network frames, or solo play.\n");return 2;}}
    wchar_t exe[32768],sandbox[32768],dll[32768],gameDir[32768],ownDir[32768],localRoot[32768],testRoot[32768],incomingRoot[32768];GetModuleFileNameW(0,ownDir,32768);parentDir(ownDir);lstrcpyW(localRoot,ownDir);parentDir(localRoot);_snwprintf(testRoot,32768,L"%s\\test",localRoot);_snwprintf(incomingRoot,32768,L"%s\\incoming",localRoot);
    if(self){_snwprintf(exe,32768,L"%s\\native-selftest.exe",ownDir);if(!absolute(argv[2],sandbox))return 2;}else{if(!absolute(argv[play?2:1],exe)||!absolute(argv[play?3:2],sandbox))return 2;}
    if((!self&&!within(exe,testRoot))||!within(sandbox,localRoot)||within(sandbox,incomingRoot)||!noReparse(exe)||!noReparse(sandbox)){fprintf(stderr,"Target must be under local test/; sandbox must stay in local workspace and outside incoming/; no reparse points.\n");return 2;}
    _snwprintf(dll,32768,L"%s\\native-probe.dll",ownDir);lstrcpyW(gameDir,exe);parentDir(gameDir);DWORD seconds=15;if(!play&&argc>3)seconds=wcstoul(argv[3],0,10);if(!play&&(seconds<1||seconds>(self?60u:1200u)))return 2;
    if(GetFileAttributesW(sandbox)!=INVALID_FILE_ATTRIBUTES){fwprintf(stderr,L"Sandbox must be a NEW directory.\n");return 2;}
    char digest[65]={0};DWORD entry=0;if(!sha(exe,digest)||(!self&&strcmp(digest,expectedHash))||!pe(exe,entry)){fprintf(stderr,"Executable SHA/PE/TLS guard failed. SHA=%s\n",digest);return 3;}
    if(!self&&(entry!=0x52fd2c)){fprintf(stderr,"Unexpected entrypoint.\n");return 3;}if(GetFileAttributesW(dll)==INVALID_FILE_ATTRIBUTES)return 3;
    if(!CreateDirectoryW(sandbox,0)){fwprintf(stderr,L"Unable to create new sandbox, error=%lu\n",GetLastError());return 3;}
    SetEnvironmentVariableW(L"COOP_PROBE_ROOT",sandbox);SetEnvironmentVariableW(L"COOP_PROBE_GAME_ROOT",gameDir);SetEnvironmentVariableW(L"COOP_PROBE_SELFTEST",self?L"1":L"0");
    if(self){wchar_t denied[32768],control[32768];_snwprintf(denied,32768,L"%s\\analysis\\native\\selftest-denied-%lu-%llu.txt",localRoot,GetCurrentProcessId(),GetTickCount64());_snwprintf(control,32768,L"%s-control",denied);if(within(denied,gameDir)||within(denied,sandbox)||GetFileAttributesW(denied)!=INVALID_FILE_ATTRIBUTES)return 3;HANDLE check=CreateFileW(control,GENERIC_WRITE,0,0,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,0);if(check==INVALID_HANDLE_VALUE){fprintf(stderr,"Selftest ACL-control file creation failed.\n");return 3;}CloseHandle(check);SetEnvironmentVariableW(L"COOP_PROBE_DENIED_WRITE",denied);printf("SELFTEST_ACL_CONTROL_CREATED\n");}
    wchar_t cmd[32768];_snwprintf(cmd,32768,L"\"%s\"",exe);STARTUPINFOW si={sizeof(si)};si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_SHOWNOACTIVATE;PROCESS_INFORMATION pi={};
    HANDLE job=CreateJobObjectW(0,0);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_ACTIVE_PROCESS;limits.BasicLimitInformation.ActiveProcessLimit=1;if(!job||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))return 4;
    // Load path: a d3d9.dll proxy beside Game.exe is loaded by Windows itself (the game imports d3d9) and loads native-probe.dll at the
    // game's entry point. This launcher writes nothing into the game's memory and starts no remote thread.
    static wchar_t proxySrc[32768],proxyDst[32768],chainDst[32768],systemD3d[32768];_snwprintf(proxySrc,32768,L"%s\\d3d9.dll",ownDir);_snwprintf(proxyDst,32768,L"%s\\d3d9.dll",gameDir);_snwprintf(chainDst,32768,L"%s\\d3d9_chain.dll",gameDir);
    UINT sysLen=GetSystemDirectoryW(systemD3d,32768);if(!sysLen||sysLen>32768-16){fprintf(stderr,"System directory lookup failed.\n");return 3;}lstrcatW(systemD3d,L"\\d3d9.dll");
    if(GetFileAttributesW(proxySrc)==INVALID_FILE_ATTRIBUTES){fwprintf(stderr,L"Missing %s (build the proxy: probe\\build.cmd).\n",proxySrc);return 3;}
    // An existing d3d9.dll is used only if it is this proxy; an older build of it (left by an update or a crash) is replaced;
    // anything else is someone else's file and is never overwritten. After the game exits both copies are removed again.
    bool placedProxy=lstrcmpiW(proxySrc,proxyDst)!=0;
    {char a[65]={0},b[65]={0};if(GetFileAttributesW(proxyDst)!=INVALID_FILE_ATTRIBUTES){if(!sha(proxySrc,a)||!sha(proxyDst,b)||strcmp(a,b)){
            if(!ownProxyBuild(proxyDst)){fwprintf(stderr,L"%s already exists and is not this proxy; refusing to overwrite it.\n",proxyDst);return 3;}
            if(!CopyFileW(proxySrc,proxyDst,FALSE)){fprintf(stderr,"Replacing an older build of the d3d9 proxy failed error=%lu\n",GetLastError());return 3;}printf("PROXY replaced an older build of this add-on's d3d9 proxy\n");}}
        else if(!CopyFileW(proxySrc,proxyDst,TRUE)){fprintf(stderr,"Copying the d3d9 proxy failed error=%lu\n",GetLastError());return 3;}
        if(!CopyFileW(systemD3d,chainDst,FALSE)){fprintf(stderr,"Copying the system d3d9 failed error=%lu\n",GetLastError());return 3;}}
    SetEnvironmentVariableW(L"COOP_PROXY_PROBE",dll);
    // The game starts inside the job object from its first instruction (job list attribute), so no suspended start is needed.
    SIZE_T attrSize=0;InitializeProcThreadAttributeList(0,1,0,&attrSize);STARTUPINFOEXW sx={};sx.StartupInfo=si;sx.StartupInfo.cb=sizeof(sx);sx.lpAttributeList=(LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(),0,attrSize);
    if(!sx.lpAttributeList||!InitializeProcThreadAttributeList(sx.lpAttributeList,1,0,&attrSize)||!UpdateProcThreadAttribute(sx.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,&job,sizeof(job),0,0)){fprintf(stderr,"Job attribute setup failed error=%lu\n",GetLastError());CloseHandle(job);return 4;}
    if(!CreateProcessW(exe,cmd,0,0,FALSE,EXTENDED_STARTUPINFO_PRESENT|CREATE_UNICODE_ENVIRONMENT,0,gameDir,&sx.StartupInfo,&pi)){fprintf(stderr,"CreateProcess failed error=%lu\n",GetLastError());CloseHandle(job);return 4;}
    bool ok=false;DWORD stage=1;HANDLE injected=0;
    do{stage=3;static wchar_t proxyLog[32768];_snwprintf(proxyLog,32768,L"%s\\proxy.log",sandbox);bool ready=false;ULONGLONG deadline=GetTickCount64()+30000;
        // The proxy logs when the game reaches its entry point and whether the probe loaded. A game that could not load the probe is stopped (fail closed).
        while(GetTickCount64()<deadline){if(WaitForSingleObject(pi.hProcess,0)==WAIT_OBJECT_0)break;int state=proxyState(proxyLog);if(state==2)break;if(state==1||probeReady(sandbox)){ready=true;break;}Sleep(20);}if(!ready)break;
        printf("HOOK_READY pid=%lu sha256=%s selftest=%u timeout=%lus play=%u\n",pi.dwProcessId,digest,self?1:0,play?0u:seconds,play?1:0);fflush(stdout);
        wchar_t marker[32768];_snwprintf(marker,32768,L"%s\\probe-finished.signal",sandbox);bool scriptFinished=false;DWORD result=WAIT_TIMEOUT;ULONGLONG endTime=GetTickCount64()+seconds*1000;
        while(play||GetTickCount64()<endTime){result=WaitForSingleObject(pi.hProcess,250);if(result!=WAIT_TIMEOUT)break;DWORD attrs=GetFileAttributesW(marker);if(!self&&attrs!=INVALID_FILE_ATTRIBUTES&&!(attrs&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){scriptFinished=true;break;}}
        if(scriptFinished){printf("PROBE_SCRIPT_FINISHED: terminating owned job pid=%lu\n",pi.dwProcessId);TerminateJobObject(job,126);WaitForSingleObject(pi.hProcess,5000);}
        else if(result==WAIT_TIMEOUT){printf("TIMEOUT: terminating owned job pid=%lu\n",pi.dwProcessId);TerminateJobObject(job,124);WaitForSingleObject(pi.hProcess,5000);}else if(result!=WAIT_OBJECT_0)break;
        DWORD code=0;GetExitCodeProcess(pi.hProcess,&code);printf("PROCESS_EXIT pid=%lu code=%lu\n",pi.dwProcessId,code);ok=code==0||(!self&&(code==124||code==126));if(self&&code==0)printf("OWN_SELFTEST_PASS\n");
    }while(false);
    if(!ok){fprintf(stderr,"FAIL closed at stage=%lu error=%lu; terminating only owned process pid=%lu\n",stage,GetLastError(),pi.dwProcessId);TerminateProcess(pi.hProcess,125);WaitForSingleObject(pi.hProcess,5000);}if(injected)CloseHandle(injected);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(job);
    if(placedProxy){char a[65]={0},b[65]={0};if(sha(proxySrc,a)&&sha(proxyDst,b)&&!strcmp(a,b))DeleteFileW(proxyDst);DeleteFileW(chainDst);}
    return ok?0:5;
}
