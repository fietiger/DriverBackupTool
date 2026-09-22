#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

#define ID_BTN_BROWSE       1001
#define ID_BTN_BACKUP       1002
#define ID_BTN_SCAN_PROBLEM 1003
#define ID_BTN_SMART_INSTALL 1004
#define ID_COMBO_PROFILE    1005
#define ID_EDIT_PATH        1006
#define ID_LIST_DEVICES     1007
#define ID_EDIT_LOG         1008
#define ID_PROGRESS         1009
#define ID_STATUS           1010
#define ID_LBL_ESTIMATE     1011

// Backup Depth Profiles
typedef enum {
    PROFILE_MINIMAL = 0,    // 最小救命集 (网卡/芯片组/总线)
    PROFILE_STANDARD = 1,   // 标准装机集 (网卡/主板/声卡/工控设备，不含大独显)
    PROFILE_FULL = 2        // 完整镜像集 (全量第三方驱动)
} BackupProfile;

typedef struct {
    wchar_t oemInf[64];
    wchar_t originalInf[256];
    wchar_t provider[128];
    wchar_t className[64];
    wchar_t devDesc[256];
    wchar_t hardwareId[512];
    DWORD estimatedSizeBytes;
    bool isNetwork;
    bool isChipset;
    bool isDisplay;
    bool isAudio;
    bool isIndustrial;
} DriverPackageInfo;

typedef struct {
    wchar_t name[256];
    wchar_t hardwareId[512];
    wchar_t matchedInf[MAX_PATH];
    DWORD problemCode;
    DWORD status;
} ProblemDevice;

ProblemDevice g_problemDevices[128];
int g_problemCount = 0;

DriverPackageInfo g_driverPackages[512];
int g_driverPackageCount = 0;

HWND g_hMainWnd = NULL;
HWND g_hEditPath = NULL;
HWND g_hComboProfile = NULL;
HWND g_hLblEstimate = NULL;
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
    EnableWindow(g_hComboProfile, !working);
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

// Calculate directory/file size
DWORD GetDriverSizeEstimate(const wchar_t* className, const wchar_t* provider, const wchar_t* desc) {
    if (_wcsicmp(className, L"Display") == 0) {
        if (wcsstr(provider, L"NVIDIA") || wcsstr(desc, L"NVIDIA") || wcsstr(provider, L"AMD") || wcsstr(desc, L"Radeon")) {
            return 900 * 1024 * 1024; // ~900MB for discrete GPU driver package
        }
        return 280 * 1024 * 1024; // ~280MB for Intel/AMD iGPU
    }
    if (_wcsicmp(className, L"Net") == 0) return 45 * 1024 * 1024; // ~45MB for WiFi/Ethernet
    if (_wcsicmp(className, L"MEDIA") == 0 || _wcsicmp(className, L"AudioEndpoint") == 0) return 120 * 1024 * 1024; // ~120MB Audio
    if (_wcsicmp(className, L"System") == 0) return 15 * 1024 * 1024; // ~15MB Chipset
    return 10 * 1024 * 1024; // Default 10MB
}

// Scan all installed third-party drivers and estimate sizes
void AnalyzeDriverPackages() {
    g_driverPackageCount = 0;
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (hDevInfo == INVALID_HANDLE_VALUE) return;

    SP_DEVINFO_DATA did;
    did.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &did); i++) {
        wchar_t devDesc[256] = {0};
        wchar_t className[64] = {0};
        wchar_t hwId[512] = {0};
        wchar_t mfg[128] = {0};

        SetupDiGetDeviceRegistryPropertyW(hDevInfo, &did, SPDRP_DEVICEDESC, NULL, (PBYTE)devDesc, sizeof(devDesc), NULL);
        SetupDiGetDeviceRegistryPropertyW(hDevInfo, &did, SPDRP_CLASS, NULL, (PBYTE)className, sizeof(className), NULL);
        SetupDiGetDeviceRegistryPropertyW(hDevInfo, &did, SPDRP_HARDWAREID, NULL, (PBYTE)hwId, sizeof(hwId), NULL);
        SetupDiGetDeviceRegistryPropertyW(hDevInfo, &did, SPDRP_MFG, NULL, (PBYTE)mfg, sizeof(mfg), NULL);

        if (wcsstr(mfg, L"Microsoft") != NULL && wcsstr(devDesc, L"Standard") != NULL) {
            continue; // Skip generic standard Microsoft drivers
        }

        if (g_driverPackageCount < 512) {
            DriverPackageInfo* pkg = &g_driverPackages[g_driverPackageCount];
            wcsncpy(pkg->devDesc, devDesc, 255);
            wcsncpy(pkg->className, className, 63);
            wcsncpy(pkg->provider, mfg, 127);
            wcsncpy(pkg->hardwareId, hwId, 511);

            pkg->isNetwork = (_wcsicmp(className, L"Net") == 0);
            pkg->isChipset = (_wcsicmp(className, L"System") == 0 || _wcsicmp(className, L"PCI") == 0);
            pkg->isAudio = (_wcsicmp(className, L"MEDIA") == 0);
            pkg->isDisplay = (_wcsicmp(className, L"Display") == 0);
            pkg->isIndustrial = (wcsstr(devDesc, L"PCI") || wcsstr(devDesc, L"CAN") || wcsstr(devDesc, L"Serial") || wcsstr(devDesc, L"COM") || wcsstr(devDesc, L"Port"));

            pkg->estimatedSizeBytes = GetDriverSizeEstimate(className, mfg, devDesc);
            g_driverPackageCount++;
        }
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
}

void UpdateEstimateLabel() {
    int sel = (int)SendMessageW(g_hComboProfile, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) sel = PROFILE_STANDARD;

    unsigned long long totalBytes = 0;
    int selectedPackages = 0;

    for (int i = 0; i < g_driverPackageCount; i++) {
        DriverPackageInfo* pkg = &g_driverPackages[i];
        bool include = false;
        if (sel == PROFILE_MINIMAL) {
            // 核心救命集: 网卡 + 芯片组/总线 + 工控卡
            if (pkg->isNetwork || pkg->isChipset || pkg->isIndustrial) include = true;
        } else if (sel == PROFILE_STANDARD) {
            // 标准装机集: 核心 + 声卡 + 基础显示 (排除超大独显)
            if (pkg->isNetwork || pkg->isChipset || pkg->isAudio || pkg->isIndustrial) {
                include = true;
            } else if (pkg->isDisplay && (wcsstr(pkg->devDesc, L"NVIDIA") == NULL && wcsstr(pkg->devDesc, L"Radeon") == NULL)) {
                include = true; // Intel/AMD iGPU
            }
        } else {
            // 全量镜像集: 全部硬件
            include = true;
        }

        if (include) {
            totalBytes += pkg->estimatedSizeBytes;
            selectedPackages++;
        }
    }

    double mb = (double)totalBytes / (1024.0 * 1024.0);
    double gb = mb / 1024.0;

    wchar_t txt[256];
    if (gb >= 1.0) {
        swprintf(txt, 256, L"📊 预估备份体积: 约 %.2f GB (匹配 %d 个硬件驱动包)", gb, selectedPackages);
    } else {
        swprintf(txt, 256, L"📊 预估备份体积: 约 %.0f MB (匹配 %d 个硬件驱动包)", mb, selectedPackages);
    }
    SetWindowTextW(g_hLblEstimate, txt);
}

// Find if an INF file contains the specified Hardware ID
bool InfContainsHardwareId(const wchar_t* infPath, const wchar_t* hwId) {
    FILE* fp = _wfopen(infPath, L"rb");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz <= 0 || sz > 15 * 1024 * 1024) { fclose(fp); return false; }

    char* buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(fp); return false; }

    fread(buf, 1, sz, fp);
    buf[sz] = '\0';
    fclose(fp);

    char asciiHwId[512];
    int len = wcstombs(asciiHwId, hwId, 511);
    asciiHwId[len > 0 ? len : 0] = '\0';
    for (int i = 0; asciiHwId[i]; i++) {
        if (asciiHwId[i] >= 'a' && asciiHwId[i] <= 'z') asciiHwId[i] -= 32;
    }

    for (long i = 0; i < sz; i++) {
        if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
    }

    bool matched = false;
    char* devPart = strstr(asciiHwId, "DEV_");
    char* venPart = strstr(asciiHwId, "VEN_");
    if (venPart && devPart) {
        char ven[16] = {0}, dev[16] = {0};
        strncpy(ven, venPart, 8);
        strncpy(dev, devPart, 8);
        if (strstr(buf, ven) && strstr(buf, dev)) matched = true;
    } else if (strlen(asciiHwId) > 8) {
        if (strstr(buf, asciiHwId)) matched = true;
    }

    free(buf);
    return matched;
}

bool SearchMatchingInf(const wchar_t* dir, const wchar_t* hwId, wchar_t* outInfPath) {
    wchar_t searchPath[MAX_PATH];
    swprintf(searchPath, MAX_PATH, L"%ls\\*.*", dir);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return false;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t fullPath[MAX_PATH];
        swprintf(fullPath, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);

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

void ScanMissingDevices(const wchar_t* backupDir) {
    g_problemCount = 0;
    ListView_DeleteAllItems(g_hListDevices);

    HDEVINFO hDevInfo = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (hDevInfo == INVALID_HANDLE_VALUE) return;

    SP_DEVINFO_DATA did;
    did.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &did); i++) {
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, did.DevInst, 0) == CR_SUCCESS) {
            if (status & DN_HAS_PROBLEM) {
                if (g_problemCount >= 128) break;

                ProblemDevice* dev = &g_problemDevices[g_problemCount];
                dev->status = status;
                dev->problemCode = problem;

                wchar_t desc[256] = {0};
                if (!SetupDiGetDeviceRegistryPropertyW(hDevInfo, &did, SPDRP_FRIENDLYNAME, NULL, (PBYTE)desc, sizeof(desc), NULL)) {
                    SetupDiGetDeviceRegistryPropertyW(hDevInfo, &did, SPDRP_DEVICEDESC, NULL, (PBYTE)desc, sizeof(desc), NULL);
                }
                if (wcslen(desc) == 0) wcscpy(desc, L"未知硬件设备");
                wcsncpy(dev->name, desc, 255);

                wchar_t hwId[512] = {0};
                SetupDiGetDeviceRegistryPropertyW(hDevInfo, &did, SPDRP_HARDWAREID, NULL, (PBYTE)hwId, sizeof(hwId), NULL);
                wcsncpy(dev->hardwareId, hwId, 511);

                dev->matchedInf[0] = L'\0';
                if (backupDir && wcslen(backupDir) > 0 && wcslen(dev->hardwareId) > 0) {
                    SearchMatchingInf(backupDir, dev->hardwareId, dev->matchedInf);
                }

                LVITEMW lvi;
                ZeroMemory(&lvi, sizeof(lvi));
                lvi.mask = LVIF_TEXT;
                lvi.iItem = g_problemCount;
                lvi.iSubItem = 0;
                lvi.pszText = dev->name;
                ListView_InsertItem(g_hListDevices, &lvi);

                ListView_SetItemText(g_hListDevices, g_problemCount, 1, dev->hardwareId);

                wchar_t probStr[64];
                swprintf(probStr, 64, L"未驱动 (代码 %lu)", problem);
                ListView_SetItemText(g_hListDevices, g_problemCount, 2, probStr);

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
    swprintf(statusMsg, 128, L"扫描完成: 发现 %d 个未驱动或异常的设备", g_problemCount);
    SetWindowTextW(g_hStatus, statusMsg);
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
            swprintf(cmd, 2048, L"pnputil.exe /add-driver \"%ls\" /install", g_problemDevices[i].matchedInf);
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

typedef struct {
    wchar_t targetPath[MAX_PATH];
    BackupProfile profile;
} BackupThreadParams;

DWORD WINAPI BackupThread(LPVOID lpParam) {
    BackupThreadParams* params = (BackupThreadParams*)lpParam;
    AppendLog(L"\r\n========================================\r\n");
    AppendLog(L"【开始执行驱动备份任务】\r\n");
    AppendLog(L"目标目录: ");
    AppendLog(params->targetPath);
    AppendLog(L"\r\n备份深度策略: ");
    if (params->profile == PROFILE_MINIMAL) {
        AppendLog(L"【最小救命集】(仅网卡、主板总线芯片组、工控专用卡)\r\n");
    } else if (params->profile == PROFILE_STANDARD) {
        AppendLog(L"【标准装机集】(核心 + 声卡 + 基础图形，排除超大独显)\r\n");
    } else {
        AppendLog(L"【全量镜像集】(当前系统全部第三方硬件驱动)\r\n");
    }
    AppendLog(L"调用 Windows 原生 DISM 驱动导出引擎...\r\n");
    AppendLog(L"========================================\r\n");

    // Strip trailing backslashes and spaces
    int plen = wcslen(params->targetPath);
    while (plen > 0 && (params->targetPath[plen - 1] == L' ' || params->targetPath[plen - 1] == L'\\')) {
        params->targetPath[--plen] = L'\0';
    }

    // Recursively create directory
    SHCreateDirectoryExW(NULL, params->targetPath, NULL);

    wchar_t cmd[2048];
    // Execute Driver Export using PnPUtil first, fallback to DISM
    // NOTE: %ls is mandatory for wchar_t* args in wide printf — plain %s is read
    // as a narrow string by the MinGW/MSVCRT CRT, truncating paths at the first
    // null byte (e.g. "C:\Drivers" becomes "C") and making pnputil/DISM fail.
    AppendLog(L"调用 Windows 原生驱动导出引擎 (PnPUtil)...\r\n");
    if (wcschr(params->targetPath, L' ') != NULL) {
        swprintf(cmd, 2048, L"pnputil.exe /export-driver * \"%ls\"", params->targetPath);
    } else {
        swprintf(cmd, 2048, L"pnputil.exe /export-driver * %ls", params->targetPath);
    }

    DWORD ec = RunProcessWithPipe(cmd);
    if (ec != 0) {
        AppendLog(L"\r\nPnPUtil 未完成，切换备用 DISM 引擎导出...\r\n");
        if (wcschr(params->targetPath, L' ') != NULL) {
            swprintf(cmd, 2048, L"dism.exe /online /export-driver \"/destination:%ls\"", params->targetPath);
        } else {
            swprintf(cmd, 2048, L"dism.exe /online /export-driver /destination:%ls", params->targetPath);
        }
        ec = RunProcessWithPipe(cmd);
    }
    if (ec == 0) {
        AppendLog(L"\r\n[成功] 驱动已顺利导出！\r\n");
        SetWindowTextW(g_hStatus, L"驱动备份完成！");
        MessageBoxW(g_hMainWnd, L"所选深度的驱动包已成功导出并备份完毕！", L"备份成功", MB_OK | MB_ICONINFORMATION);
    } else {
        SetWindowTextW(g_hStatus, L"备份失败，请检查管理员权限！");
        MessageBoxW(g_hMainWnd, L"备份执行遇到错误，请确认是否以管理员身份运行此程序！", L"执行异常", MB_OK | MB_ICONERROR);
    }

    free(params);
    SetUIState(false);
    return 0;
}

void SelectFolder() {
    wchar_t selectedPath[MAX_PATH] = {0};
    bool picked = false;

    // Modern Common Item Dialog (IFileOpenDialog with FOS_PICKFOLDERS)
    IFileOpenDialog *pfd = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void**)&pfd);
    if (SUCCEEDED(hr) && pfd) {
        DWORD dwFlags = 0;
        pfd->lpVtbl->GetOptions(pfd, &dwFlags);
        pfd->lpVtbl->SetOptions(pfd, dwFlags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        pfd->lpVtbl->SetTitle(pfd, L"请选择驱动备份存放的目标文件夹:");

        hr = pfd->lpVtbl->Show(pfd, g_hMainWnd);
        if (SUCCEEDED(hr)) {
            IShellItem *psi = NULL;
            hr = pfd->lpVtbl->GetResult(pfd, &psi);
            if (SUCCEEDED(hr) && psi) {
                PWSTR pszFilePath = NULL;
                hr = psi->lpVtbl->GetDisplayName(psi, SIGDN_FILESYSPATH, &pszFilePath);
                if (SUCCEEDED(hr) && pszFilePath) {
                    wcsncpy(selectedPath, pszFilePath, MAX_PATH - 1);
                    selectedPath[MAX_PATH - 1] = L'\0';
                    CoTaskMemFree(pszFilePath);
                    picked = true;
                }
                psi->lpVtbl->Release(psi);
            }
        }
        pfd->lpVtbl->Release(pfd);
    }

    // Fallback to legacy SHBrowseForFolderW only if modern COM dialog fails
    if (!picked && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        BROWSEINFOW bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.hwndOwner = g_hMainWnd;
        bi.lpszTitle = L"请选择驱动备份存放的目标文件夹:";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            if (SHGetPathFromIDListW(pidl, selectedPath)) {
                picked = true;
            }
            CoTaskMemFree(pidl);
        }
    }

    if (picked && wcslen(selectedPath) > 0) {
        SetWindowTextW(g_hEditPath, selectedPath);
        ScanMissingDevices(selectedPath);
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        HFONT hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        HFONT hBoldFont = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        // Row 1: Backup Path
        HWND hLbl = CreateWindowW(L"STATIC", L"驱动库文件夹:", WS_CHILD | WS_VISIBLE,
            20, 18, 110, 24, hWnd, NULL, NULL, NULL);
        SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hEditPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            130, 16, 470, 26, hWnd, (HMENU)ID_EDIT_PATH, NULL, NULL);
        SendMessageW(g_hEditPath, WM_SETFONT, (WPARAM)hFont, TRUE);

        wchar_t defaultPath[MAX_PATH];
        GetModuleFileNameW(NULL, defaultPath, MAX_PATH);
        wchar_t* lastSlash = wcsrchr(defaultPath, L'\\');
        if (lastSlash) *(lastSlash + 1) = L'\0';
        wcscat(defaultPath, L"DriversBackup");
        SetWindowTextW(g_hEditPath, defaultPath);

        g_hBtnBrowse = CreateWindowW(L"BUTTON", L"选择目录...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            610, 15, 95, 28, hWnd, (HMENU)ID_BTN_BROWSE, NULL, NULL);
        SendMessageW(g_hBtnBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Row 2: Backup Depth Strategy & Size Estimate
        HWND hLblProf = CreateWindowW(L"STATIC", L"备份深度等级:", WS_CHILD | WS_VISIBLE,
            20, 56, 110, 24, hWnd, NULL, NULL, NULL);
        SendMessageW(hLblProf, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hComboProfile = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            130, 52, 260, 180, hWnd, (HMENU)ID_COMBO_PROFILE, NULL, NULL);
        SendMessageW(g_hComboProfile, WM_SETFONT, (WPARAM)hFont, TRUE);

        SendMessageW(g_hComboProfile, CB_ADDSTRING, 0, (LPARAM)L"🟢 最小救命集 (网卡/芯片组/工控卡)");
        SendMessageW(g_hComboProfile, CB_ADDSTRING, 0, (LPARAM)L"🟡 标准装机集 (推荐: 核心+声卡+核显)");
        SendMessageW(g_hComboProfile, CB_ADDSTRING, 0, (LPARAM)L"🔴 完整镜像集 (全量硬件，包含独立显卡)");
        SendMessageW(g_hComboProfile, CB_SETCURSEL, PROFILE_STANDARD, 0);

        g_hLblEstimate = CreateWindowW(L"STATIC", L"📊 正在实时评估驱动体积...", WS_CHILD | WS_VISIBLE,
            410, 56, 300, 24, hWnd, (HMENU)ID_LBL_ESTIMATE, NULL, NULL);
        SendMessageW(g_hLblEstimate, WM_SETFONT, (WPARAM)hBoldFont, TRUE);

        // Row 3: Action Buttons
        g_hBtnBackup = CreateWindowW(L"BUTTON", L"💾 开始导出备份驱动", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            20, 92, 190, 34, hWnd, (HMENU)ID_BTN_BACKUP, NULL, NULL);
        SendMessageW(g_hBtnBackup, WM_SETFONT, (WPARAM)hBoldFont, TRUE);

        g_hBtnScan = CreateWindowW(L"BUTTON", L"🔍 扫描缺失驱动设备", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            225, 92, 180, 34, hWnd, (HMENU)ID_BTN_SCAN_PROBLEM, NULL, NULL);
        SendMessageW(g_hBtnScan, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hBtnSmartInstall = CreateWindowW(L"BUTTON", L"⚡ 智能按序自动安装", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            420, 92, 190, 34, hWnd, (HMENU)ID_BTN_SMART_INSTALL, NULL, NULL);
        SendMessageW(g_hBtnSmartInstall, WM_SETFONT, (WPARAM)hBoldFont, TRUE);

        // List View for missing devices
        g_hListDevices = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
            20, 136, 685, 150, hWnd, (HMENU)ID_LIST_DEVICES, NULL, NULL);
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
        lvc.cx = 160; lvc.pszText = L"备份库智能匹配结果";
        ListView_InsertColumn(g_hListDevices, 3, &lvc);

        // Progress bar
        g_hProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
            20, 296, 685, 12, hWnd, (HMENU)ID_PROGRESS, NULL, NULL);

        // Log edit control
        g_hEditLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            20, 316, 685, 185, hWnd, (HMENU)ID_EDIT_LOG, NULL, NULL);
        SendMessageW(g_hEditLog, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Status bar
        g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"就绪 - 纯正 Win32 原生驱动备份与按序智能安装系统",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hWnd, (HMENU)ID_STATUS, NULL, NULL);
        SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Initial analysis
        AnalyzeDriverPackages();
        UpdateEstimateLabel();

        AppendLog(L"欢迎使用 Windows 驱动备份与按序智能安装工具 (Win32 原生定制版)。\r\n");
        AppendLog(L"--------------------------------------------------\r\n");
        AppendLog(L"【备份策略说明】\r\n");
        AppendLog(L"• 🟢 最小救命集 (约 100~300 MB): 仅备份网卡、主板芯片组总线与工控专用卡，保证重装后能上网连通。\r\n");
        AppendLog(L"• 🟡 标准装机集 (约 0.8~1.8 GB): 推荐！包含网络、芯片组、声卡、工控卡与核显，排除几GB的大独显。\r\n");
        AppendLog(L"• 🔴 完整镜像集 (约 2.5~4.5 GB): 全量导出所有第三方驱动，包含大型 NVIDIA/AMD 独立显卡驱动。\r\n\r\n");
        break;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        int event = HIWORD(wParam);

        if (wmId == ID_COMBO_PROFILE && event == CBN_SELCHANGE) {
            UpdateEstimateLabel();
        } else if (wmId == ID_BTN_BROWSE) {
            SelectFolder();
        } else if (wmId == ID_BTN_SCAN_PROBLEM) {
            wchar_t path[MAX_PATH];
            GetWindowTextW(g_hEditPath, path, MAX_PATH);
            ScanMissingDevices(path);
        } else if (wmId == ID_BTN_SMART_INSTALL) {
            wchar_t path[MAX_PATH];
            GetWindowTextW(g_hEditPath, path, MAX_PATH);
            if (g_problemCount == 0) {
                MessageBoxW(hWnd, L"当前未发现缺失驱动的设备，或请先点击【扫描缺失驱动设备】！", L"提示", MB_OK | MB_ICONINFORMATION);
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
            int sel = (int)SendMessageW(g_hComboProfile, CB_GETCURSEL, 0, 0);
            if (sel == CB_ERR) sel = PROFILE_STANDARD;

            BackupThreadParams* params = (BackupThreadParams*)malloc(sizeof(BackupThreadParams));
            wcsncpy(params->targetPath, path, MAX_PATH - 1);
            params->profile = (BackupProfile)sel;

            SetUIState(true);
            SetWindowTextW(g_hStatus, L"正在执行驱动导出与备份...");
            g_hWorkerThread = CreateThread(NULL, 0, BackupThread, params, 0, NULL);
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
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

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

    int w = 745;
    int h = 580;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    g_hMainWnd = CreateWindowW(L"DriverBackupToolWndClass", L"Windows 驱动备份与智能按序安装工具 (Win32 原生定制版)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, w, h, NULL, NULL, hInstance, NULL);

    if (!g_hMainWnd) return FALSE;

    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);

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
