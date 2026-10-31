/*
 * zapret-tray for Windows: one toggle for zapret, in the tray and a small window.
 *
 * Windows port of the GTK app in src/zapret_tray. The design is the same:
 *
 *   Linux                               Windows
 *   zapret.service (systemd)       ->   "zapret" Windows service running winws.exe
 *   polkit rule for the user       ->   service DACL granting the user start/stop
 *   StartUnit/StopUnit over D-Bus  ->   StartService/ControlService (SCM API)
 *   ~/.config/autostart entry      ->   HKCU\...\CurrentVersion\Run value
 *   ~/.config/zapret-tray/enabled  ->   %APPDATA%\zapret-tray\enabled
 *
 * The app itself never needs administrator rights: the MSI installer
 * (windows/zapret.wxs) creates the service and grants the installing user
 * permission to start and stop it.
 *
 * Controls: left-click the tray icon opens the window, right-click opens the
 * menu, middle-click flips zapret on/off. Closing the window keeps the app in
 * the tray. All controls follow the service's real state via a short poll, so
 * changes made elsewhere (services.msc, sc.exe) show up too.
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
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>

#include "resource.h"

#define SERVICE_NAME   L"zapret"
#define WINDOW_CLASS   L"org.zapret.Tray"
#define MUTEX_NAME     L"Local\\org.zapret.Tray"
#define RUN_KEY        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define APPROVED_KEY   L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run"
#define RUN_VALUE      L"ZapretTray"

#define POLL_MS        3000
#define SETTLE_MS      15000   /* max wait for the service to reach the target state */
#define CONFIRM_MS     1500    /* winws exits quickly on a bad config; wait to catch it */

#define WM_APP_TRAY    (WM_APP + 1)   /* tray icon callback */
#define WM_APP_DONE    (WM_APP + 2)   /* worker finished: wParam = wanted state, lParam = error */
#define WM_APP_SHOW    (WM_APP + 3)   /* second instance asks us to show the window */

#define IDC_TITLE      101
#define IDC_SWITCH     102
#define IDC_STATUS     103
#define IDC_AUTOSTART  104

#define IDM_TOGGLE     201
#define IDM_AUTOSTART  202
#define IDM_OPEN       203
#define IDM_QUIT       204

#define TIMER_POLL     1
#define TRAY_ID        1

/* Custom error (bit 29 marks application-defined codes): winws died at start. */
#define APP_ERR_EXITED 0x20000001u

#define ELLIPSIS L"\u2026"

enum svc_state { SVC_UNKNOWN = -1, SVC_OFF = 0, SVC_ON = 1, SVC_MISSING = 2 };

static HINSTANCE g_inst;
static HWND g_hwnd, g_title, g_switch, g_status, g_autostart;
static HFONT g_font, g_title_font;
static HICON g_icon_on, g_icon_off;
static UINT g_dpi = 96;
static UINT g_msg_taskbar_created;
static BOOL g_tray_added;
static BOOL g_hinted_tray;

static int g_state = SVC_UNKNOWN;
static BOOL g_busy;           /* a start/stop request is in flight */
static BOOL g_want;           /* what that request is aiming for */
static BOOL g_quiet;          /* don't report errors for this request (startup restore) */
static WCHAR g_error[1024];    /* detail text from the worker, if any */

static int scale(int v) { return MulDiv(v, (int)g_dpi, 96); }

/* ------------------------------------------------------------------------ */
/* zapret service                                                            */
/* ------------------------------------------------------------------------ */

static int service_query(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm)
        return SVC_OFF;
    int result = SVC_OFF;
    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_QUERY_STATUS);
    if (!svc) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
            result = SVC_MISSING;
    } else {
        SERVICE_STATUS st;
        /* Treat transitional "coming up" states as on, so the switch doesn't
         * flicker off while the service is still starting. */
        if (QueryServiceStatus(svc, &st) &&
            (st.dwCurrentState == SERVICE_RUNNING ||
             st.dwCurrentState == SERVICE_START_PENDING ||
             st.dwCurrentState == SERVICE_CONTINUE_PENDING))
            result = SVC_ON;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return result;
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

/* Last few lines winws printed, from the log zapret-service.exe keeps in
 * %ProgramData%\Zapret\winws.log. Skips the launcher's own bookkeeping lines.
 * Returns FALSE if there is nothing useful. */
static BOOL winws_log_tail(WCHAR *buf, size_t cap)
{
    WCHAR path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, path)))
        return FALSE;
    wcsncat(path, L"\\Zapret\\winws.log", MAX_PATH - wcslen(path) - 1);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    char raw[4096];
    DWORD got = 0;
    LARGE_INTEGER size, pos;
    if (GetFileSizeEx(h, &size) && size.QuadPart > (LONGLONG)sizeof(raw) - 1) {
        pos.QuadPart = size.QuadPart - (LONGLONG)sizeof(raw) + 1;
        SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
    }
    BOOL ok = ReadFile(h, raw, sizeof(raw) - 1, &got, NULL);
    CloseHandle(h);
    if (!ok || !got)
        return FALSE;
    raw[got] = 0;
    WCHAR text[4096];
    if (!MultiByteToWideChar(CP_UTF8, 0, raw, -1, text, ARRAYSIZE(text)))
        return FALSE;

    /* Keep the last 4 interesting lines. */
    const WCHAR *lines[4];
    int count = 0;
    WCHAR *cursor = text;
    for (WCHAR *line; (line = next_line(&cursor)) != NULL;) {
        if (!*line || wcsncmp(line, L"zapret-service: starting", 24) == 0
            || wcsncmp(line, L"zapret-service: winws exited", 28) == 0)
            continue;
        if (count == 4) {
            memmove(lines, lines + 1, 3 * sizeof(lines[0]));
            count = 3;
        }
        lines[count++] = line;
    }
    if (!count)
        return FALSE;
    buf[0] = 0;
    for (int i = 0; i < count; i++) {
        if (i)
            wcsncat(buf, L"\n", cap - wcslen(buf) - 1);
        wcsncat(buf, lines[i], cap - wcslen(buf) - 1);
    }
    return TRUE;
}

/* Wait until the service reaches the wanted state. Returns 0 or an error. */
static DWORD service_settle(SC_HANDLE svc, BOOL want)
{
    DWORD start = GetTickCount();
    BOOL confirming = FALSE;
    for (;;) {
        SERVICE_STATUS_PROCESS sp;
        DWORD needed;
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&sp,
                                  sizeof(sp), &needed))
            return GetLastError();

        if (want && sp.dwCurrentState == SERVICE_RUNNING) {
            if (confirming)
                return 0;
            confirming = TRUE;          /* still running a moment later? */
            Sleep(CONFIRM_MS);
            continue;
        }
        if (sp.dwCurrentState == SERVICE_STOPPED) {
            if (!want)
                return 0;
            DWORD code = sp.dwWin32ExitCode == ERROR_SERVICE_SPECIFIC_ERROR
                             ? sp.dwServiceSpecificExitCode
                             : sp.dwWin32ExitCode;
            WCHAR tail[600];
            if (winws_log_tail(tail, ARRAYSIZE(tail)))
                _snwprintf(g_error, ARRAYSIZE(g_error),
                           L"winws stopped right after starting:\n\n%ls\n\n"
                           L"Check the options in winws.conf next to zapret-tray.exe.",
                           tail);
            else
                _snwprintf(g_error, ARRAYSIZE(g_error),
                           L"winws stopped right after starting (exit code %lu). "
                           L"Check the options in winws.conf next to zapret-tray.exe.",
                           (unsigned long)code);
            g_error[ARRAYSIZE(g_error) - 1] = 0;
            return APP_ERR_EXITED;
        }
        if (GetTickCount() - start > SETTLE_MS)
            return ERROR_SERVICE_REQUEST_TIMEOUT;
        Sleep(250);
    }
}

static DWORD service_set(BOOL want)
{
    g_error[0] = 0;
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm)
        return GetLastError();
    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME,
                                 SERVICE_QUERY_STATUS | (want ? SERVICE_START : SERVICE_STOP));
    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        return err;
    }

    DWORD err = 0;
    if (want) {
        if (!StartServiceW(svc, 0, NULL)) {
            err = GetLastError();
            if (err == ERROR_SERVICE_ALREADY_RUNNING)
                err = 0;
        }
    } else {
        SERVICE_STATUS st;
        if (!ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
            err = GetLastError();
            if (err == ERROR_SERVICE_NOT_ACTIVE)
                err = 0;
        }
    }
    if (!err)
        err = service_settle(svc, want);

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return err;
}

static DWORD WINAPI service_worker(LPVOID arg)
{
    BOOL want = (BOOL)(INT_PTR)arg;
    DWORD err = service_set(want);
    PostMessageW(g_hwnd, WM_APP_DONE, (WPARAM)want, (LPARAM)err);
    return 0;
}

static void describe_error(DWORD err, WCHAR *buf, size_t cap)
{
    if (g_error[0]) {
        lstrcpynW(buf, g_error, (int)cap);
    } else if (err == ERROR_ACCESS_DENIED) {
        lstrcpynW(buf, L"Permission denied. Reinstall Zapret (run the .msi again) to let "
                       L"your account start and stop the zapret service.", (int)cap);
    } else if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
        lstrcpynW(buf, L"The zapret service is not installed. Run the Zapret installer (.msi).", (int)cap);
    } else if (!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               NULL, err, 0, buf, (DWORD)cap, NULL)) {
        _snwprintf(buf, cap, L"Error %lu.", (unsigned long)err);
        buf[cap - 1] = 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Installer helpers (run by the MSI as SYSTEM, never by the user)           */
/* ------------------------------------------------------------------------ */

/* --grant-service-access SID: add an entry to the zapret service's DACL that
 * lets this user query, start and stop it -- the Windows counterpart of the
 * Linux polkit rule. Existing entries are kept; running it twice is harmless. */
static int grant_service_access(const WCHAR *sid_str)
{
    PSID sid = NULL;
    if (!ConvertStringSidToSidW(sid_str, &sid))
        return 2;  /* not a SID: refuse rather than write a bogus ACE */
    LocalFree(sid);

    int rc = 3;
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, SERVICE_NAME, READ_CONTROL | WRITE_DAC) : NULL;
    PSECURITY_DESCRIPTOR old_sd = NULL, new_sd = NULL;
    LPWSTR old_sddl = NULL;
    WCHAR *new_sddl = NULL;
    DWORD need = 0;
    if (!svc)
        goto out;

    QueryServiceObjectSecurity(svc, DACL_SECURITY_INFORMATION, NULL, 0, &need);
    old_sd = LocalAlloc(LPTR, need ? need : 1);
    if (!old_sd || !QueryServiceObjectSecurity(svc, DACL_SECURITY_INFORMATION, old_sd, need, &need))
        goto out;
    if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(
            old_sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &old_sddl, NULL))
        goto out;

    /* CC query config, LC query status, RP start, WP stop, LO interrogate, RC read */
    WCHAR ace[256];
    _snwprintf(ace, ARRAYSIZE(ace), L"(A;;CCLCRPWPLORC;;;%ls)", sid_str);
    ace[ARRAYSIZE(ace) - 1] = 0;
    if (wcsstr(old_sddl, ace)) {
        rc = 0;
        goto out;
    }
    size_t len = wcslen(old_sddl) + wcslen(ace) + 1;
    new_sddl = LocalAlloc(LPTR, len * sizeof(WCHAR));
    if (!new_sddl)
        goto out;
    _snwprintf(new_sddl, len, L"%ls%ls", old_sddl, ace);
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(new_sddl, SDDL_REVISION_1, &new_sd, NULL)
        && SetServiceObjectSecurity(svc, DACL_SECURITY_INFORMATION, new_sd))
        rc = 0;

out:
    if (new_sd) LocalFree(new_sd);
    if (new_sddl) LocalFree(new_sddl);
    if (old_sddl) LocalFree(old_sddl);
    if (old_sd) LocalFree(old_sd);
    if (svc) CloseServiceHandle(svc);
    if (scm) CloseServiceHandle(scm);
    return rc;
}

/* --close-running: end every other zapret-tray.exe (any user) so the
 * uninstaller can delete the file. */
static int close_running(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W pe = {.dwSize = sizeof(pe)};
    DWORD self = GetCurrentProcessId();
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == self || lstrcmpiW(pe.szExeFile, L"zapret-tray.exe") != 0)
            continue;
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
        if (h) {
            TerminateProcess(h, 0);
            WaitForSingleObject(h, 5000);
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Start at login + remembered state                                         */
/* ------------------------------------------------------------------------ */

static BOOL autostart_is_enabled(void)
{
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ,
                     NULL, NULL, NULL) != ERROR_SUCCESS)
        return FALSE;
    /* Task Manager's "Disable" leaves the Run value but flags it here; an odd
     * first byte means disabled. */
    BYTE flag[12];
    DWORD size = sizeof(flag);
    if (RegGetValueW(HKEY_CURRENT_USER, APPROVED_KEY, RUN_VALUE, RRF_RT_REG_BINARY,
                     NULL, flag, &size) == ERROR_SUCCESS && size > 0 && (flag[0] & 1))
        return FALSE;
    return TRUE;
}

static void autostart_set(BOOL enabled)
{
    /* Clearing the Task Manager flag makes our setting the one that counts. */
    RegDeleteKeyValueW(HKEY_CURRENT_USER, APPROVED_KEY, RUN_VALUE);
    if (!enabled) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE);
        return;
    }
    WCHAR exe[MAX_PATH], cmd[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
    if (!n || n >= ARRAYSIZE(exe))
        return;
    _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" --hidden", exe);
    cmd[ARRAYSIZE(cmd) - 1] = 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, REG_SZ, cmd,
                    (DWORD)((wcslen(cmd) + 1) * sizeof(WCHAR)));
}

static BOOL state_path(WCHAR *dir, WCHAR *file)
{
    WCHAR base[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, base)))
        return FALSE;
    _snwprintf(dir, MAX_PATH, L"%ls\\zapret-tray", base);
    _snwprintf(file, MAX_PATH, L"%ls\\zapret-tray\\enabled", base);
    dir[MAX_PATH - 1] = file[MAX_PATH - 1] = 0;
    return TRUE;
}

/* Returns 1/0 for the saved choice, or -1 if it was never saved. */
static int state_load(void)
{
    WCHAR dir[MAX_PATH], file[MAX_PATH];
    if (!state_path(dir, file))
        return -1;
    HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    char c = 0;
    DWORD got = 0;
    BOOL ok = ReadFile(h, &c, 1, &got, NULL);
    CloseHandle(h);
    return (ok && got == 1) ? (c == '1') : -1;
}

static void state_save(BOOL enabled)
{
    WCHAR dir[MAX_PATH], file[MAX_PATH], tmp[MAX_PATH + 4];
    if (!state_path(dir, file))
        return;
    CreateDirectoryW(dir, NULL);
    _snwprintf(tmp, ARRAYSIZE(tmp), L"%ls.tmp", file);
    tmp[ARRAYSIZE(tmp) - 1] = 0;
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD put;
    BOOL ok = WriteFile(h, enabled ? "1" : "0", 1, &put, NULL);
    CloseHandle(h);
    if (ok)
        MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING);
}

/* ------------------------------------------------------------------------ */
/* Tray icon                                                                 */
/* ------------------------------------------------------------------------ */

static const WCHAR *state_text(BOOL for_menu)
{
    if (g_busy)
        return g_want ? L"Turning on" ELLIPSIS : L"Turning off" ELLIPSIS;
    switch (g_state) {
    case SVC_ON:      return for_menu ? L"zapret: on" : L"Active";
    case SVC_OFF:     return for_menu ? L"zapret: off" : L"Inactive";
    case SVC_MISSING: return for_menu ? L"zapret: not installed"
                                      : L"Service not installed \u2014 run the installer";
    default:          return ELLIPSIS;
    }
}

static void tray_fill(NOTIFYICONDATAW *nid)
{
    ZeroMemory(nid, sizeof(*nid));
    nid->cbSize = sizeof(*nid);
    nid->hWnd = g_hwnd;
    nid->uID = TRAY_ID;
}

static void tray_load_icons(void)
{
    if (g_icon_on) DestroyIcon(g_icon_on);
    if (g_icon_off) DestroyIcon(g_icon_off);
    g_icon_on = g_icon_off = NULL;
    LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_ON), LIM_SMALL, &g_icon_on);
    LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_OFF), LIM_SMALL, &g_icon_off);
}

static void tray_update(BOOL add)
{
    NOTIFYICONDATAW nid;
    tray_fill(&nid);
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    BOOL on = g_busy ? g_want : g_state == SVC_ON;
    nid.hIcon = on ? g_icon_on : g_icon_off;
    const WCHAR *tip = L"Zapret";
    if (!g_busy && g_state == SVC_ON)
        tip = L"Zapret \u2014 on";
    else if (!g_busy && g_state == SVC_OFF)
        tip = L"Zapret \u2014 off";
    else if (!g_busy && g_state == SVC_MISSING)
        tip = L"Zapret \u2014 not installed";
    lstrcpynW(nid.szTip, tip, ARRAYSIZE(nid.szTip));

    if (add) {
        g_tray_added = Shell_NotifyIconW(NIM_ADD, &nid);
        if (g_tray_added) {
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
        }
    } else if (g_tray_added) {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

static void tray_remove(void)
{
    if (!g_tray_added)
        return;
    NOTIFYICONDATAW nid;
    tray_fill(&nid);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_tray_added = FALSE;
}

static void notify(const WCHAR *title, const WCHAR *body, BOOL error)
{
    /* An error while the window is in front deserves a dialog, not a toast. */
    if (!g_tray_added || (error && IsWindowVisible(g_hwnd))) {
        MessageBoxW(g_hwnd, body, title, MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
        return;
    }
    NOTIFYICONDATAW nid;
    tray_fill(&nid);
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = error ? NIIF_ERROR : NIIF_INFO;
    lstrcpynW(nid.szInfoTitle, title, ARRAYSIZE(nid.szInfoTitle));
    lstrcpynW(nid.szInfo, body, ARRAYSIZE(nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

/* ------------------------------------------------------------------------ */
/* UI state                                                                  */
/* ------------------------------------------------------------------------ */

static BOOL can_toggle(void)
{
    return !g_busy && (g_state == SVC_ON || g_state == SVC_OFF);
}

static void update_ui(void)
{
    SetWindowTextW(g_status, state_text(FALSE));
    EnableWindow(g_switch, can_toggle());
    InvalidateRect(g_switch, NULL, TRUE);
    tray_update(FALSE);
}

static void apply_autostart_ui(void)
{
    Button_SetCheck(g_autostart, autostart_is_enabled() ? BST_CHECKED : BST_UNCHECKED);
}

static void refresh(BOOL force)
{
    int s = service_query();
    if (force || s != g_state) {
        g_state = s;
        update_ui();
    }
}

static void set_active(BOOL want, BOOL quiet)
{
    if (g_busy)
        return;
    g_busy = TRUE;
    g_want = want;
    g_quiet = quiet;
    update_ui();
    HANDLE th = CreateThread(NULL, 0, service_worker, (LPVOID)(INT_PTR)want, 0, NULL);
    if (th) {
        CloseHandle(th);
    } else {
        g_busy = FALSE;
        refresh(TRUE);
    }
}

static void on_done(BOOL want, DWORD err)
{
    g_busy = FALSE;
    if (!err) {
        state_save(want);  /* restored at next startup */
        g_state = want ? SVC_ON : SVC_OFF;
        update_ui();
        return;
    }
    refresh(TRUE);
    if (!g_quiet) {
        WCHAR msg[512];
        describe_error(err, msg, ARRAYSIZE(msg));
        notify(want ? L"Could not enable zapret" : L"Could not disable zapret", msg, TRUE);
    }
}

/* At startup, bring zapret to the user's last saved on/off choice. */
static void restore_saved_state(void)
{
    int saved = state_load();
    if (saved < 0 || (g_state != SVC_ON && g_state != SVC_OFF))
        return;
    if (saved != (g_state == SVC_ON))
        set_active(saved, TRUE);
}

/* ------------------------------------------------------------------------ */
/* Window                                                                    */
/* ------------------------------------------------------------------------ */

static UINT dpi_for_window(HWND hwnd)
{
    typedef UINT (WINAPI *fn_t)(HWND);
    static fn_t fn;
    static BOOL looked;
    if (!looked) {
        fn = (fn_t)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        looked = TRUE;
    }
    if (fn && hwnd)
        return fn(hwnd);
    HDC dc = GetDC(NULL);
    UINT dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    return dpi;
}

static void adjust_rect(RECT *rc, DWORD style, DWORD ex)
{
    typedef BOOL (WINAPI *fn_t)(LPRECT, DWORD, BOOL, DWORD, UINT);
    fn_t fn = (fn_t)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                           "AdjustWindowRectExForDpi");
    if (fn)
        fn(rc, style, FALSE, ex, g_dpi);
    else
        AdjustWindowRectEx(rc, style, FALSE, ex);
}

static void make_fonts(void)
{
    if (g_font) DeleteObject(g_font);
    if (g_title_font) DeleteObject(g_title_font);
    g_font = CreateFontW(-MulDiv(9, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                         DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_title_font = CreateFontW(-MulDiv(15, (int)g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                               DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

/* Client size at 96 DPI. */
#define CLIENT_W 300
#define CLIENT_H 190

static void layout(void)
{
    int w = scale(CLIENT_W);
    SendMessageW(g_title, WM_SETFONT, (WPARAM)g_title_font, TRUE);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_autostart, WM_SETFONT, (WPARAM)g_font, TRUE);

    /* Size the checkbox to its label so it centers properly. */
    HDC dc = GetDC(g_autostart);
    HGDIOBJ old = SelectObject(dc, g_font);
    SIZE ext = {0, 0};
    GetTextExtentPoint32W(dc, L"Start at login", 14, &ext);
    SelectObject(dc, old);
    ReleaseDC(g_autostart, dc);
    int cb_w = ext.cx + scale(24);

    MoveWindow(g_title, 0, scale(22), w, scale(32), TRUE);
    MoveWindow(g_switch, (w - scale(64)) / 2, scale(66), scale(64), scale(32), TRUE);
    MoveWindow(g_status, scale(8), scale(106), w - scale(16), scale(20), TRUE);
    MoveWindow(g_autostart, (w - cb_w) / 2, scale(142), cb_w, scale(24), TRUE);
}

static void resize_to_dpi(const RECT *suggested)
{
    DWORD style = (DWORD)GetWindowLongW(g_hwnd, GWL_STYLE);
    DWORD ex = (DWORD)GetWindowLongW(g_hwnd, GWL_EXSTYLE);
    RECT rc = {0, 0, scale(CLIENT_W), scale(CLIENT_H)};
    adjust_rect(&rc, style, ex);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    if (suggested) {
        SetWindowPos(g_hwnd, NULL, suggested->left, suggested->top, ww, wh,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        /* Center on the work area of the monitor the window is on. */
        MONITORINFO mi = {.cbSize = sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        RECT wa = mi.rcWork;
        SetWindowPos(g_hwnd, NULL, wa.left + (wa.right - wa.left - ww) / 2,
                     wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* An on/off switch, drawn 4x oversized and scaled down for smooth edges. */
static void draw_switch(const DRAWITEMSTRUCT *di)
{
    const int k = 4;
    RECT rc = di->rcItem;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    BOOL on = g_busy ? g_want : g_state == SVC_ON;
    BOOL enabled = can_toggle();

    HDC mem = CreateCompatibleDC(di->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(di->hDC, w * k, h * k);
    HGDIOBJ old_bmp = SelectObject(mem, bmp);
    RECT all = {0, 0, w * k, h * k};
    FillRect(mem, &all, GetSysColorBrush(COLOR_WINDOW));

    COLORREF accent = enabled ? RGB(0x00, 0x67, 0xC0) : RGB(0x9C, 0xB8, 0xD4);
    COLORREF grey = enabled ? RGB(0x5F, 0x5F, 0x5F) : RGB(0xB8, 0xB8, 0xB8);
    COLORREF track_fill = on ? accent : GetSysColor(COLOR_WINDOW);
    COLORREF track_edge = on ? accent : grey;
    COLORREF knob = on ? RGB(0xFF, 0xFF, 0xFF) : grey;

    int m = scale(4) * k;                         /* margin for the focus ring */
    int th = h * k - 2 * m;                       /* track height */
    int tw = th * 2;                              /* track width */
    int tx = (w * k - tw) / 2, ty = m;
    int pen_w = scale(1) * k + k / 2;

    HPEN pen = CreatePen(PS_SOLID, pen_w, track_edge);
    HBRUSH brush = CreateSolidBrush(track_fill);
    HGDIOBJ old_pen = SelectObject(mem, pen);
    HGDIOBJ old_brush = SelectObject(mem, brush);
    RoundRect(mem, tx, ty, tx + tw, ty + th, th, th);

    int pad = th / 4;
    int d = th - 2 * pad;
    int kx = on ? tx + tw - pad - d : tx + pad;
    HBRUSH knob_brush = CreateSolidBrush(knob);
    SelectObject(mem, GetStockObject(NULL_PEN));
    SelectObject(mem, knob_brush);
    Ellipse(mem, kx, ty + pad, kx + d, ty + pad + d);

    SetStretchBltMode(di->hDC, HALFTONE);
    SetBrushOrgEx(di->hDC, 0, 0, NULL);
    StretchBlt(di->hDC, rc.left, rc.top, w, h, mem, 0, 0, w * k, h * k, SRCCOPY);

    if ((di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT)) {
        RECT f = {tx / k - scale(2), ty / k - scale(2),
                  (tx + tw) / k + scale(2), (ty + th) / k + scale(2)};
        OffsetRect(&f, rc.left, rc.top);
        DrawFocusRect(di->hDC, &f);
    }

    SelectObject(mem, old_pen);
    SelectObject(mem, old_brush);
    SelectObject(mem, old_bmp);
    DeleteObject(pen);
    DeleteObject(brush);
    DeleteObject(knob_brush);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static void show_window(void)
{
    apply_autostart_ui();
    refresh(FALSE);
    ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_switch);
}

static void hide_window(void)
{
    if (!g_tray_added) {
        DestroyWindow(g_hwnd);  /* no tray to come back from: closing means quit */
        return;
    }
    ShowWindow(g_hwnd, SW_HIDE);
    if (!g_hinted_tray) {
        g_hinted_tray = TRUE;
        notify(L"Zapret is still running",
               L"Toggle it from the tray icon, or choose Quit in its menu.", FALSE);
    }
}

static void toggle_autostart(void)
{
    autostart_set(!autostart_is_enabled());
    apply_autostart_ui();
}

static void show_menu(int x, int y)
{
    refresh(FALSE);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, state_text(TRUE));
    AppendMenuW(m, MF_STRING | ((g_busy ? g_want : g_state == SVC_ON) ? MF_CHECKED : 0)
                       | (can_toggle() ? 0 : MF_GRAYED),
                IDM_TOGGLE, L"Enable zapret");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (autostart_is_enabled() ? MF_CHECKED : 0),
                IDM_AUTOSTART, L"Start at login");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_OPEN, L"Open Zapret" ELLIPSIS);
    AppendMenuW(m, MF_STRING, IDM_QUIT, L"Quit");
    SetMenuDefaultItem(m, IDM_OPEN, FALSE);

    /* Required so the menu closes when the user clicks elsewhere. */
    SetForegroundWindow(g_hwnd);
    UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    TrackPopupMenuEx(m, align | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, x, y, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_msg_taskbar_created && msg != 0) {
        /* Explorer restarted: our tray icon is gone, add it again. */
        g_tray_added = FALSE;
        tray_load_icons();
        tray_update(TRUE);
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        g_title = CreateWindowExW(0, L"STATIC", L"Zapret", WS_CHILD | WS_VISIBLE | SS_CENTER,
                                  0, 0, 0, 0, hwnd, (HMENU)IDC_TITLE, g_inst, NULL);
        g_switch = CreateWindowExW(0, L"BUTTON", L"Enable zapret",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                   0, 0, 0, 0, hwnd, (HMENU)IDC_SWITCH, g_inst, NULL);
        g_status = CreateWindowExW(0, L"STATIC", ELLIPSIS, WS_CHILD | WS_VISIBLE | SS_CENTER,
                                   0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, g_inst, NULL);
        g_autostart = CreateWindowExW(0, L"BUTTON", L"Start at login",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                      0, 0, 0, 0, hwnd, (HMENU)IDC_AUTOSTART, g_inst, NULL);
        g_dpi = dpi_for_window(hwnd);
        make_fonts();
        layout();
        resize_to_dpi(NULL);
        return 0;

    case WM_DPICHANGED:
        g_dpi = HIWORD(wp);
        make_fonts();
        layout();
        resize_to_dpi((const RECT *)lp);
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        SetTextColor(dc, GetSysColor((HWND)lp == g_status ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }

    case WM_DRAWITEM:
        if (wp == IDC_SWITCH) {
            draw_switch((const DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SWITCH:
            if (HIWORD(wp) == BN_CLICKED && can_toggle())
                set_active(g_state != SVC_ON, FALSE);
            return 0;
        case IDC_AUTOSTART:
            if (HIWORD(wp) == BN_CLICKED) {
                autostart_set(Button_GetCheck(g_autostart) == BST_CHECKED);
                apply_autostart_ui();
            }
            return 0;
        case IDM_TOGGLE:
            if (can_toggle())
                set_active(g_state != SVC_ON, FALSE);
            return 0;
        case IDM_AUTOSTART:
            toggle_autostart();
            return 0;
        case IDM_OPEN:
            show_window();
            return 0;
        case IDM_QUIT:
            DestroyWindow(hwnd);
            return 0;
        case IDCANCEL:  /* Esc */
            hide_window();
            return 0;
        }
        break;

    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            show_window();
            break;
        case WM_CONTEXTMENU:
            show_menu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
            break;
        case WM_MBUTTONUP:
            if (can_toggle())
                set_active(g_state != SVC_ON, FALSE);
            break;
        }
        return 0;

    case WM_APP_DONE:
        on_done((BOOL)wp, (DWORD)lp);
        return 0;

    case WM_APP_SHOW:
        show_window();
        return 0;

    case WM_TIMER:
        if (wp == TIMER_POLL && !g_busy)
            refresh(FALSE);
        return 0;

    case WM_CLOSE:
        hide_window();
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;  /* logoff, or the installer replacing our files: let it */

    case WM_ENDSESSION:
        if (wp)
            DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_POLL);
        tray_remove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev;
    (void)show;
    (void)cmdline;
    g_inst = inst;

    BOOL hidden = FALSE;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc == 3 && wcscmp(argv[1], L"--grant-service-access") == 0)
            return grant_service_access(argv[2]);
        if (argc == 2 && wcscmp(argv[1], L"--close-running") == 0)
            return close_running();
        for (int i = 1; i < argc; i++)
            if (wcscmp(argv[i], L"--hidden") == 0)
                hidden = TRUE;
        LocalFree(argv);
    }

    /* One instance per user session; a second launch just shows the window. */
    HANDLE mutex = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(WINDOW_CLASS, NULL);
        if (other && !hidden) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, WM_APP_SHOW, 0, 0);
        }
        return 0;
    }

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g_msg_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = WINDOW_CLASS;
    LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &wc.hIcon);
    LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &wc.hIconSm);
    if (!RegisterClassExW(&wc))
        return 1;

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    if (!CreateWindowExW(WS_EX_CONTROLPARENT, WINDOW_CLASS, L"Zapret", style,
                         CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, NULL, NULL, inst, NULL))
        return 1;

    /* Lets the installer's Restart Manager close and reopen us during upgrades. */
    RegisterApplicationRestart(L"--hidden", RESTART_NO_CRASH | RESTART_NO_HANG);

    g_state = service_query();
    tray_load_icons();
    tray_update(TRUE);
    update_ui();
    apply_autostart_ui();
    restore_saved_state();
    SetTimer(g_hwnd, TIMER_POLL, POLL_MS, NULL);

    if (!hidden || !g_tray_added)
        show_window();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (mutex)
        CloseHandle(mutex);
    return (int)m.wParam;
}
