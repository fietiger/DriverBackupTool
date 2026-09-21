#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "ole32.lib")

#define ID_BTN_BROWSE       1001
#define ID_BTN_BACKUP       1002
#define ID_BTN_SCAN_PROBLEM 1003
#define ID_BTN_SMART_INSTALL 1004
#define ID_EDIT_PATH        1005
#define ID_LIST_DEVICES     1006
#define ID_EDIT_LOG         1007
#define ID_PROGRESS         1008
#define ID_STATUS           1009

typedef struct {
    wchar_t name[256];
    wchar_t hardwareId[512];
    wchar_t matchedInf[MAX_PATH];
    DWORD problemCode;
    DWORD status;
} ProblemDevice;

ProblemDevice g_problemDevices[128];
int g_problemCount = 0;

HWND g_hMainWnd = NULL;
HWND g_hEditPath = NULL;
HWND g_hListDevices = NULL;
HWND g_hEditLog = NULL;
HWND g_hBtnBackup = NULL;
HWND g_hBtnScan = NULL;
HWND g_hBtnSmartInstall = NULL;
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
    EnableWindow(g_hBtnScan, !working);
    EnableWindow(g_hBtnSmartInstall, !working);
    EnableWindow(g_hBtnBrowse, !working);
    EnableWindow(g_hEditPath, !working);
    if (working) {
        SendMessageW(g_hProgressBar, PBM_SETMARQUEE, TRUE, 30);
    } else {
        SendMessageW(g_hProgressBar, PBM_SETMARQUEE, FALSE, 0);
        SendMessageW(g_hProgressBar, PBM_SETPOS, 0, 0);
    }
}

DWORD RunProcessWithPipe(const wchar_t* cmd) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return 1;
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
        return 1;
    }
    CloseHandle(hWrite);

    char buffer[512];
    DWORD bytesRead;
    while (ReadFile(hRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
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

// Find if an INF file contains the specified Hardware ID
bool InfContainsHardwareId(const wchar_t* infPath, const wchar_t* hwId) {
    FILE* fp = _wfopen(infPath, L"rb");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz <= 0 || sz > 15 * 1024 * 1024) { // Ignore >15MB
        fclose(fp);
        return false;
    }

    char* buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(fp); return false; }

    fread(buf, 1, sz, fp);
    buf[sz] = '\0';
    fclose(fp);

    // Convert hwId to uppercase ASCII
    char asciiHwId[512];
    int len = wcstombs(asciiHwId, hwId, 511);
    asciiHwId[len > 0 ? len : 0] = '\0';
    for (int i = 0; asciiHwId[i]; i++) {
        if (asciiHwId[i] >= 'a' && asciiHwId[i] <= 'z') asciiHwId[i] -= 32;
    }

    // Convert buf to uppercase for case-insensitive match
    for (long i = 0; i < sz; i++) {
        if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
    }

    bool matched = false;
    // Extract PCI/USB specific IDs (e.g. VEN_xxxx&DEV_xxxx or VID_xxxx&PID_xxxx)
    char* devPart = strstr(asciiHwId, "DEV_");
    char* venPart = strstr(asciiHwId, "VEN_");
    if (venPart && devPart) {
        char key[64] = {0};
        char ven[16] = {0}, dev[16] = {0};
        strncpy(ven, venPart, 8); // VEN_xxxx
        strncpy(dev, devPart, 8); // DEV_xxxx
        if (strstr(buf, ven) && strstr(buf, dev)) {
            matched = true;
        }
    } else if (strlen(asciiHwId) > 8) {
        if (strstr(buf, asciiHwId)) matched = true;
    }

    free(buf);
    return matched;
}

// Recursively search for matching INF in directory
bool SearchMatchingInf(const wchar_t* dir, const wchar_t* hwId, wchar_t* outInfPath) {
    wchar_t searchPath[MAX_PATH];
    swprintf(searchPath, MAX_PATH, L"%s\\*.*", dir);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return false;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t fullPath[MAX_PATH];
        swprintf(fullPath, MAX_PATH, L"%s\\%s", dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (SearchMatchingInf(fullPath, hwId, outInfPath)) {
                FindClose(hFind);
                return true;
            }
        } else {
            wchar_t* ext = wcsrchr(fd.cFileName, L'.');
            if (ext && _wcsicmp(ext, L".inf") == 0) {
                if (InfContainsHardwareId(fullPath, hwId)) {
                    wcsncpy(outInfPath, fullPath, MAX_PATH - 1);
                    FindClose(hFind);
                    return true;
                }
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return false;
}

// Scan missing or problem devices using SetupAPI and Config Manager
void ScanMissingDevices(const wchar_t* backupDir) {
    g_problemCount = 0;
    ListView_DeleteAllItems(g_hListDevices);

    HDEVINFO hDevInfo = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (hDevInfo == INVALID_HANDLE_VALUE) {
        AppendLog(L"[错误] 无法获取系统设备列表。\r\n");
        return;
    }

    SP_DEVINFO_DATA did;
    did.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &did); i++) {
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, did.DevInst, 0) == CR_SUCCESS) {
            // Check if device has problem or missing driver (DN_HAS_PROBLEM)
            if (status & DN_HAS_PROBLEM) {
                if (g_problemCount >= 128) break;

                ProblemDevice* dev = &g_problemDevices[g_problemCount];
                dev->status = status;
                dev->problemCode = problem;

                // 1. Get Friendly Name or Device Desc
                wchar_t desc[256] = {0};
                if (!SetupDiGetDeviceRegistryPropertyW(hDevInfo, &did, SPDRP_FRIENDLYNAME, NULL, (PBYTE)desc, sizeof(desc), NULL)) {
                    SetupDiGetDeviceRegistryPropertyW(hDevInfo, &did, SPDRP_DEVICEDESC, NULL, (PBYTE)desc, sizeof(desc), NULL);
                }
                if (wcslen(desc) == 0) wcscpy(desc, L"未知异常设备");
                wcsncpy(dev->name, desc, 255);

                // 2. Get Hardware IDs
                wchar_t hwId[512] = {0};
                SetupDiGetDeviceRegistryPropertyW(hDevInfo, &did, SPDRP_HARDWAREID, NULL, (PBYTE)hwId, sizeof(hwId), NULL);
                wcsncpy(dev->hardwareId, hwId, 511);

                // 3. Match against backup directory
                dev->matchedInf[0] = L'\0';
                if (backupDir && wcslen(backupDir) > 0 && wcslen(dev->hardwareId) > 0) {
                    SearchMatchingInf(backupDir, dev->hardwareId, dev->matchedInf);
                }

                // Add to List View
                LVITEMW lvi;
                ZeroMemory(&lvi, sizeof(lvi));
                lvi.mask = LVIF_TEXT;
                lvi.iItem = g_problemCount;
                lvi.iSubItem = 0;
                lvi.pszText = dev->name;
                ListView_InsertItem(g_hListDevices, &lvi);

                // Col 1: Hardware ID snippet
                ListView_SetItemText(g_hListDevices, g_problemCount, 1, dev->hardwareId);

                // Col 2: Problem status
                wchar_t probStr[64];
                swprintf(probStr, 64, L"未驱动 (代码 %lu)", problem);
                ListView_SetItemText(g_hListDevices, g_problemCount, 2, probStr);

                // Col 3: Matched Driver
                if (wcslen(dev->matchedInf) > 0) {
                    wchar_t* fname = wcsrchr(dev->matchedInf, L'\\');
                    ListView_SetItemText(g_hListDevices, g_problemCount, 3, fname ? fname + 1 : dev->matchedInf);
                } else {
                    ListView_SetItemText(g_hListDevices, g_problemCount, 3, L"未找到匹配驱动");
                }

                g_problemCount++;
            }
        }
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);

    wchar_t statusMsg[128];
    swprintf(statusMsg, 128, L"扫描完成: 发现 %d 个缺失驱动或异常的设备", g_problemCount);
    SetWindowTextW(g_hStatus, statusMsg);

    AppendLog(L"\r\n--- 扫描缺失驱动设备结果 ---\r\n");
    for (int j = 0; j < g_problemCount; j++) {
        AppendLog(L"• [异常硬件] ");
        AppendLog(g_problemDevices[j].name);
        AppendLog(L"\r\n  硬件ID: ");
        AppendLog(g_problemDevices[j].hardwareId);
        AppendLog(L"\r\n  匹配结果: ");
        if (wcslen(g_problemDevices[j].matchedInf) > 0) {
            AppendLog(L"✅ 已匹配到 -> ");
            AppendLog(g_problemDevices[j].matchedInf);
        } else {
            AppendLog(L"❌ 备份库中暂无匹配项");
        }
        AppendLog(L"\r\n");
    }
}

DWORD WINAPI SmartInstallThread(LPVOID lpParam) {
    wchar_t* targetPath = (wchar_t*)lpParam;
    AppendLog(L"\r\n========================================\r\n");
    AppendLog(L"【智能定向安装模式启动】\r\n");
    AppendLog(L"开始针对列表中的未驱动硬件按顺序精确安装...\r\n");
    AppendLog(L"========================================\r\n");

    int installedCount = 0;
    for (int i = 0; i < g_problemCount; i++) {
        if (wcslen(g_problemDevices[i].matchedInf) > 0) {
            AppendLog(L"\r\n[正在安装] ");
            AppendLog(g_problemDevices[i].name);
            AppendLog(L"\r\n驱动文件: ");
            AppendLog(g_problemDevices[i].matchedInf);
            AppendLog(L"\r\n");

            wchar_t cmd[2048];
            swprintf(cmd, 2048, L"pnputil.exe /add-driver \"%s\" /install", g_problemDevices[i].matchedInf);
            RunProcessWithPipe(cmd);
            installedCount++;
            Sleep(500);
        }
    }

    AppendLog(L"\r\n========================================\r\n");
    wchar_t summary[128];
    swprintf(summary, 128, L"智能安装流程执行完毕！已定向注入 %d 个硬件匹配驱动。\r\n", installedCount);
    AppendLog(summary);
    AppendLog(L"正在重新扫描刷新硬件状态...\r\n");
    AppendLog(L"========================================\r\n");

    ScanMissingDevices(targetPath);

    free(targetPath);
    SetUIState(false);
    return 0;
}

DWORD WINAPI BackupThread(LPVOID lpParam) {
    wchar_t* targetPath = (wchar_t*)lpParam;
    AppendLog(L"\r\n========================================\r\n");
    AppendLog(L"【开始执行驱动完整备份】\r\n");
    AppendLog(L"目标目录: ");
    AppendLog(targetPath);
    AppendLog(L"\r\n调用 Windows 原生 DISM 驱动导出引擎...\r\n");
    AppendLog(L"========================================\r\n");

    CreateDirectoryW(targetPath, NULL);
    wchar_t cmd[2048];
    swprintf(cmd, 2048, L"dism.exe /online /export-driver /destination:\"%s\"", targetPath);

    DWORD ec = RunProcessWithPipe(cmd);
    if (ec == 0) {
        AppendLog(L"\r\n[成功] 当前系统的全部第三方硬件驱动已完整导出！\r\n");
        SetWindowTextW(g_hStatus, L"驱动备份完成！");
        MessageBoxW(g_hMainWnd, L"所有第三方硬件驱动已成功导出并备份完毕！", L"备份成功", MB_OK | MB_ICONINFORMATION);
    } else {
        SetWindowTextW(g_hStatus, L"备份失败，请检查管理员权限！");
        MessageBoxW(g_hMainWnd, L"备份执行遇到错误，请确认是否以管理员身份运行此程序！", L"执行异常", MB_OK | MB_ICONERROR);
    }

    free(targetPath);
    SetUIState(false);
    return 0;
}

void SelectFolder() {
    BROWSEINFOW bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = g_hMainWnd;
    bi.lpszTitle = L"请选择驱动备份所在的文件夹:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) {
            SetWindowTextW(g_hEditPath, path);
            ScanMissingDevices(path);
        }
        CoTaskMemFree(pidl);
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        HFONT hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        // Top path row
        HWND hLbl = CreateWindowW(L"STATIC", L"驱动库文件夹:", WS_CHILD | WS_VISIBLE,
            20, 18, 110, 24, hWnd, NULL, NULL, NULL);
        SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hEditPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            130, 16, 460, 26, hWnd, (HMENU)ID_EDIT_PATH, NULL, NULL);
        SendMessageW(g_hEditPath, WM_SETFONT, (WPARAM)hFont, TRUE);

        wchar_t defaultPath[MAX_PATH];
        GetModuleFileNameW(NULL, defaultPath, MAX_PATH);
        wchar_t* lastSlash = wcsrchr(defaultPath, L'\\');
        if (lastSlash) *(lastSlash + 1) = L'\0';
        wcscat(defaultPath, L"DriversBackup");
        SetWindowTextW(g_hEditPath, defaultPath);

        g_hBtnBrowse = CreateWindowW(L"BUTTON", L"选择目录...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            600, 15, 95, 28, hWnd, (HMENU)ID_BTN_BROWSE, NULL, NULL);
        SendMessageW(g_hBtnBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Buttons row
        g_hBtnScan = CreateWindowW(L"BUTTON", L"🔍 扫描未驱动设备", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            20, 52, 170, 32, hWnd, (HMENU)ID_BTN_SCAN_PROBLEM, NULL, NULL);
        SendMessageW(g_hBtnScan, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hBtnSmartInstall = CreateWindowW(L"BUTTON", L"⚡ 智能按序自动安装", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            200, 52, 180, 32, hWnd, (HMENU)ID_BTN_SMART_INSTALL, NULL, NULL);
        SendMessageW(g_hBtnSmartInstall, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hBtnBackup = CreateWindowW(L"BUTTON", L"💾 备份当前系统所有驱动", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            495, 52, 200, 32, hWnd, (HMENU)ID_BTN_BACKUP, NULL, NULL);
        SendMessageW(g_hBtnBackup, WM_SETFONT, (WPARAM)hFont, TRUE);

        // List View for missing devices
        g_hListDevices = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
            20, 92, 675, 170, hWnd, (HMENU)ID_LIST_DEVICES, NULL, NULL);
        ListView_SetExtendedListViewStyle(g_hListDevices, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        SendMessageW(g_hListDevices, WM_SETFONT, (WPARAM)hFont, TRUE);

        LVCOLUMNW lvc;
        lvc.mask = LVCF_TEXT | LVCF_WIDTH;
        lvc.cx = 180; lvc.pszText = L"未驱动设备名称";
        ListView_InsertColumn(g_hListDevices, 0, &lvc);
        lvc.cx = 200; lvc.pszText = L"硬件 ID (Hardware ID)";
        ListView_InsertColumn(g_hListDevices, 1, &lvc);
        lvc.cx = 120; lvc.pszText = L"状态";
        ListView_InsertColumn(g_hListDevices, 2, &lvc);
        lvc.cx = 150; lvc.pszText = L"备份库智能匹配结果";
        ListView_InsertColumn(g_hListDevices, 3, &lvc);

        // Progress bar
        g_hProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
            20, 268, 675, 12, hWnd, (HMENU)ID_PROGRESS, NULL, NULL);

        // Log edit control
        g_hEditLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            20, 286, 675, 195, hWnd, (HMENU)ID_EDIT_LOG, NULL, NULL);
        SendMessageW(g_hEditLog, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Status bar
        g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"就绪 - 点击【扫描未驱动设备】自动检测黄色感叹号硬件",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hWnd, (HMENU)ID_STATUS, NULL, NULL);
        SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

        AppendLog(L"欢迎使用 Windows 智能驱动匹配与按序自动安装工具。\r\n");
        AppendLog(L"无需逐个去设备管理器更新！程序自动检测所有黄色感叹号设备，并在备份库中精确匹配 INF 驱动安装。\r\n");
        break;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == ID_BTN_BROWSE) {
            SelectFolder();
        } else if (wmId == ID_BTN_SCAN_PROBLEM) {
            wchar_t path[MAX_PATH];
            GetWindowTextW(g_hEditPath, path, MAX_PATH);
            ScanMissingDevices(path);
        } else if (wmId == ID_BTN_SMART_INSTALL) {
            wchar_t path[MAX_PATH];
            GetWindowTextW(g_hEditPath, path, MAX_PATH);
            if (g_problemCount == 0) {
                MessageBoxW(hWnd, L"当前没有扫描到缺失驱动的设备，或者请先点击【扫描未驱动设备】！", L"提示", MB_OK | MB_ICONINFORMATION);
                break;
            }
            SetUIState(true);
            SetWindowTextW(g_hStatus, L"正在智能匹配并按序自动安装驱动...");
            wchar_t* pCopy = _wcsdup(path);
            g_hWorkerThread = CreateThread(NULL, 0, SmartInstallThread, pCopy, 0, NULL);
        } else if (wmId == ID_BTN_BACKUP) {
            wchar_t path[MAX_PATH];
            GetWindowTextW(g_hEditPath, path, MAX_PATH);
            if (wcslen(path) == 0) {
                MessageBoxW(hWnd, L"请指定备份目标路径！", L"提示", MB_OK | MB_ICONWARNING);
                break;
            }
            SetUIState(true);
            SetWindowTextW(g_hStatus, L"正在完整备份当前系统驱动...");
            wchar_t* pCopy = _wcsdup(path);
            g_hWorkerThread = CreateThread(NULL, 0, BackupThread, pCopy, 0, NULL);
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
    icex.dwICC = ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES;
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

    int w = 730;
    int h = 550;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    g_hMainWnd = CreateWindowW(L"DriverBackupToolWndClass", L"Windows 驱动备份与智能匹配按序安装工具 (Win32 原生版)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, w, h, NULL, NULL, hInstance, NULL);

    if (!g_hMainWnd) return FALSE;

    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);

    // Initial auto-scan on startup
    wchar_t defaultPath[MAX_PATH];
    GetWindowTextW(g_hEditPath, defaultPath, MAX_PATH);
    ScanMissingDevices(defaultPath);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
