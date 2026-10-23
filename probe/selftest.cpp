#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"advapi32.lib")
#pragma comment(lib,"d3d9.lib")
extern "C" void* __stdcall Direct3DCreate9(UINT);
// Imported but never called: like the real game, this makes Windows load the d3d9 proxy that loads the probe.
int wmain(){wchar_t never[2];if(GetEnvironmentVariableW(L"COOP_SELFTEST_NEVER",never,2))Direct3DCreate9(32);wchar_t p[MAX_PATH],root[32768];GetEnvironmentVariableW(L"COOP_PROBE_ROOT",root,32768);if(FAILED(SHGetFolderPathW(0,CSIDL_PERSONAL,0,0,p))||wcsncmp(p,root,wcslen(root)))return 10;
    wchar_t path[MAX_PATH];swprintf_s(path,L"%s\\selftest.txt",p);HANDLE f=CreateFileW(path,GENERIC_WRITE,0,0,CREATE_ALWAYS,0,0);if(f==INVALID_HANDLE_VALUE)return 11;DWORD done;bool wrote=WriteFile(f,"sandbox only\n",13,&done,0)!=0;CloseHandle(f);if(!wrote||done!=13)return 12;
    HKEY k=0;if(RegCreateKeyW(HKEY_CURRENT_USER,L"Software\\CoopAnythingOwnSelfTest",&k))return 13;DWORD v=42;if(RegSetValueExW(k,L"Example",0,REG_DWORD,(BYTE*)&v,4))return 14;DWORD n=4,type=0,out=0;if(RegQueryValueExW(k,L"Example",0,&type,(BYTE*)&out,&n)||out!=42||type!=REG_DWORD)return 15;RegCloseKey(k);
    // Launcher proved the containing analysis directory writable with a separate
    // new control file. This path is outside both sandbox and selftest gameRoot.
    wchar_t denied[32768];if(!GetEnvironmentVariableW(L"COOP_PROBE_DENIED_WRITE",denied,32768)||GetFileAttributesW(denied)!=INVALID_FILE_ATTRIBUTES)return 16;f=CreateFileW(denied,GENERIC_WRITE,0,0,CREATE_NEW,0,0);if(f!=INVALID_HANDLE_VALUE){CloseHandle(f);return 17;}if(GetLastError()!=ERROR_ACCESS_DENIED||GetFileAttributesW(denied)!=INVALID_FILE_ATTRIBUTES)return 18;
    printf("OWN_SELFTEST_PASS folders/files/virtual-registry/blocked-write\n");return 0;
}
