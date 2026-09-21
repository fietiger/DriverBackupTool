#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

// Simple console runner that executes a command via CreateProcess and pipes output to console
DWORD RunCommand(const wchar_t* cmd) {
    wprintf(L"\r\n[执行系统指令]: %ls\r\n\r\n", cmd);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        wprintf(L"[错误] 无法创建进程通信管道\r\n");
        return 1;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    wchar_t cmdBuffer[2048];
    wcsncpy(cmdBuffer, cmd, 2047);

    if (!CreateProcessW(NULL, cmdBuffer, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(hWrite);
        CloseHandle(hRead);
        DWORD err = GetLastError();
        wprintf(L"[错误] 启动进程失败，错误码: %lu (请确认是否以管理员身份运行!)\r\n", err);
        return err;
    }
    CloseHandle(hWrite);

    char buffer[512];
    DWORD bytesRead;
    while (ReadFile(hRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        // Output OEM text directly to console
        printf("%s", buffer);
        fflush(stdout);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    return exitCode;
}

int wmain(int argc, wchar_t* argv[]) {
    // Set console to support UTF-8 / Chinese
    SetConsoleOutputCP(CP_UTF8);

    wprintf(L"========================================================\r\n");
    wprintf(L"       Windows 驱动导出与还原底层引擎测试工具 (CLI)\r\n");
    wprintf(L"========================================================\r\n");

    wchar_t targetDir[MAX_PATH] = L"D:\\XX";

    if (argc >= 2) {
        wcsncpy(targetDir, argv[1], MAX_PATH - 1);
    } else {
        wprintf(L"提示: 未传入参数，默认使用测试目标路径: %ls\r\n", targetDir);
        wprintf(L"用法: DriverTestCLI.exe [目标路径，如 D:\\XX 或 D:\\Drivers]\r\n\r\n");
    }

    // Clean path
    int plen = wcslen(targetDir);
    while (plen > 0 && (targetDir[plen - 1] == L' ' || targetDir[plen - 1] == L'\\')) {
        targetDir[--plen] = L'\0';
    }

    wprintf(L"1. 正在创建目标目录: %ls ...\r\n", targetDir);
    HRESULT hr = SHCreateDirectoryExW(NULL, targetDir, NULL);
    if (SUCCEEDED(hr) || hr == ERROR_ALREADY_EXISTS) {
        wprintf(L"   [成功] 目标目录就绪。\r\n");
    } else {
        wprintf(L"   [警告] 目录创建返回: 0x%08lx，将继续尝试导出...\r\n", hr);
    }

    // Ensure directory exists and is validated
    SHCreateDirectoryExW(NULL, targetDir, NULL);

    // Format PnPUtil command.
    // NOTE: %ls is mandatory for wchar_t* args in wide printf — plain %s is read
    // as a narrow string by the MinGW/MSVCRT CRT, truncating paths at the first
    // null byte (e.g. "C:\Drivers" becomes "C") and making pnputil/DISM fail.
    wchar_t cmdPnp[2048];
    if (wcschr(targetDir, L' ') != NULL) {
        swprintf(cmdPnp, 2048, L"pnputil.exe /export-driver * \"%ls\"", targetDir);
    } else {
        swprintf(cmdPnp, 2048, L"pnputil.exe /export-driver * %ls", targetDir);
    }

    DWORD ecPnp = RunCommand(cmdPnp);
    wprintf(L"\r\nPnPUtil 执行退出码: %lu\r\n", ecPnp);

    if (ecPnp == 0) {
        wprintf(L"\r\n========================================================\r\n");
        wprintf(L"✅ [测试通过] PnPUtil 成功将全部第三方驱动导出到: %ls\r\n", targetDir);
        wprintf(L"========================================================\r\n");
    } else {
        wprintf(L"\r\n⚠️ [PnPUtil 异常，尝试 DISM 备用引擎测试...]\r\n");
        wchar_t cmdDism[2048];
        if (wcschr(targetDir, L' ') != NULL) {
            swprintf(cmdDism, 2048, L"dism.exe /online /export-driver \"/destination:%ls\"", targetDir);
        } else {
            swprintf(cmdDism, 2048, L"dism.exe /online /export-driver /destination:%ls", targetDir);
        }
        DWORD ecDism = RunCommand(cmdDism);
        wprintf(L"\r\nDISM 执行退出码: %lu\r\n", ecDism);
    }

    wprintf(L"\r\n按任意键退出测试程序...\r\n");
    system("pause >nul");
    return 0;
}
