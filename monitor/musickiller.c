// musickiller.c — MusicKiller 纯EXE版 (Windows 10 x64)
//
// 单文件程序: 无参数运行 = 后台静默监控 (开机自启项调用此模式, 无窗口无控制台);
// 带参数运行 = 命令行控制器 (install / uninstall / run / kill / stop / status / allow)。
//
// 逻辑: 每 1 秒枚举进程, 发现网易云音乐 (cloudmusic*) / QQ音乐 (qqmusic*, qmbrowser*)
// 进程且 C:\allow.txt 不存在时, 直接静默结束该进程; allow.txt 存在则完全放行。
//
// 持久化: install 命令注册注册表 Run 键实现开机自启 (登录后自动后台运行)。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define MK_MUTEX_NAME   L"Local\\MusicKillerMonitorMutex"
#define MK_STOP_EVENT   L"Local\\MusicKillerStopEvent"
#define MK_ALLOW_FILE   L"C:\\allow.txt"
#define MK_RUNKEY_NAME  L"MusicKiller"
#define MK_RUNKEY_PATH  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"
#define MK_EXE_NAME     L"MusicKiller.exe"
#define MK_DIR_NAME     L"MusicKiller"
#define MK_LOG_NAME     L"monitor.log"
#define MK_POLL_MS      1000
#define MK_VERSION      "2.0"

static volatile BOOL g_stopFlag = FALSE;

/* 目标进程名前缀 (不区分大小写)。覆盖主程序与辅助进程:
     cloudmusic.exe / cloudmusic_reporter.exe ...  (网易云音乐)
     QQMusic.exe / QQMusicExternal.exe ...        (QQ音乐)
     qmbrowser.exe                                 (QQ音乐内置浏览器) */
static const wchar_t* g_prefixes[] = {
    L"cloudmusic",
    L"qqmusic",
    L"qmbrowser",
};

/* ------------------------------------------------------------------------ */
static void LogLine(const char* fmt, ...)
{
    wchar_t dir[MAX_PATH];
    wchar_t path[MAX_PATH];
    char msg[1024];
    va_list ap;
    FILE* f;
    SYSTEMTIME st;

    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH) == 0) {
        return;
    }
    wcscat_s(dir, MAX_PATH, L"\\" MK_DIR_NAME);
    CreateDirectoryW(dir, NULL);
    wcscpy_s(path, MAX_PATH, dir);
    wcscat_s(path, MAX_PATH, L"\\" MK_LOG_NAME);

    f = NULL;
    if (_wfopen_s(&f, path, L"ab") != 0 || f == NULL) {
        return;
    }
    fseek(f, 0, SEEK_END);
    if (ftell(f) > 256 * 1024) {
        fclose(f);
        f = NULL;
        if (_wfopen_s(&f, path, L"wb") != 0) {
            return;
        }
    }
    if (f) {
        GetLocalTime(&st);
        fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\r\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
        fclose(f);
    }
}

/* ------------------------------------------------------------------------ */
static void EnsureConsole(void)
{
    FILE* fp;
    if (AttachConsole(ATTACH_PARENT_PROCESS) == 0) {
        if (AllocConsole() == 0) {
            return;
        }
    }
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    fp = NULL;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    fp = NULL;
    freopen_s(&fp, "CONOUT$", "w", stderr);
}

/* ------------------------------------------------------------------------ */
static void PrintUsage(void)
{
    printf("MusicKiller 纯EXE版 v%s —— 网易云音乐 / QQ音乐 进程终结者\n", MK_VERSION);
    printf("用法: MusicKiller.exe [命令]\n\n");
    printf("  (无参数)          后台静默监控模式 (无窗口无提示, 开机自启项调用的就是它)\n");
    printf("  run               前台监控 (可看实时日志, Ctrl+C 退出)\n");
    printf("  kill              立即清理一次目标进程\n");
    printf("  install           安装开机自启 (当前用户, 免管理员)\n");
    printf("  install all       安装开机自启 (所有用户, 需管理员)\n");
    printf("  uninstall         卸载 (停止监控 + 删除自启 + 删除安装文件)\n");
    printf("  stop              停止正在运行的后台监控 (不卸载)\n");
    printf("  status            查询: 自启状态 / 运行状态 / allow.txt\n");
    printf("  allow on|off|status  放行开关 (创建/删除 C:\\allow.txt)\n");
    printf("  help              显示本帮助\n");
}

/* ------------------------------------------------------------------------ */
static BOOL AllowFileExists(void)
{
    return GetFileAttributesW(MK_ALLOW_FILE) != INVALID_FILE_ATTRIBUTES;
}

/* ------------------------------------------------------------------------ */
static BOOL IsTargetName(const wchar_t* exeName)
{
    size_t i;
    size_t nameLen = wcslen(exeName);
    for (i = 0; i < ARRAYSIZE(g_prefixes); ++i) {
        size_t plen = wcslen(g_prefixes[i]);
        if (nameLen >= plen && _wcsnicmp(exeName, g_prefixes[i], plen) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------------------ */
/* 枚举并静默结束所有目标进程, 返回结束的个数。                              */
static int SweepTargets(BOOL verbose)
{
    HANDLE snap;
    PROCESSENTRY32W pe;
    int killed = 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }

    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (IsTargetName(pe.szExeFile)) {
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hProc != NULL) {
                    if (TerminateProcess(hProc, 1)) {
                        char nameUtf8[256];
                        WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1,
                                            nameUtf8, (int)sizeof(nameUtf8), NULL, NULL);
                        nameUtf8[sizeof(nameUtf8) - 1] = 0;
                        if (verbose) {
                            printf("[!] 已结束进程: %s (PID %lu)\n",
                                   nameUtf8, (unsigned long)pe.th32ProcessID);
                        }
                        LogLine("killed %s pid=%lu", nameUtf8, (unsigned long)pe.th32ProcessID);
                        killed++;
                    }
                    CloseHandle(hProc);
                } else {
                    DWORD err = GetLastError();
                    LogLine("OpenProcess failed pid=%lu err=%lu",
                            (unsigned long)pe.th32ProcessID, (unsigned long)err);
                    if (verbose) {
                        printf("[!] 无法结束 PID %lu (错误 %lu)。若目标以管理员身份运行,\n"
                               "    本程序也需要用管理员身份运行。\n",
                               (unsigned long)pe.th32ProcessID, (unsigned long)err);
                    }
                }
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return killed;
}

/* ------------------------------------------------------------------------ */
static BOOL WINAPI ConsoleCtrlHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_BREAK_EVENT || type == CTRL_LOGOFF_EVENT ||
        type == CTRL_SHUTDOWN_EVENT) {
        g_stopFlag = TRUE;
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------------ */
/* 监控主循环。foreground=TRUE 时在控制台输出日志。单实例 (互斥锁)。          */
static void MonitorLoop(BOOL foreground)
{
    HANDLE hMutex;
    HANDLE hStop;

    hMutex = CreateMutexW(NULL, TRUE, MK_MUTEX_NAME);
    if (hMutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (foreground) {
            printf("[*] 监控已在后台运行中 (单实例限制)。\n");
        }
        if (hMutex != NULL) {
            CloseHandle(hMutex);
        }
        return;
    }

    hStop = CreateEventW(NULL, TRUE, FALSE, MK_STOP_EVENT);
    if (hStop != NULL) {
        ResetEvent(hStop);
    }

    if (foreground) {
        printf("[*] MusicKiller 监控已启动, 每 %d 秒检查一次。Ctrl+C 退出。\n",
               MK_POLL_MS / 1000);
        printf("[*] 放行开关: 存在 C:\\allow.txt 时不拦截。\n");
    }
    LogLine("monitor started (%s)", foreground ? "foreground" : "hidden");

    for (;;) {
        if (g_stopFlag) {
            break;
        }
        if (!AllowFileExists()) {
            SweepTargets(foreground);
        }
        if (hStop != NULL) {
            if (WaitForSingleObject(hStop, MK_POLL_MS) == WAIT_OBJECT_0) {
                break;
            }
        } else {
            Sleep(MK_POLL_MS);
        }
    }

    LogLine("monitor stopped");
    if (hStop != NULL) {
        CloseHandle(hStop);
    }
    CloseHandle(hMutex);
}

/* ------------------------------------------------------------------------ */
static BOOL GetInstallPath(BOOL allUsers, wchar_t* destDir, size_t dirChars,
                           wchar_t* destPath, size_t pathChars)
{
    if (allUsers) {
        if (GetEnvironmentVariableW(L"ProgramFiles", destDir, (DWORD)dirChars) == 0) {
            return FALSE;
        }
    } else {
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", destDir, (DWORD)dirChars) == 0) {
            return FALSE;
        }
    }
    wcscat_s(destDir, dirChars, L"\\" MK_DIR_NAME);
    wcscpy_s(destPath, pathChars, destDir);
    wcscat_s(destPath, pathChars, L"\\" MK_EXE_NAME);
    return TRUE;
}

/* ------------------------------------------------------------------------ */
static void SignalMonitorStop(void)
{
    HANDLE hStop = OpenEventW(EVENT_MODIFY_STATE, FALSE, MK_STOP_EVENT);
    if (hStop != NULL) {
        SetEvent(hStop);
        CloseHandle(hStop);
    }
}

/* ------------------------------------------------------------------------ */
static int CmdInstall(BOOL allUsers)
{
    wchar_t selfPath[MAX_PATH * 2];
    wchar_t destDir[MAX_PATH];
    wchar_t destPath[MAX_PATH * 2];
    wchar_t regValue[MAX_PATH * 2 + 4];
    HKEY rootKey;
    HKEY hKey;
    LSTATUS ls;
    DWORD n;

    rootKey = allUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;

    n = GetModuleFileNameW(NULL, selfPath, (DWORD)ARRAYSIZE(selfPath));
    if (n == 0 || n >= ARRAYSIZE(selfPath)) {
        printf("[!] 无法获取自身路径。\n");
        return 1;
    }
    if (!GetInstallPath(allUsers, destDir, ARRAYSIZE(destDir),
                        destPath, ARRAYSIZE(destPath))) {
        printf("[!] 无法解析安装目录。\n");
        return 1;
    }

    printf("[*] 安装到: %ls\n", destPath);

    if (CreateDirectoryW(destDir, NULL) == 0) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            if (err == ERROR_ACCESS_DENIED) {
                printf("[!] 无权限创建目录, 请右键“以管理员身份运行”cmd 后再试。\n");
            } else {
                printf("[!] 创建目录失败 (错误 %lu)。\n", (unsigned long)err);
            }
            return 1;
        }
    }

    if (_wcsicmp(selfPath, destPath) != 0) {
        if (!CopyFileW(selfPath, destPath, FALSE)) {
            DWORD err = GetLastError();
            if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED) {
                /* 可能是旧版监控正在运行: 先停掉再重试一次 */
                printf("[*] 目标文件被占用, 停止旧监控后重试...\n");
                SignalMonitorStop();
                Sleep(2000);
            }
            if (!CopyFileW(selfPath, destPath, FALSE)) {
                DWORD err2 = GetLastError();
                printf("[!] 复制失败 (错误 %lu)。%s\n", (unsigned long)err2,
                       err2 == ERROR_ACCESS_DENIED ? "请用管理员身份运行。" : "");
                return 1;
            }
        }
    } else {
        printf("[*] 已在安装目录中运行, 跳过复制。\n");
    }

    /* 注册 Run 键实现开机自启 */
    swprintf_s(regValue, ARRAYSIZE(regValue), L"\"%ls\"", destPath);
    ls = RegCreateKeyExW(rootKey, MK_RUNKEY_PATH, 0, NULL, 0, KEY_SET_VALUE,
                         NULL, &hKey, NULL);
    if (ls != ERROR_SUCCESS) {
        printf("[!] 打开注册表 Run 键失败 (错误 %ld)。%s\n", (long)ls,
               allUsers ? "所有用户安装需要管理员身份运行。" : "");
        return 1;
    }
    ls = RegSetValueExW(hKey, MK_RUNKEY_NAME, 0, REG_SZ, (const BYTE*)regValue,
                        (DWORD)((wcslen(regValue) + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    if (ls != ERROR_SUCCESS) {
        printf("[!] 写入注册表失败 (错误 %ld)。\n", (long)ls);
        return 1;
    }

    printf("[+] 开机自启已注册 (%s)。\n", allUsers ? "所有用户" : "当前用户");

    /* 立即启动监控 */
    {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));
        if (CreateProcessW(destPath, NULL, NULL, NULL, FALSE,
                           DETACHED_PROCESS | CREATE_NO_WINDOW,
                           NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            printf("[+] 监控已在后台静默启动 (无窗口无提示, 立即生效)。\n");
        } else {
            printf("[*] 监控将在下次登录时自动启动 (当前可运行 MusicKiller.exe run 手动启动)。\n");
        }
    }

    printf("[*] 查询状态: MusicKiller.exe status   卸载: MusicKiller.exe uninstall\n");
    return 0;
}

/* ------------------------------------------------------------------------ */
static int CmdUninstall(void)
{
    wchar_t selfPath[MAX_PATH * 2];
    wchar_t destDir[MAX_PATH];
    wchar_t destPath[MAX_PATH * 2];
    HKEY hKey;
    int scope;

    GetModuleFileNameW(NULL, selfPath, (DWORD)ARRAYSIZE(selfPath));

    printf("[*] 停止后台监控...\n");
    SignalMonitorStop();
    Sleep(2000);

    /* 删除两个作用域的自启项 */
    for (scope = 0; scope < 2; ++scope) {
        HKEY rootKey = scope == 0 ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
        const char* label = scope == 0 ? "当前用户" : "所有用户";
        if (RegOpenKeyExW(rootKey, MK_RUNKEY_PATH, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            if (RegDeleteValueW(hKey, MK_RUNKEY_NAME) == ERROR_SUCCESS) {
                printf("[+] 已删除开机自启 (%s)。\n", label);
            }
            RegCloseKey(hKey);
        }
    }

    /* 删除安装目录中的程序 (两个可能位置) */
    for (scope = 0; scope < 2; ++scope) {
        if (!GetInstallPath(scope == 1, destDir, ARRAYSIZE(destDir),
                            destPath, ARRAYSIZE(destPath))) {
            continue;
        }
        if (GetFileAttributesW(destPath) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        if (_wcsicmp(selfPath, destPath) == 0) {
            /* 正在运行 uninstall 的就是安装目录里的自己: 延迟自删 */
            wchar_t cmdLine[MAX_PATH * 3];
            STARTUPINFOW si;
            PROCESS_INFORMATION pi;
            swprintf_s(cmdLine, ARRAYSIZE(cmdLine),
                       L"cmd.exe /c timeout /t 2 /nobreak >nul & del /f /q \"%ls\"",
                       destPath);
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            ZeroMemory(&pi, sizeof(pi));
            if (CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE,
                               CREATE_NO_WINDOW | DETACHED_PROCESS,
                               NULL, NULL, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                printf("[+] 程序将在本命令退出后自动删除: %ls\n", destPath);
            } else {
                printf("[!] 请手动删除: %ls\n", destPath);
            }
        } else {
            if (DeleteFileW(destPath)) {
                printf("[+] 已删除: %ls\n", destPath);
            } else {
                printf("[!] 删除失败 (%lu), 请手动删除: %ls\n",
                       (unsigned long)GetLastError(), destPath);
            }
        }
    }

    printf("[+] 卸载完成。\n");
    return 0;
}

/* ------------------------------------------------------------------------ */
static int CmdStop(void)
{
    HANDLE hMutex = OpenMutexW(SYNCHRONIZE, FALSE, MK_MUTEX_NAME);
    if (hMutex == NULL) {
        printf("[*] 监控没有在运行。\n");
        return 0;
    }
    CloseHandle(hMutex);
    SignalMonitorStop();
    printf("[+] 已发送停止指令, 后台监控将在几秒内静默退出 (自启项保留, 下次登录仍会启动)。\n");
    return 0;
}

/* ------------------------------------------------------------------------ */
static void PrintRunKeyState(HKEY rootKey, const char* label)
{
    HKEY hKey;
    wchar_t val[MAX_PATH * 2];
    DWORD type = 0;
    DWORD size = sizeof(val);

    if (RegOpenKeyExW(rootKey, MK_RUNKEY_PATH, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        printf("开机自启(%s): 未设置\n", label);
        return;
    }
    if (RegQueryValueExW(hKey, MK_RUNKEY_NAME, NULL, &type, (LPBYTE)val, &size) == ERROR_SUCCESS
        && type == REG_SZ) {
        char u8[MAX_PATH * 4];
        WideCharToMultiByte(CP_UTF8, 0, val, -1, u8, (int)sizeof(u8), NULL, NULL);
        u8[sizeof(u8) - 1] = 0;
        printf("开机自启(%s): 已设置 -> %s\n", label, u8);
    } else {
        printf("开机自启(%s): 未设置\n", label);
    }
    RegCloseKey(hKey);
}

/* ------------------------------------------------------------------------ */
static int CmdStatus(void)
{
    HANDLE hMutex;

    printf("========== MusicKiller 状态 ==========\n");

    PrintRunKeyState(HKEY_CURRENT_USER, "当前用户");
    PrintRunKeyState(HKEY_LOCAL_MACHINE, "所有用户");

    hMutex = OpenMutexW(SYNCHRONIZE, FALSE, MK_MUTEX_NAME);
    printf("监控运行:   %s\n", hMutex != NULL ? "正在后台静默运行" : "未运行");
    if (hMutex != NULL) {
        CloseHandle(hMutex);
    }

    printf("allow.txt:  %s\n", AllowFileExists()
           ? "存在 -> 放行模式 (不拦截)"
           : "不存在 -> 拦截模式 (拦截网易云/QQ音乐)");

    printf("======================================\n");
    return 0;
}

/* ------------------------------------------------------------------------ */
static int CmdAllow(const wchar_t* sub)
{
    HANDLE h;

    if (sub == NULL || _wcsicmp(sub, L"status") == 0) {
        printf("allow.txt: %s\n", AllowFileExists() ? "存在 -> 放行模式" : "不存在 -> 拦截模式");
        return 0;
    }
    if (_wcsicmp(sub, L"on") == 0) {
        h = CreateFileW(MK_ALLOW_FILE, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            printf("[!] 创建 C:\\allow.txt 失败 (错误 %lu)。%s\n", (unsigned long)err,
                   err == ERROR_ACCESS_DENIED ? "C 盘根目录需要管理员权限, 请以管理员身份运行。" : "");
            return 1;
        }
        CloseHandle(h);
        printf("[+] 已创建 C:\\allow.txt —— 放行模式, 网易云/QQ音乐可正常运行。\n");
        return 0;
    }
    if (_wcsicmp(sub, L"off") == 0) {
        if (!DeleteFileW(MK_ALLOW_FILE)) {
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND) {
                printf("[*] C:\\allow.txt 本来就不存在, 当前为拦截模式。\n");
                return 0;
            }
            printf("[!] 删除失败 (错误 %lu)。\n", (unsigned long)err);
            return 1;
        }
        printf("[+] 已删除 C:\\allow.txt —— 拦截模式。\n");
        return 0;
    }
    printf("[!] 未知参数。用法: MusicKiller.exe allow on|off|status\n");
    return 1;
}

/* ------------------------------------------------------------------------ */
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPWSTR lpCmdLine, int nShowCmd)
{
    int argc = 0;
    LPWSTR* argv;
    int ret = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nShowCmd;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argv == NULL || argc <= 1) {
        /* 无参数 = 后台静默监控 (开机自启调用此模式): 无窗口、无控制台、无提示 */
        MonitorLoop(FALSE);
        if (argv) {
            LocalFree(argv);
        }
        return 0;
    }

    EnsureConsole();
    printf("MusicKiller 纯EXE版 v%s\n\n", MK_VERSION);

    if (_wcsicmp(argv[1], L"run") == 0) {
        SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
        MonitorLoop(TRUE);
    } else if (_wcsicmp(argv[1], L"kill") == 0) {
        int k;
        if (AllowFileExists()) {
            printf("[*] allow.txt 存在, 当前为放行模式, 未执行清理。\n");
            k = 0;
        } else {
            k = SweepTargets(TRUE);
            if (k > 0) {
                printf("[+] 本次共结束 %d 个目标进程。\n", k);
            } else {
                printf("[*] 未发现正在运行的目标进程。\n");
            }
        }
    } else if (_wcsicmp(argv[1], L"install") == 0) {
        ret = CmdInstall(argc >= 3 && _wcsicmp(argv[2], L"all") == 0);
    } else if (_wcsicmp(argv[1], L"uninstall") == 0) {
        ret = CmdUninstall();
    } else if (_wcsicmp(argv[1], L"stop") == 0) {
        ret = CmdStop();
    } else if (_wcsicmp(argv[1], L"status") == 0) {
        ret = CmdStatus();
    } else if (_wcsicmp(argv[1], L"allow") == 0) {
        ret = CmdAllow(argc >= 3 ? argv[2] : NULL);
    } else if (_wcsicmp(argv[1], L"help") == 0 || _wcsicmp(argv[1], L"/?") == 0 ||
               _wcsicmp(argv[1], L"-h") == 0 || _wcsicmp(argv[1], L"--help") == 0) {
        PrintUsage();
    } else {
        printf("[!] 未知命令: %ls\n\n", argv[1]);
        PrintUsage();
        ret = 1;
    }

    LocalFree(argv);
    return ret;
}
