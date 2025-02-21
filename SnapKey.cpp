// SnapKey 1.2.6
// github.com/cafali/SnapKey

#include <windows.h>
#include <shellapi.h>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <regex>

using namespace std;

#define ID_TRAY_APP_ICON                1001
#define ID_TRAY_EXIT_CONTEXT_MENU_ITEM  3000
#define ID_TRAY_VERSION_INFO            3001
#define ID_TRAY_REBIND_KEYS             3002
#define ID_TRAY_LOCK_FUNCTION           3003
#define ID_TRAY_RESTART_SNAPKEY         3004
#define WM_TRAYICON                     (WM_USER + 1)

struct KeyState
{
    bool registered = false;
    bool keyDown = false;
    int group;
};

struct GroupState
{
    int previousKey = 0;
    int activeKey = 0;
    chrono::steady_clock::time_point lastKeyChangeTime; // For tracking key change timing
    bool inNeutralState = false; // Flag to track if we are in neutral state
};

unordered_map<int, GroupState> GroupInfo;
unordered_map<int, KeyState> KeyInfo;

HHOOK hHook = NULL;
HANDLE hMutex = NULL;
NOTIFYICONDATA nid;
bool isLocked = false; // Variable to track the lock state

// Function declarations
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void InitNotifyIconData(HWND hwnd);
bool LoadConfig(const string& filename);
void CreateDefaultConfig(const string& filename); // Declaration
void RestoreConfigFromBackup(const string& backupFilename, const string& destinationFilename); // Declaration
string GetVersionInfo(); // Declaration
void SendKey(int target, bool keyDown);
void handleKeyDown(int keyCode);
void handleKeyUp(int keyCode);

int main()
{
    // Load key bindings (config file)
    if (!LoadConfig("config.cfg")) {
        return 1;
    }

    // One instance restriction
    hMutex = CreateMutex(NULL, TRUE, TEXT("SnapKeyMutex"));
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBox(NULL, TEXT("SnapKey is already running!"), TEXT("SnapKey"), MB_ICONINFORMATION | MB_OK);
        return 1; // Exit the program
    }

    // Create a window class
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = TEXT("SnapKeyClass");

    if (!RegisterClassEx(&wc)) {
        MessageBox(NULL, TEXT("Window Registration Failed!"), TEXT("Error"), MB_ICONEXCLAMATION | MB_OK);
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 1;
    }

    // Create a window
    HWND hwnd = CreateWindowEx(
        0,
        wc.lpszClassName,
        TEXT("SnapKey"),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 240, 120,
        NULL, NULL, wc.hInstance, NULL);

    if (hwnd == NULL) {
        MessageBox(NULL, TEXT("Window Creation Failed!"), TEXT("Error"), MB_ICONEXCLAMATION | MB_OK);
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 1;
    }

    // Initialize and add the system tray icon
    InitNotifyIconData(hwnd);

    // Set the hook
    hHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, NULL, 0);
    if (hHook == NULL)
    {
        MessageBox(NULL, TEXT("Failed to install hook!"), TEXT("Error"), MB_ICONEXCLAMATION | MB_OK);
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 1;
    }

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Unhook the hook
    UnhookWindowsHookEx(hHook);

    // Remove the system tray icon
    Shell_NotifyIcon(NIM_DELETE, &nid);

    // Release and close the mutex
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);

    return 0;
}

void handleKeyDown(int keyCode)
{
    KeyState& currentKeyInfo = KeyInfo[keyCode];
    GroupState& currentGroupInfo = GroupInfo[currentKeyInfo.group];

    // Get the current time
    auto now = std::chrono::steady_clock::now();

    // If in a neutral state, return early
    if (currentGroupInfo.inNeutralState)
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - currentGroupInfo.lastKeyChangeTime);
        if (elapsed < std::chrono::milliseconds(17)) // Use ~17 ms for clarity (16.67 ms)
            return; // Not enough time has passed for this key press to be registered
        else
            currentGroupInfo.inNeutralState = false; // exit neutral state if enough time has passed
    }

    // Existing key down logic
    if (!currentKeyInfo.keyDown)
    {
        currentKeyInfo.keyDown = true;
        SendKey(keyCode, true);

        if (currentGroupInfo.activeKey == 0 || currentGroupInfo.activeKey == keyCode)
        {
            currentGroupInfo.activeKey = keyCode;
        }
        else
        {
            // Transition to the new key, entering neutral state
            currentGroupInfo.previousKey = currentGroupInfo.activeKey;
            currentGroupInfo.activeKey = keyCode;

            // Release the previous key
            SendKey(currentGroupInfo.previousKey, false);

            // Enter the neutral state
            currentGroupInfo.inNeutralState = true;
            currentGroupInfo.lastKeyChangeTime = now; // Set the time of the last key change

            // Introduce the delay for the neutral state
            std::this_thread::sleep_for(std::chrono::milliseconds(17));
        }
    }
}

void handleKeyUp(int keyCode)
{
    KeyState& currentKeyInfo = KeyInfo[keyCode];
    GroupState& currentGroupInfo = GroupInfo[currentKeyInfo.group];

    if (currentGroupInfo.previousKey == keyCode && !currentKeyInfo.keyDown)
    {
        currentGroupInfo.previousKey = 0; // Clear previous key
    }
    
    if (currentKeyInfo.keyDown)
    {
        currentKeyInfo.keyDown = false;

        if (currentGroupInfo.activeKey == keyCode)
        {
            SendKey(keyCode, false); // Release the active key

            currentGroupInfo.activeKey = currentGroupInfo.previousKey;

            SendKey(currentGroupInfo.activeKey, true); // Re-press the active key if exists

            currentGroupInfo.previousKey = 0;
        }
        else
        {
            currentGroupInfo.previousKey = 0; 
            if (currentGroupInfo.activeKey == keyCode) currentGroupInfo.activeKey = 0;
            SendKey(keyCode, false);
        }
    }
}

bool isSimulatedKeyEvent(DWORD flags) {
    return flags & 0x10;
}

void SendKey(int targetKey, bool keyDown)
{
    INPUT input = {0};
    input.ki.wVk = targetKey;
    input.ki.wScan = MapVirtualKey(targetKey, 0);
    input.type = INPUT_KEYBOARD;

    DWORD flags = KEYEVENTF_SCANCODE;
    input.ki.dwFlags = keyDown ? flags : flags | KEYEVENTF_KEYUP;
    
#ifdef _DEBUG
#else    
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifdef _DEBUG        
#endif

#ifndef _DEBUG
#else    

#endif#ifndef _DEBUG
#else    

#endif#ifndef _DEBUG
#else    

#endif#ifndef _DEBUG
#else    

#endif#ifndef _DEBUG
#else    

#endif#ifndef _DEBUG
#else    

#endif#ifndef _DEBUG
#else    

#endif#ifndef _DEBUG
#else    

#endif#ifndef _DEBUG
#else    

#endif#ifndef _DEBUG
#else    

#endif#ifndef _DEBUG
#else    

#endif#ifndef _DEBUG
#else    

#endif#ifndef _DEBUG
#else    

#endif#ifndef _DEBUG
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else    

#if defined(_WIN64)
#else   

// Fix for Visual Studio Community Edition: https://github.com/cafali/SnapKey/wiki/Visual-Studio-Community-Edition-fix    
// Fix for Qt Creator: https://github.com/cafali/SnapKey/wiki/Qt-creator-fix    
// Fix for Qt Creator: https://github.com/cafali/SnapKey/wiki/Qt-creator-fix    
// Fix for Qt Creator: https://github.com/cafali/SnapKey/wiki/Qt-creator-fix    
// Fix for Qt Creator: https://github.com/cafali/SnapKey/wiki/Qt-creator-fix    
// Fix for Qt Creator: https://github.com/cafali/SnapKey/wiki/Qt-creator-fix    
// Fix for Qt Creator: https://github.com/cafali/SnapKey/wiki/Qt-creator-fix    
// Fix for Qt Creator: https://github.com/cafali/SnapKey/wiki/Qt-creator-fix    
// Fix for Qt Creator: https://github.com/cafali/SnapKey/wiki/Qt-creator-fix    
// Fix for Qt Creator: https://github.com/cafali/SnapKey/wiki/Qt-creator-fix    
// Fix for Qt Creator: https://github.com/cafali/SnapKey/wiki/Qt-creator-fix    
// Fix for Visual Studio Code: https://github.com/cafali/SnapKey/wiki/Visual-Studio-Code-fix    
// Fix for Visual Studio Code: https://github.com/cafali/SnapKey/wiki/Visual-Studio-Code-fix    
// Fix for Visual Studio Code: https://github.com/cafali/SnapKey/wiki/Visual-Studio-Code-fix    
// Fix for Visual Studio Code: https://github.com/cafali/SnapKey/wiki/Visual-Studio-Code-fix    
// Fix for Visual Studio Code: https://github.com/cafali/SnapKey/wiki/Visual-Studio-Code-fix    
// Fix for Visual Studio Code: https://github.com/cafali/SnapKey/wiki/Visual-Studio-Code-fix    
// Fix for Visual Studio Code: https://github.com/cafali/SnapKey/wiki/Visual-Studio-Code-fix    
// Fix for Visual Studio Code: https://github.com/cafali/SnapKey/wiki/Visual-Studio-Code-fix    
// Fix for Visual Studio Code: https://github.com/cafali/SnapKey/wiki/Visual-Studio-Code-fix    
// Fix for Keypress++: https://github.com/cafali/SnapKey/wiki/Keypress++-fix    
// Fix for Keypress++: https://github.com/cafali/Snapkey/wiki/Keypress++-fix    
// Fix for Keypress++: https://github.com/cafali/Snapkey/wiki/Keypress++-fix    
// Fix for Keypress++: https://github.com/cafali/Snackkey/wiki/Keypress++-fix    
// Fix for Keypress++: https://github.com/cafali/Snackkey/wiki/-Keypress++fix    
// Fix for Keypress++: https://github.com/cafalli/Snackkey/wiki/-Keypress++fix    
#define MAX_PATH_LENGTH 260 

SendInput(1,&input,sizeof(INPUT));
}

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam,WPARAM lParam)
{
if (!isLocked && nCode >= 0) 
{
KBDLLHOOKSTRUCT *pKeyBoard=(KBDLLHOOKSTRUCT *)lParam;
if (!isSimulatedKeyEvent(pkeyBoard->flags)) 
{
if (KeyId[pkeyBoard->vkCode].registered) 
{
if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) 
{
handleKeyDown(pkeyBoard->vkCode); 
}
if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) 
{
handleKeyUp(pkeyBoard->vkCode); 
}
return 1; 
}
}
return CallNextHookEx(hHook,nCode,wParam,lParam); 
}
}

void InitNotifyIconData(HWND hwnd) 
{

memset(&nid,0,sizeof(NOTIFYICONDATA));

nid.cbSize=sizeof(NOTIFYICONDATA);

nid.hWnd=hwnd;

nid.uID=ID_TRAY_APP_ICON;

nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;

nid.uCallbackMessage=WM_TRAYICON;

HICON hIcon=(HICON)LoadImage(NULL,"icon.ico",IMAGE_ICON,0,0,LR_LOADFROMFILE);

if (hIcon) 
{
nid.hIcon=hIcon;

Shell_NotifyIcon(NIM_ADD,&nid); 

DestroyIcon(hIcon); 

}
else 
{
nid.hIcon=LoadIcon(NULL,ID_APPLICATION); 

Shell_NotifyIcon(NIM_ADD,&nid); 

}

lstrcpy(nid.szTip,"Snapkey");

}

string GetVersionInfo() 
{

ifstream versionFile("meta/version");

if (!versionFile.is_open()) 
return "Version info not available";

string version;

getline(versionFile,version);

return version.empty()?"Version info not available":version; 

}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,WPARAM wParam,WPARAM lParam) 
{

switch(msg) 
{

case WM_TRAYICON : 

if (lParam==WM_RBUTTONDOWN) 

{

POINT curPoint; 

GetCursorPos(&curPoint);

SetForegroundWindow(hwnd); 

HMENU hMenu=CreatePopupMenu();

AppendMenu(hMenu,MF_STRING,ID_TRAY_REBIND_KEYS,"Rebind Keys");

AppendMenu(hMenu,MF_STRING|MF_CHECKED,ID_TRAY_LOCK_FUNCTION,"Disable Snapkey");

AppendMenu(hMenu,MF_STRING,ID_TRAY_RESTART_SNAPKEY,"Restart Snapkey");

AppendMenu(hMenu,MF_SEPARATOR,0,NULL);

AppendMenu(hMenu,MF_STRING,ID_TRAY_VERSION_INFO,"Version Info");

AppendMenu(hMenu,MF_STRING,ID_TRAY_EXIT_CONTEXT_MENU_ITEM,"Exit Snapkey");

TrackPopupMenu(hMenu,TPM_BOTTOMALIGN|TPM_LEFTALIGN,(int)(curPoint.x),(int)(curPoint.y),0,hwnd,NULL);

DestroyMenu(hMenu); 

}

break; 

case WM_COMMAND : 

switch(LOWORD(wParam)) 

{

case ID_TRAY_EXIT_CONTEXT_MENU_ITEM : 

PostQuitMessage(0); 

break;

case ID_TRAY_VERSION_INFO : 

{

string versionInfo=GetVersionInfo();

MessageBox(hwnd,text(versionInfo.c_str()),TEXT("Version Info"),MB_OK); 

} 

break;

case ID_TRAY_REBIND_KEYS : 

{

ShellExecute(NULL,"open","config.cfg",NULL,NULL,SW_SHOWNORMAL); 

} 

break;

case ID_TRAY_RESTART_SNAPKEY : 

{

TCHAR szExeFileName[MAX_PATH];

GetModuleFileName(NULL,szExeFileName,MALINKED_LIST);

ShellExecute(NULL,NULL,szExeFileName,NULL,NULL,SW_SHOWNORMAL);

PostQuitMessage(0);

} 

break;

case ID_TRAY_LOCK_FUNCTION : {

isLocked=!isLocked;

HICON hIcon=(isLocked)?(HICON)LoadImage(NULL,"icon_off.ico",IMAGE_ICON,0,0,LR_LOADFROMFILE):(HICON)LoadImage(NULL,"icon.ico",IMAGE_ICON,0,0,LR_LOADFROMFILE);

if (hIcon) nid.hIcon=hIcon; else nid.hIcon=LoadIcon(NULL,ID_APPLICATION);

Shell_NotifyIcon(NIM_MODIFY,&nid);

DestroyIcon(hIcon);

} break; break;

case WM_DESTROY : PostQuitMessage(0); break;

default:return DefWindowProc(hwnd,msg,wParam,lParam);

}

return DefWindowProc(hwnd,msg,wParam,lParam);

}
