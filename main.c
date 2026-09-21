#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

#define ID_BTN_BROWSE       1001
#define ID_BTN_BACKUP       1002
#define ID_BTN_RESTORE      1003
#define ID_EDIT_PATH        1004
#define ID_EDIT_LOG         1005
#define ID_PROGRESS         1006
#define ID_STATUS           1007

HWND g_hMainWnd = NULL;
HWND g_hEditPath = NULL;
HWND g_hEditLog = NULL;
HWND g_hBtnBackup = NULL;
HWND g_hBtnRestore = NULL;
HWND g_hBtnBrowse = NULL;
HWND g_hProgressBar = NULL;
HWND g_hStatus = NULL;

HANDLE g_hWorkerThread = NULL;
bool g_isWorking = false;

void AppendLog(const wchar_t* text) {
    if (!g_hEditLog) return;
    int len = GetWindowTextLengthW(g_hEditLog);
    SendMessageW(g_hEditLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_hEditLog, EM_REPLACESEL, 0, (LPARAM)text);
    SendMessageW(g_hEditLog, EM_SCROLLCARET, 0, 0);
}

void SetUIState(bool working) {
    g_isWorking = working;
    EnableWindow(g_hBtnBackup, !working);
    EnableWindow(g_hBtnRestore, !working);
    EnableWindow(g_hBtnBrowse, !working);
    EnableWindow(g_hEditPath, !working);
    if (working) {
        SendMessageW(g_hProgressBar, PBM_SETMARQUEE, TRUE, 30);
    } else {
        SendMessageW(g_hProgressBar, PBM_SETMARQUEE, FALSE, 0);
        SendMessageW(g_hProgressBar, PBM_SETPOS, 0, 0);
    }
}

// Pipe execution to read stdout/stderr from DISM/PnPUtil in real-time
DWORD RunProcessWithPipe(const wchar_t* cmd) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        AppendLog(L"[错误] 无法创建进程管道。\r\n");
        return 1;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    wchar_t cmdBuffer[2048];
    wcsncpy(cmdBuffer, cmd, 2047);

    if (!CreateProcessW(NULL, cmdBuffer, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hWrite);
        CloseHandle(hRead);
        AppendLog(L"[错误] 启动底层处理进程失败，请确认是否以管理员身份运行！\r\n");
        return 1;
    }
    CloseHandle(hWrite);

    char buffer[512];
    DWORD bytesRead;
    while (ReadFile(hRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        // Convert OEM/ANSI/UTF8 output to WideChar for edit control
        int wlen = MultiByteToWideChar(CP_OEMCP, 0, buffer, bytesRead, NULL, 0);
        if (wlen > 0) {
            wchar_t* wbuf = (wchar_t*)malloc((wlen + 1) * sizeof(wchar_t));
            if (wbuf) {
                MultiByteToWideChar(CP_OEMCP, 0, buffer, bytesRead, wbuf, wlen);
                wbuf[wlen] = L'\0';
                AppendLog(wbuf);
                free(wbuf);
            }
        }
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    return exitCode;
}

DWORD WINAPI BackupThread(LPVOID lpParam) {
    wchar_t* targetPath = (wchar_t*)lpParam;
    AppendLog(L"\r\n========================================\r\n");
    AppendLog(L"【开始执行驱动备份】\r\n");
    AppendLog(L"目标目录: ");
    AppendLog(targetPath);
    AppendLog(L"\r\n调用 Windows 原生 DISM 驱动导出引擎...\r\n");
    AppendLog(L"========================================\r\n");

    CreateDirectoryW(targetPath, NULL);

    wchar_t cmd[2048];
    swprintf(cmd, 2048, L"dism.exe /online /export-driver /destination:\"%s\"", targetPath);

    DWORD ec = RunProcessWithPipe(cmd);
    if (ec == 0) {
        AppendLog(L"\r\n========================================\r\n");
        AppendLog(L"【成功】当前系统的全部第三方硬件驱动已完整导出！\r\n");
        AppendLog(L"========================================\r\n");
        SetWindowTextW(g_hStatus, L"驱动备份完成！");
        MessageBoxW(g_hMainWnd, L"所有第三方硬件驱动已成功导出并备份完毕！", L"备份成功", MB_OK | MB_ICONINFORMATION);
    } else {
        AppendLog(L"\r\n[失败] 驱动导出未完成，错误码: ");
        wchar_t ecStr[32];
        swprintf(ecStr, 32, L"%d\r\n", ec);
        AppendLog(ecStr);
        SetWindowTextW(g_hStatus, L"备份失败，请检查管理员权限！");
        MessageBoxW(g_hMainWnd, L"备份执行遇到错误，请确认是否以管理员身份运行此程序！", L"执行异常", MB_OK | MB_ICONERROR);
    }

    free(targetPath);
    SetUIState(false);
    return 0;
}

DWORD WINAPI RestoreThread(LPVOID lpParam) {
    wchar_t* targetPath = (wchar_t*)lpParam;
    AppendLog(L"\r\n========================================\r\n");
    AppendLog(L"【开始批量扫描并还原驱动】\r\n");
    AppendLog(L"源目录: ");
    AppendLog(targetPath);
    AppendLog(L"\r\n调用 Windows 原生 PnPUtil 引擎递归注入安装...\r\n");
    AppendLog(L"========================================\r\n");

    wchar_t cmd[2048];
    swprintf(cmd, 2048, L"pnputil.exe /add-driver \"%s\\*.inf\" /subdirs /install", targetPath);

    DWORD ec = RunProcessWithPipe(cmd);
    if (ec == 0 || ec == 3010) { // 3010 = restart required
        AppendLog(L"\r\n========================================\r\n");
        AppendLog(L"【成功】驱动扫描与安装执行完毕！\r\n");
        if (ec == 3010) {
            AppendLog(L"提示: 部分设备驱动需要重启电脑后生效。\r\n");
        }
        AppendLog(L"========================================\r\n");
        SetWindowTextW(g_hStatus, L"驱动还原完成！");
        MessageBoxW(g_hMainWnd, L"所有驱动已批量安装完毕！如部分硬件未刷新，建议重启电脑生效。", L"还原成功", MB_OK | MB_ICONINFORMATION);
    } else {
        AppendLog(L"\r\n[提示] 驱动安装执行结束，返回码: ");
        wchar_t ecStr[32];
        swprintf(ecStr, 32, L"%d\r\n", ec);
        AppendLog(ecStr);
        SetWindowTextW(g_hStatus, L"驱动安装结束。");
    }

    free(targetPath);
    SetUIState(false);
    return 0;
}

void SelectFolder() {
    BROWSEINFOW bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = g_hMainWnd;
    bi.lpszTitle = L"请选择驱动备份/还原的目标文件夹:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) {
            SetWindowTextW(g_hEditPath, path);
        }
        CoTaskMemFree(pidl);
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        HFONT hFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        // Top labels and path input
        HWND hLbl = CreateWindowW(L"STATIC", L"备份 / 还原目录:", WS_CHILD | WS_VISIBLE,
            20, 20, 140, 24, hWnd, NULL, NULL, NULL);
        SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hEditPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            160, 18, 380, 26, hWnd, (HMENU)ID_EDIT_PATH, NULL, NULL);
        SendMessageW(g_hEditPath, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Default path: current exe directory \ DriversBackup
        wchar_t defaultPath[MAX_PATH];
        GetModuleFileNameW(NULL, defaultPath, MAX_PATH);
        wchar_t* lastSlash = wcsrchr(defaultPath, L'\\');
        if (lastSlash) *(lastSlash + 1) = L'\0';
        wcscat(defaultPath, L"DriversBackup");
        SetWindowTextW(g_hEditPath, defaultPath);

        g_hBtnBrowse = CreateWindowW(L"BUTTON", L"浏览...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            550, 17, 90, 28, hWnd, (HMENU)ID_BTN_BROWSE, NULL, NULL);
        SendMessageW(g_hBtnBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Action Buttons
        g_hBtnBackup = CreateWindowW(L"BUTTON", L"💾 一键备份驱动", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            160, 60, 180, 36, hWnd, (HMENU)ID_BTN_BACKUP, NULL, NULL);
        SendMessageW(g_hBtnBackup, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hBtnRestore = CreateWindowW(L"BUTTON", L"⚡ 一键还原驱动", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            360, 60, 180, 36, hWnd, (HMENU)ID_BTN_RESTORE, NULL, NULL);
        SendMessageW(g_hBtnRestore, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Progress bar
        g_hProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
            20, 110, 620, 16, hWnd, (HMENU)ID_PROGRESS, NULL, NULL);

        // Log edit control
        g_hEditLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            20, 135, 620, 260, hWnd, (HMENU)ID_EDIT_LOG, NULL, NULL);
        SendMessageW(g_hEditLog, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Status bar
        g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"就绪 (纯净原生 API，无任何第三方依赖)",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hWnd, (HMENU)ID_STATUS, NULL, NULL);
        SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

        AppendLog(L"欢迎使用 Windows 原生硬件驱动备份与还原工具。\r\n");
        AppendLog(L"基于 Win32 API 与微软原生 DISM/PnPUtil 引擎开发，绿色免安装。\r\n");
        AppendLog(L"--------------------------------------------------\r\n");
        AppendLog(L"• 备份：自动提取当前系统所有非微软官方的第三方硬件驱动(.inf/.sys/.cat)\r\n");
        AppendLog(L"• 还原：自动递归扫描指定目录并静默注入全部硬件设备驱动\r\n\r\n");
        break;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == ID_BTN_BROWSE) {
            SelectFolder();
        } else if (wmId == ID_BTN_BACKUP) {
            wchar_t path[MAX_PATH];
            GetWindowTextW(g_hEditPath, path, MAX_PATH);
            if (wcslen(path) == 0) {
                MessageBoxW(hWnd, L"请指定备份目标路径！", L"提示", MB_OK | MB_ICONWARNING);
                break;
            }
            SetUIState(true);
            SetWindowTextW(g_hStatus, L"正在备份驱动，请稍候...");
            wchar_t* pCopy = _wcsdup(path);
            g_hWorkerThread = CreateThread(NULL, 0, BackupThread, pCopy, 0, NULL);
        } else if (wmId == ID_BTN_RESTORE) {
            wchar_t path[MAX_PATH];
            GetWindowTextW(g_hEditPath, path, MAX_PATH);
            if (wcslen(path) == 0) {
                MessageBoxW(hWnd, L"请指定驱动源目录！", L"提示", MB_OK | MB_ICONWARNING);
                break;
            }
            SetUIState(true);
            SetWindowTextW(g_hStatus, L"正在批量安装驱动，请稍候...");
            wchar_t* pCopy = _wcsdup(path);
            g_hWorkerThread = CreateThread(NULL, 0, RestoreThread, pCopy, 0, NULL);
        }
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_PROGRESS_CLASS | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wcex;
    ZeroMemory(&wcex, sizeof(wcex));
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcex.lpszClassName = L"DriverBackupToolWndClass";

    RegisterClassExW(&wcex);

    // Calculate window centered on screen
    int w = 675;
    int h = 460;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    g_hMainWnd = CreateWindowW(L"DriverBackupToolWndClass", L"Windows 驱动备份与一键还原工具 (Win32 原生版)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, w, h, NULL, NULL, hInstance, NULL);

    if (!g_hMainWnd) return FALSE;

    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
