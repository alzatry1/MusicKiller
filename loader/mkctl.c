// mkctl.c — MusicKiller 驱动注入/控制程序 (Windows 10 x64)
//
// 命令:
//   setup                 一键安装并启动驱动 (服务设为自动启动, 重启后依然生效)
//   install [sys路径]     安装驱动服务 (自动启动), 默认从本程序同目录复制 MusicKiller.sys
//   start                 启动驱动
//   stop                  停止驱动
//   restart               重启驱动
//   status                查询驱动加载状态 / allow.txt / 测试签名模式
//   uninstall             停止并卸载驱动
//   allow on|off|status   创建 / 删除 / 查询 C:\allow.txt 放行文件
//   testsign on|off       开启 / 关闭测试签名模式 (修改后需重启)
//
// 需要管理员权限 (清单已声明 requireAdministrator, 双击会自动弹出 UAC)。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MK_SERVICE_NAME   L"MusicKiller"
#define MK_DISPLAY_NAME   L"MusicKiller Driver"
#define MK_DESCRIPTION    L"Block NetEase Cloud Music / QQ Music processes (bypass file: C:\\allow.txt)"
#define MK_DRIVER_FILE    L"MusicKiller.sys"
#define MK_ALLOW_FILE     L"C:\\allow.txt"

#define WAIT_TIMEOUT_MS   15000

/* ------------------------------------------------------------------------ */
static void PrintUsage(void)
{
    printf("MusicKiller 驱动控制程序 (mkctl.exe)\n");
    printf("用法: mkctl.exe <命令> [参数]\n\n");
    printf("  setup                 一键安装并启动驱动 (开机自启, 重启依然生效)\n");
    printf("  install [sys路径]     仅安装驱动服务 (默认复制同目录的 MusicKiller.sys)\n");
    printf("  start                 启动驱动\n");
    printf("  stop                  停止驱动\n");
    printf("  restart               重启驱动\n");
    printf("  status                查询驱动状态 / allow.txt / 测试签名模式\n");
    printf("  uninstall             停止并卸载驱动\n");
    printf("  allow on|off|status   创建 / 删除 / 查询 C:\\allow.txt 放行文件\n");
    printf("  testsign on|off       开启 / 关闭测试签名模式 (需重启生效)\n");
    printf("\n注意: 所有命令都需要以管理员身份运行。\n");
}

/* ------------------------------------------------------------------------ */
static void PrintWin32Error(const char* prefix, DWORD err)
{
    LPWSTR msg = NULL;
    char   msgUtf8[512];

    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   (LPWSTR)&msg, 0, NULL);

    printf("[!] %s失败, 错误码 %lu", prefix, (unsigned long)err);
    if (msg) {
        WideCharToMultiByte(CP_UTF8, 0, msg, -1, msgUtf8, (int)sizeof(msgUtf8), NULL, NULL);
        msgUtf8[sizeof(msgUtf8) - 1] = 0;
        printf(" : %s", msgUtf8);
        LocalFree(msg);
    }
    printf("\n");
}

/* ------------------------------------------------------------------------ */
/* 运行一条控制台命令并捕获输出, 返回进程退出码。输出截断到 bufSize-1。     */
static DWORD RunCaptureW(const wchar_t* cmd, char* buf, DWORD bufSize)
{
    HANDLE hRead = NULL, hWrite = NULL;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exitCode = 1;
    DWORD total = 0;
    wchar_t* cmdCopy;
    char scratch[512];

    if (buf && bufSize > 0) {
        buf[0] = 0;
    }

    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        return 1;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    ZeroMemory(&pi, sizeof(pi));

    cmdCopy = _wcsdup(cmd);
    if (cmdCopy == NULL) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return 1;
    }

    if (CreateProcessW(NULL, cmdCopy, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                       NULL, NULL, &si, &pi)) {
        DWORD avail, readn;

        CloseHandle(hWrite);
        hWrite = NULL;

        for (;;) {
            avail = 0;
            if (PeekNamedPipe(hRead, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                char* dst = scratch;
                DWORD want = avail;
                if (want > sizeof(scratch)) {
                    want = sizeof(scratch);
                }
                if (buf && (total + 1 < bufSize)) {
                    DWORD room = bufSize - 1 - total;
                    dst = buf + total;
                    if (want > room) {
                        want = room;
                    }
                }
                if (want == 0) {
                    dst = scratch;
                    want = (avail > sizeof(scratch)) ? (DWORD)sizeof(scratch) : avail;
                }
                if (!ReadFile(hRead, dst, want, &readn, NULL) || readn == 0) {
                    break;
                }
                if (dst != scratch) {
                    total += readn;
                }
            } else {
                if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
                    /* 进程退出后尽量把剩余数据读干净 */
                    for (;;) {
                        avail = 0;
                        if (!PeekNamedPipe(hRead, NULL, 0, NULL, &avail, NULL) || avail == 0) {
                            break;
                        }
                        {
                            char* dst = scratch;
                            DWORD want = (avail > sizeof(scratch)) ? (DWORD)sizeof(scratch) : avail;
                            if (buf && (total + 1 < bufSize)) {
                                DWORD room = bufSize - 1 - total;
                                dst = buf + total;
                                if (want > room) {
                                    want = room;
                                }
                            }
                            if (want == 0 ||
                                !ReadFile(hRead, dst, want, &readn, NULL) || readn == 0) {
                                break;
                            }
                            if (dst != scratch) {
                                total += readn;
                            }
                        }
                    }
                    break;
                }
            }
        }

        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    if (hWrite) {
        CloseHandle(hWrite);
    }
    CloseHandle(hRead);
    free(cmdCopy);

    if (buf && bufSize > 0) {
        if (total >= bufSize) {
            total = bufSize - 1;
        }
        buf[total] = 0;
    }
    return exitCode;
}

/* ------------------------------------------------------------------------ */
/* 1 = 已启用, 0 = 未启用, -1 = 查询失败                                     */
static int IsTestSigningEnabled(void)
{
    static char buf[16384];
    DWORD rc = RunCaptureW(L"bcdedit.exe /enum {current}", buf, (DWORD)sizeof(buf));
    char* p;
    char* eol;

    if (rc != 0) {
        return -1;
    }

    p = strstr(buf, "testsigning");
    if (p == NULL) {
        return 0;
    }
    eol = strchr(p, '\n');
    if (eol) {
        *eol = 0;
    }
    if (strstr(p, "Yes") || strstr(p, "yes")) {
        return 1;
    }
    /* 中文系统 bcdedit 输出 “是” (UTF-8: E6 98 AF) */
    if (strstr(p, "\xE6\x98\xAF")) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
static SC_HANDLE OpenMkService(DWORD access, SC_HANDLE* outScm)
{
    SC_HANDLE scm;
    SC_HANDLE svc;

    *outScm = NULL;
    scm = OpenSCManagerW(NULL, NULL, access);
    if (scm == NULL) {
        return NULL;
    }
    svc = OpenServiceW(scm, MK_SERVICE_NAME, access);
    if (svc == NULL) {
        CloseServiceHandle(scm);
        return NULL;
    }
    *outScm = scm;
    return svc;
}

/* ------------------------------------------------------------------------ */
static DWORD MkWaitServiceState(SC_HANDLE svc, DWORD target, DWORD timeoutMs)
{
    SERVICE_STATUS_PROCESS ssp;
    DWORD needed;
    ULONGLONG start = GetTickCount64();

    for (;;) {
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                  (LPBYTE)&ssp, sizeof(ssp), &needed)) {
            return 0xFFFFFFFF;
        }
        if (ssp.dwCurrentState == target) {
            return target;
        }
        if (GetTickCount64() - start > timeoutMs) {
            return ssp.dwCurrentState;
        }
        Sleep(200);
    }
}

/* ------------------------------------------------------------------------ */
static const char* ServiceStateText(DWORD state)
{
    switch (state) {
    case SERVICE_STOPPED:         return "已停止 (STOPPED)";
    case SERVICE_START_PENDING:   return "正在启动 (START_PENDING)";
    case SERVICE_STOP_PENDING:    return "正在停止 (STOP_PENDING)";
    case SERVICE_RUNNING:         return "运行中 (RUNNING)";
    case SERVICE_CONTINUE_PENDING:return "CONTINUE_PENDING";
    case SERVICE_PAUSE_PENDING:   return "PAUSE_PENDING";
    case SERVICE_PAUSED:          return "已暂停 (PAUSED)";
    default:                      return "未知";
    }
}

/* ------------------------------------------------------------------------ */
static BOOL GetDriverDestPath(wchar_t* out, size_t outChars)
{
    UINT n = GetSystemDirectoryW(out, (UINT)outChars);
    if (n == 0 || n >= outChars) {
        return FALSE;
    }
    if (out[wcslen(out) - 1] != L'\\') {
        wcscat_s(out, outChars, L"\\");
    }
    wcscat_s(out, outChars, L"drivers\\");
    wcscat_s(out, outChars, MK_DRIVER_FILE);
    return TRUE;
}

/* ------------------------------------------------------------------------ */
static BOOL ResolveDriverSource(const wchar_t* argPath, wchar_t* out, size_t outChars)
{
    DWORD n;
    wchar_t* slash;

    if (argPath && argPath[0]) {
        wcsncpy_s(out, outChars, argPath, _TRUNCATE);
        return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
    }

    n = GetModuleFileNameW(NULL, out, (DWORD)outChars);
    if (n == 0 || n >= outChars) {
        return FALSE;
    }
    slash = wcsrchr(out, L'\\');
    if (slash) {
        slash[1] = 0;
    }
    wcscat_s(out, outChars, MK_DRIVER_FILE);
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

/* ------------------------------------------------------------------------ */
static int CmdInstall(const wchar_t* argPath)
{
    wchar_t src[MAX_PATH * 2];
    wchar_t dst[MAX_PATH * 2];
    SC_HANDLE scm = NULL;
    SC_HANDLE svc = NULL;
    int ret = 1;

    if (!ResolveDriverSource(argPath, src, ARRAYSIZE(src))) {
        printf("[!] 找不到驱动文件 %ls。\n", MK_DRIVER_FILE);
        printf("    请把 %ls 放到 mkctl.exe 同目录, 或用 mkctl install <路径> 指定。\n", MK_DRIVER_FILE);
        return 1;
    }
    if (!GetDriverDestPath(dst, ARRAYSIZE(dst))) {
        printf("[!] 无法解析系统 drivers 目录。\n");
        return 1;
    }

    printf("[*] 复制驱动: %ls\n    -> %ls\n", src, dst);
    if (!CopyFileW(src, dst, FALSE)) {
        DWORD err = GetLastError();
        PrintWin32Error("复制驱动文件", err);
        printf("    如果提示文件被占用, 请先 mkctl stop 停止驱动后重试。\n");
        return 1;
    }

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scm == NULL) {
        PrintWin32Error("打开服务管理器(请以管理员身份运行)", GetLastError());
        goto done;
    }

    svc = OpenServiceW(scm, MK_SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (svc) {
        printf("[*] 服务已存在, 更新为自动启动配置...\n");
        if (!ChangeServiceConfigW(svc, SERVICE_KERNEL_DRIVER, SERVICE_AUTO_START,
                                  SERVICE_ERROR_NORMAL, dst, NULL, NULL, NULL,
                                  NULL, NULL, MK_DISPLAY_NAME)) {
            PrintWin32Error("更新服务配置", GetLastError());
            goto done;
        }
    } else {
        printf("[*] 创建驱动服务 (自动启动, 重启后自动加载)...\n");
        svc = CreateServiceW(scm, MK_SERVICE_NAME, MK_DISPLAY_NAME,
                             SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER,
                             SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                             dst, NULL, NULL, NULL, NULL, NULL);
        if (svc == NULL) {
            PrintWin32Error("创建服务", GetLastError());
            goto done;
        }
    }

    {
        SERVICE_DESCRIPTIONW sd;
        wchar_t desc[256];
        wcsncpy_s(desc, ARRAYSIZE(desc), MK_DESCRIPTION, _TRUNCATE);
        sd.lpDescription = desc;
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &sd);
    }

    printf("[+] 安装成功。服务: %ls, 驱动文件: %ls\n", MK_SERVICE_NAME, dst);
    printf("    启动驱动: mkctl start\n");
    ret = 0;

done:
    if (svc) CloseServiceHandle(svc);
    if (scm) CloseServiceHandle(scm);
    return ret;
}

/* ------------------------------------------------------------------------ */
/* 驱动加载后兜底清理一次仍在运行的目标进程 (正常情况下驱动自己会处理)。     */
static void KillTargetsNow(void)
{
    printf("[*] 清理正在运行的目标进程...\n");
    RunCaptureW(L"taskkill.exe /F /T /IM cloudmusic.exe", NULL, 0);
    RunCaptureW(L"taskkill.exe /F /T /IM QQMusic.exe", NULL, 0);
}

/* ------------------------------------------------------------------------ */
static int CmdStart(void)
{
    SC_HANDLE scm = NULL;
    SC_HANDLE svc;
    DWORD state;
    int ret = 1;

    svc = OpenMkService(SERVICE_ALL_ACCESS, &scm);
    if (svc == NULL) {
        PrintWin32Error("打开服务(是否已 mkctl install? 需要管理员)", GetLastError());
        return 1;
    }

    state = MkWaitServiceState(svc, SERVICE_RUNNING, 0);
    if (state == SERVICE_RUNNING) {
        printf("[*] 驱动已在运行中。\n");
        KillTargetsNow();
        ret = 0;
        goto done;
    }

    printf("[*] 正在启动驱动服务...\n");
    if (!StartServiceW(svc, 0, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_ALREADY_RUNNING) {
            printf("[*] 驱动已在运行中。\n");
            ret = 0;
            goto done;
        }
        PrintWin32Error("启动驱动", err);
        if (err == 577 /* ERROR_INVALID_IMAGE_HASH */) {
            printf("    原因: 驱动签名验证失败。\n");
            printf("    处理: 1) mkctl testsign on  2) 重启电脑  3) mkctl start\n");
            printf("          (若 bcdedit 报错被安全启动保护, 需进 BIOS 关闭 Secure Boot)\n");
            printf("          并确认 MusicKiller.sys 是 CI 编译的已签名版本。\n");
        } else if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            printf("    驱动文件缺失, 请先执行: mkctl install\n");
        }
        goto done;
    }

    state = MkWaitServiceState(svc, SERVICE_RUNNING, WAIT_TIMEOUT_MS);
    if (state == SERVICE_RUNNING) {
        printf("[+] 驱动已启动, 拦截已生效。\n");
        KillTargetsNow();
        ret = 0;
    } else {
        printf("[!] 驱动未能在超时时间内进入运行状态, 当前状态: %s\n",
               ServiceStateText(state));
    }

done:
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ret;
}

/* ------------------------------------------------------------------------ */
static int CmdStop(void)
{
    SC_HANDLE scm = NULL;
    SC_HANDLE svc;
    SERVICE_STATUS status;
    DWORD state;
    int ret = 1;

    svc = OpenMkService(SERVICE_ALL_ACCESS, &scm);
    if (svc == NULL) {
        PrintWin32Error("打开服务(是否已 mkctl install? 需要管理员)", GetLastError());
        return 1;
    }

    state = MkWaitServiceState(svc, SERVICE_STOPPED, 0);
    if (state == SERVICE_STOPPED) {
        printf("[*] 驱动已是停止状态。\n");
        ret = 0;
        goto done;
    }

    printf("[*] 正在停止驱动...\n");
    if (!ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
        PrintWin32Error("停止驱动", GetLastError());
        goto done;
    }

    state = MkWaitServiceState(svc, SERVICE_STOPPED, WAIT_TIMEOUT_MS);
    if (state == SERVICE_STOPPED) {
        printf("[+] 驱动已停止, 拦截已解除。\n");
        ret = 0;
    } else {
        printf("[!] 停止超时, 当前状态: %s\n", ServiceStateText(state));
    }

done:
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ret;
}

/* ------------------------------------------------------------------------ */
static int CmdStatus(void)
{
    SC_HANDLE scm = NULL;
    SC_HANDLE svc;
    SERVICE_STATUS_PROCESS ssp;
    DWORD needed;
    DWORD err;
    int ts;

    printf("========== MusicKiller 状态 ==========\n");

    svc = OpenMkService(SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG, &scm);
    err = GetLastError();
    if (svc == NULL) {
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            printf("驱动服务:   未安装 (执行 mkctl setup 一键安装)\n");
        } else {
            PrintWin32Error("查询服务", err);
        }
    } else {
        DWORD startType = 0;
        DWORD cfgNeeded = 0;
        QUERY_SERVICE_CONFIGW cfgBuf;
        LPQUERY_SERVICE_CONFIGW pCfg = &cfgBuf;

        if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                 (LPBYTE)&ssp, sizeof(ssp), &needed)) {
            printf("驱动服务:   已安装, 状态 = %s\n", ServiceStateText(ssp.dwCurrentState));
        }
        QueryServiceConfigW(svc, &cfgBuf, sizeof(cfgBuf), &cfgNeeded);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && cfgNeeded > 0) {
            pCfg = (LPQUERY_SERVICE_CONFIGW)HeapAlloc(GetProcessHeap(), 0, cfgNeeded);
        }
        if (pCfg && QueryServiceConfigW(svc, pCfg, cfgNeeded ? cfgNeeded : sizeof(cfgBuf), &cfgNeeded)) {
            startType = pCfg->dwStartType;
            printf("启动类型:   %s\n",
                   startType == SERVICE_AUTO_START ? "自动启动 (重启后自动加载)" :
                   startType == SERVICE_DEMAND_START ? "手动启动" :
                   startType == SERVICE_DISABLED ? "已禁用" :
                   startType == SERVICE_SYSTEM_START ? "系统启动" :
                   startType == SERVICE_BOOT_START ? "引导启动" : "其他");
            if (pCfg != &cfgBuf) {
                HeapFree(GetProcessHeap(), 0, pCfg);
            }
        }
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
    }

    {
        wchar_t dst[MAX_PATH * 2];
        if (GetDriverDestPath(dst, ARRAYSIZE(dst))) {
            printf("驱动文件:   %ls %s\n", dst,
                   GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES ? "(存在)" : "(缺失!)");
        }
    }

    printf("allow.txt:  %s\n",
           GetFileAttributesW(MK_ALLOW_FILE) != INVALID_FILE_ATTRIBUTES
               ? "存在 -> 放行模式 (不拦截)"
               : "不存在 -> 拦截模式 (拦截网易云/QQ音乐)");

    ts = IsTestSigningEnabled();
    printf("测试签名:   %s\n",
           ts == 1 ? "已启用" :
           ts == 0 ? "未启用 (mkctl testsign on 后重启才能加载测试签名驱动)" :
                     "查询失败");

    printf("======================================\n");
    return 0;
}

/* ------------------------------------------------------------------------ */
static int CmdUninstall(void)
{
    SC_HANDLE scm = NULL;
    SC_HANDLE svc;
    SERVICE_STATUS status;
    wchar_t dst[MAX_PATH * 2];
    int ret = 1;

    svc = OpenMkService(SERVICE_ALL_ACCESS, &scm);
    if (svc == NULL) {
        printf("[*] 服务不存在或无法打开, 尝试清理驱动文件...\n");
    } else {
        if (ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
            MkWaitServiceState(svc, SERVICE_STOPPED, WAIT_TIMEOUT_MS);
            printf("[*] 驱动已停止。\n");
        }
        if (!DeleteService(svc)) {
            PrintWin32Error("删除服务", GetLastError());
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return 1;
        }
        printf("[+] 驱动服务已删除 (下次重启后不再加载)。\n");
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
    }

    if (GetDriverDestPath(dst, ARRAYSIZE(dst))) {
        if (!DeleteFileW(dst)) {
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND) {
                /* 已不存在 */
            } else {
                PrintWin32Error("删除驱动文件", err);
                printf("    文件可能仍被占用, 重启后会自动解除占用, 可手动删除。\n");
                goto done;
            }
        }
        printf("[+] 驱动文件已删除: %ls\n", dst);
    }
    ret = 0;

done:
    printf("[*] 如需关闭测试签名模式: mkctl testsign off 后重启。\n");
    return ret;
}

/* ------------------------------------------------------------------------ */
static int CmdAllow(const wchar_t* sub)
{
    HANDLE h;

    if (sub == NULL || _wcsicmp(sub, L"status") == 0) {
        printf("allow.txt: %s\n",
               GetFileAttributesW(MK_ALLOW_FILE) != INVALID_FILE_ATTRIBUTES
                   ? "存在 -> 放行模式" : "不存在 -> 拦截模式");
        return 0;
    }
    if (_wcsicmp(sub, L"on") == 0) {
        h = CreateFileW(MK_ALLOW_FILE, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            PrintWin32Error("创建 C:\\allow.txt", GetLastError());
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
            PrintWin32Error("删除 C:\\allow.txt", GetLastError());
            return 1;
        }
        printf("[+] 已删除 C:\\allow.txt —— 拦截模式, 目标进程将被阻止启动。\n");
        return 0;
    }
    printf("[!] 未知参数。用法: mkctl allow on|off|status\n");
    return 1;
}

/* ------------------------------------------------------------------------ */
static int CmdTestsign(const wchar_t* sub)
{
    static char out[8192];
    DWORD rc;

    if (sub == NULL) {
        int ts = IsTestSigningEnabled();
        printf("测试签名: %s\n", ts == 1 ? "已启用" : ts == 0 ? "未启用" : "查询失败");
        return 0;
    }
    if (_wcsicmp(sub, L"on") == 0) {
        rc = RunCaptureW(L"bcdedit.exe /set testsigning on", out, (DWORD)sizeof(out));
        if (rc == 0) {
            printf("[+] 已开启测试签名模式, 请重启电脑后生效。\n");
            printf("    重启后执行 mkctl status 确认 “测试签名: 已启用”。\n");
        } else {
            printf("[!] 设置失败 (退出码 %lu)。\n", (unsigned long)rc);
            printf("    若提示 “被安全启动策略保护”, 请进 BIOS/UEFI 关闭 Secure Boot 后重试。\n");
        }
        return rc == 0 ? 0 : 1;
    }
    if (_wcsicmp(sub, L"off") == 0) {
        rc = RunCaptureW(L"bcdedit.exe /set testsigning off", out, (DWORD)sizeof(out));
        if (rc == 0) {
            printf("[+] 已关闭测试签名模式, 请重启电脑后生效。\n");
        } else {
            printf("[!] 设置失败 (退出码 %lu)。\n", (unsigned long)rc);
        }
        return rc == 0 ? 0 : 1;
    }
    printf("[!] 未知参数。用法: mkctl testsign on|off\n");
    return 1;
}

/* ------------------------------------------------------------------------ */
static int CmdSetup(const wchar_t* argPath)
{
    int ts = IsTestSigningEnabled();
    int r;

    printf("========== MusicKiller 一键安装 ==========\n");

    if (ts == 0) {
        printf("[!] 测试签名模式未启用, 正在尝试开启...\n");
        if (CmdTestsign(L"on") != 0) {
            printf("[!] 无法开启测试签名, 驱动将无法加载。请先解决后再 setup。\n");
            return 1;
        }
        printf("[!] 重要: 请先重启电脑, 重启后再运行 mkctl start 使驱动生效。\n");
        printf("    (服务已安装为自动启动, 以后每次开机都会自动加载驱动)\n");
    }

    r = CmdInstall(argPath);
    if (r != 0) {
        return r;
    }

    if (ts == 1) {
        r = CmdStart();
    } else {
        printf("[*] 重启前驱动无法加载, 请重启电脑 (重启后会自动加载, 无需再操作)。\n");
    }
    return r;
}

/* ------------------------------------------------------------------------ */
int wmain(int argc, wchar_t* argv[])
{
    int ret = 1;

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    printf("MusicKiller 驱动控制程序 v1.0 (目标: Windows 10 x64)\n\n");

    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    if (_wcsicmp(argv[1], L"setup") == 0) {
        ret = CmdSetup(argc >= 3 ? argv[2] : NULL);
    } else if (_wcsicmp(argv[1], L"install") == 0) {
        ret = CmdInstall(argc >= 3 ? argv[2] : NULL);
    } else if (_wcsicmp(argv[1], L"start") == 0) {
        ret = CmdStart();
    } else if (_wcsicmp(argv[1], L"stop") == 0) {
        ret = CmdStop();
    } else if (_wcsicmp(argv[1], L"restart") == 0) {
        CmdStop();
        ret = CmdStart();
    } else if (_wcsicmp(argv[1], L"status") == 0) {
        ret = CmdStatus();
    } else if (_wcsicmp(argv[1], L"uninstall") == 0) {
        ret = CmdUninstall();
    } else if (_wcsicmp(argv[1], L"allow") == 0) {
        ret = CmdAllow(argc >= 3 ? argv[2] : NULL);
    } else if (_wcsicmp(argv[1], L"testsign") == 0) {
        ret = CmdTestsign(argc >= 3 ? argv[2] : NULL);
    } else if (_wcsicmp(argv[1], L"help") == 0 ||
               _wcsicmp(argv[1], L"/?") == 0 ||
               _wcsicmp(argv[1], L"-h") == 0 ||
               _wcsicmp(argv[1], L"--help") == 0) {
        PrintUsage();
        ret = 0;
    } else {
        printf("[!] 未知命令: %ls\n\n", argv[1]);
        PrintUsage();
    }

    return ret;
}
