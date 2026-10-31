/*
 * zapret-service.exe: the "zapret" Windows service. Runs winws with the
 * options from winws.conf, and stops it when the service is stopped.
 *
 * Why not run winws.exe as the service directly: winws can read options from a
 * file (winws @file), but in the Windows build that goes through Cygwin's
 * wordexp(), which shells out to /bin/bash -- not present on a normal Windows
 * PC. So this launcher reads winws.conf itself and passes the options on the
 * command line. It also captures winws's output in
 * %ProgramData%\Zapret\winws.log, which the tray app shows when winws fails.
 *
 * Layout (next to this exe):
 *   winws.conf          options, one or more lines; lines starting with # are comments
 *   winws\winws.exe     zapret's Windows engine
 *
 * Run it from an administrator command prompt (with the service stopped) to
 * see winws's output live; Ctrl+C stops it.
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <wchar.h>

#define SERVICE_NAME L"zapret"
#define MAX_CMDLINE  32767
#define STOP_WAIT_MS 5000

static SERVICE_STATUS_HANDLE g_status_handle;
static SERVICE_STATUS g_status;
static HANDLE g_stop_event;
static HANDLE g_log = INVALID_HANDLE_VALUE;

/* ------------------------------------------------------------------------ */
/* Output                                                                    */
/* ------------------------------------------------------------------------ */

static void out(const WCHAR *fmt, ...)
{
    WCHAR wbuf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(wbuf, ARRAYSIZE(wbuf), fmt, ap);
    va_end(ap);
    wbuf[ARRAYSIZE(wbuf) - 1] = 0;

    char buf[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, sizeof(buf) - 2, NULL, NULL);
    if (n <= 0)
        return;
    n--;  /* drop the terminator */
    buf[n++] = '\r';
    buf[n++] = '\n';
    HANDLE h = g_log != INVALID_HANDLE_VALUE ? g_log : GetStdHandle(STD_ERROR_HANDLE);
    DWORD put;
    WriteFile(h, buf, (DWORD)n, &put, NULL);
}

static void out_error(const WCHAR *what, DWORD err)
{
    WCHAR msg[512] = L"";
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0,
                   msg, ARRAYSIZE(msg), NULL);
    for (size_t i = wcslen(msg); i > 0 && (msg[i - 1] == L'\r' || msg[i - 1] == L'\n'); i--)
        msg[i - 1] = 0;
    out(L"zapret-service: %ls: %ls (error %lu)", what, msg, (unsigned long)err);
}

/* %ProgramData%\Zapret\winws.log, truncated at each start. Readable by users,
 * so the tray app can show it. */
static HANDLE open_log(void)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH + 16];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, dir)))
        return INVALID_HANDLE_VALUE;
    wcsncat(dir, L"\\Zapret", MAX_PATH - wcslen(dir) - 1);
    CreateDirectoryW(dir, NULL);
    _snwprintf(path, ARRAYSIZE(path), L"%ls\\winws.log", dir);
    path[ARRAYSIZE(path) - 1] = 0;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};  /* inheritable: winws writes to it */
    return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* ------------------------------------------------------------------------ */
/* winws                                                                     */
/* ------------------------------------------------------------------------ */

/* Directory of this exe, without a trailing backslash. */
static BOOL exe_dir(WCHAR *dir, size_t cap)
{
    DWORD n = GetModuleFileNameW(NULL, dir, (DWORD)cap);
    if (!n || n >= cap)
        return FALSE;
    WCHAR *slash = wcsrchr(dir, L'\\');
    if (!slash)
        return FALSE;
    *slash = 0;
    return TRUE;
}

/* Split text into lines in place: returns the next non-empty line, or NULL.
 * (wcstok's signature differs between mingw-w64 versions, so avoid it.) */
static WCHAR *next_line(WCHAR **cursor)
{
    WCHAR *s = *cursor;
    if (!s)
        return NULL;
    while (*s == L'\r' || *s == L'\n')
        s++;
    if (!*s) {
        *cursor = NULL;
        return NULL;
    }
    WCHAR *e = s + wcscspn(s, L"\r\n");
    *cursor = *e ? e + 1 : NULL;
    *e = 0;
    return s;
}

/* Append the options from winws.conf to cmd: lines are joined with spaces,
 * blank lines and lines starting with # are skipped. */
static DWORD read_options(const WCHAR *conf, WCHAR *cmd, size_t cap)
{
    HANDLE h = CreateFileW(conf, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError();
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 256 * 1024) {
        CloseHandle(h);
        return ERROR_FILE_TOO_LARGE;
    }
    DWORD len = (DWORD)size.QuadPart, got = 0;
    char *raw = HeapAlloc(GetProcessHeap(), 0, len + 1);
    WCHAR *text = HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
    DWORD err = 0;
    if (!raw || !text || !ReadFile(h, raw, len, &got, NULL)) {
        err = raw && text ? GetLastError() : ERROR_OUTOFMEMORY;
        goto done;
    }
    raw[got] = 0;
    char *start = raw;
    if (got >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB
        && (unsigned char)raw[2] == 0xBF)
        start += 3;  /* UTF-8 BOM, as Notepad may write */
    if (!MultiByteToWideChar(CP_UTF8, 0, start, -1, text, (int)len + 1)) {
        err = GetLastError();
        goto done;
    }

    WCHAR *cursor = text;
    for (WCHAR *line; (line = next_line(&cursor)) != NULL;) {
        while (*line == L' ' || *line == L'\t')
            line++;
        size_t n = wcslen(line);
        while (n && (line[n - 1] == L' ' || line[n - 1] == L'\t'))
            line[--n] = 0;
        if (!n || *line == L'#')
            continue;
        if (wcslen(cmd) + 1 + n + 1 > cap) {
            err = ERROR_BUFFER_OVERFLOW;
            goto done;
        }
        wcscat(cmd, L" ");
        wcscat(cmd, line);
    }

done:
    if (raw) HeapFree(GetProcessHeap(), 0, raw);
    if (text) HeapFree(GetProcessHeap(), 0, text);
    CloseHandle(h);
    return err;
}

/* Run winws until it exits or stop_event is set.
 * Returns 0 if stopped on request, otherwise a nonzero code: winws's own exit
 * code, or a Windows error if it could not be started. *started tells which. */
static DWORD run_winws(HANDLE stop_event, BOOL service, BOOL *started)
{
    *started = FALSE;
    WCHAR dir[MAX_PATH], exe[MAX_PATH + 32], winws_dir[MAX_PATH + 16], conf[MAX_PATH + 16];
    if (!exe_dir(dir, ARRAYSIZE(dir)))
        return ERROR_PATH_NOT_FOUND;
    _snwprintf(winws_dir, ARRAYSIZE(winws_dir), L"%ls\\winws", dir);
    _snwprintf(exe, ARRAYSIZE(exe), L"%ls\\winws\\winws.exe", dir);
    _snwprintf(conf, ARRAYSIZE(conf), L"%ls\\winws.conf", dir);

    WCHAR *cmd = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, MAX_CMDLINE * sizeof(WCHAR));
    if (!cmd)
        return ERROR_OUTOFMEMORY;
    _snwprintf(cmd, MAX_CMDLINE, L"\"%ls\"", exe);
    DWORD err = read_options(conf, cmd, MAX_CMDLINE);
    if (err) {
        out_error(conf, err);
        HeapFree(GetProcessHeap(), 0, cmd);
        return err;
    }
    out(L"zapret-service: starting %ls", cmd);

    /* Kill winws if this launcher dies, so it never runs unsupervised. */
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }

    STARTUPINFOW si = {.cb = sizeof(si)};
    BOOL inherit = FALSE;
    if (service && g_log != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = NULL;
        si.hStdOutput = g_log;
        si.hStdError = g_log;
        inherit = TRUE;
    }
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, inherit,
                        CREATE_SUSPENDED | (service ? CREATE_NO_WINDOW : 0),
                        NULL, winws_dir, &si, &pi)) {
        err = GetLastError();
        out_error(err == ERROR_ELEVATION_REQUIRED ? L"winws needs an administrator prompt" : exe, err);
        HeapFree(GetProcessHeap(), 0, cmd);
        if (job) CloseHandle(job);
        return err;
    }
    HeapFree(GetProcessHeap(), 0, cmd);
    if (job)
        AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    *started = TRUE;

    if (service) {
        g_status.dwCurrentState = SERVICE_RUNNING;
        g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
        g_status.dwWaitHint = 0;
        SetServiceStatus(g_status_handle, &g_status);
    }

    HANDLE waits[2] = {pi.hProcess, stop_event};
    DWORD which = WaitForMultipleObjects(stop_event ? 2 : 1, waits, FALSE, INFINITE);
    DWORD code = 0;
    if (which == WAIT_OBJECT_0 + 1) {
        /* Stop requested. Ending the process closes its WinDivert handle, which
         * removes the packet filter. */
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, STOP_WAIT_MS);
        out(L"zapret-service: stopped");
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
        out(L"zapret-service: winws exited with code %lu", (unsigned long)code);
        if (!code)
            code = ERROR_PROCESS_ABORTED;  /* it should never exit on its own */
    }
    CloseHandle(pi.hProcess);
    if (job)
        CloseHandle(job);
    return code;
}

/* ------------------------------------------------------------------------ */
/* Service plumbing                                                          */
/* ------------------------------------------------------------------------ */

static DWORD WINAPI control_handler(DWORD control, DWORD type, LPVOID data, LPVOID ctx)
{
    (void)type;
    (void)data;
    (void)ctx;
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_status.dwCurrentState = SERVICE_STOP_PENDING;
        g_status.dwControlsAccepted = 0;
        g_status.dwWaitHint = STOP_WAIT_MS + 1000;
        SetServiceStatus(g_status_handle, &g_status);
        SetEvent(g_stop_event);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void WINAPI service_main(DWORD argc, LPWSTR *argv)
{
    (void)argc;
    (void)argv;
    g_status_handle = RegisterServiceCtrlHandlerExW(SERVICE_NAME, control_handler, NULL);
    if (!g_status_handle)
        return;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = SERVICE_START_PENDING;
    g_status.dwWaitHint = 5000;
    SetServiceStatus(g_status_handle, &g_status);

    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_log = open_log();

    BOOL started;
    DWORD code = run_winws(g_stop_event, TRUE, &started);

    /* winws's own exit codes are reported as service-specific errors, so the
     * tray app can tell them apart from Windows errors. */
    g_status.dwCurrentState = SERVICE_STOPPED;
    g_status.dwControlsAccepted = 0;
    g_status.dwWaitHint = 0;
    g_status.dwWin32ExitCode = code && started ? ERROR_SERVICE_SPECIFIC_ERROR : code;
    g_status.dwServiceSpecificExitCode = started ? code : 0;
    if (g_log != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
    SetServiceStatus(g_status_handle, &g_status);
}

static BOOL WINAPI console_handler(DWORD type)
{
    (void)type;
    return TRUE;  /* winws shares our console and gets Ctrl+C itself */
}

int wmain(void)
{
    SERVICE_TABLE_ENTRYW table[] = {
        {(LPWSTR)SERVICE_NAME, service_main},
        {NULL, NULL},
    };
    if (StartServiceCtrlDispatcherW(table))
        return 0;
    if (GetLastError() != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
        return 1;

    /* Started by hand: run in the foreground for troubleshooting. */
    SetConsoleCtrlHandler(console_handler, TRUE);
    BOOL started;
    return (int)run_winws(NULL, FALSE, &started);
}
