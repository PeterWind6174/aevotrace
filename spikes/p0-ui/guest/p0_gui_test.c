/*
 * P0-UI-01 Windows x64 GUI fixture (DLL).
 *
 * This is the input program that the macOS P0 harness executes as guest x64
 * machine code. It is deliberately written without the C runtime so that the
 * PE imports are a small, inspectable Win32 surface.
 *
 * Controls / menu / message behaviour:
 *   - main window class registered with P0WndProc (exported)
 *   - static label, edit box, push button, popup menu (Chinese text)
 *   - WM_COMMAND from the button mutates business state and synchronously
 *     calls GetWindowTextW + SetWindowTextW
 *   - WM_COMMAND from the edit box records text changes
 *   - About menu item calls MessageBoxW synchronously (modal)
 *   - Worker menu item calls CreateThread; worker Sleeps, PostMessageW,
 *     increments counters, then ExitThread
 *   - WM_P0_ECHO returns a fixed value so the host can test a synchronous
 *     Host -> Guest -> Host round trip and inspect eax.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "p0_gui_test.h"

/* ------------------------------------------------------------------ */
/* Tiny CRT-free helpers                                              */
/* ------------------------------------------------------------------ */

static int p0_wlen(const wchar_t *s)
{
    int n = 0;
    if (s == 0) {
        return 0;
    }
    while (s[n] != 0) {
        n++;
    }
    return n;
}

static void p0_wcopy(wchar_t *dst, int cap, const wchar_t *src)
{
    int i = 0;
    if (dst == 0 || cap <= 0) {
        return;
    }
    if (src != 0) {
        while (src[i] != 0 && i < cap - 1) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = 0;
}

static void p0_wcat(wchar_t *dst, int cap, const wchar_t *src)
{
    int n = p0_wlen(dst);
    int i = 0;
    if (n >= cap - 1) {
        return;
    }
    if (src != 0) {
        while (src[i] != 0 && n < cap - 1) {
            dst[n++] = src[i++];
        }
    }
    dst[n] = 0;
}

static void p0_append_u64(wchar_t *dst, int cap, unsigned long long value)
{
    wchar_t tmp[24];
    int n = 0;
    int pos = p0_wlen(dst);
    if (value == 0) {
        tmp[n++] = L'0';
    } else {
        while (value != 0 && n < (int)(sizeof(tmp) / sizeof(tmp[0]))) {
            tmp[n++] = (wchar_t)(L'0' + (int)(value % 10));
            value /= 10;
        }
    }
    while (n > 0 && pos < cap - 1) {
        dst[pos++] = tmp[--n];
    }
    dst[pos] = 0;
}

static unsigned long long p0_hash_text(const wchar_t *s)
{
    unsigned long long h = 1469598103934665603ULL; /* FNV-1a 64 offset */
    int i;
    for (i = 0; s[i] != 0; i++) {
        h ^= (unsigned long long)(unsigned short)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Guest business state                                               */
/* ------------------------------------------------------------------ */

static P0State g_state;
static HINSTANCE g_hinst;
static HWND g_main;
static HWND g_button;
static HWND g_edit;
static HWND g_label;
static HWND g_status;
static HANDLE g_worker;

static void p0_build_status(void)
{
    wchar_t *b = g_state.statusText;
    b[0] = 0;
    p0_wcat(b, P0_STATE_TEXT_CAP, L"点击=");
    p0_append_u64(b, P0_STATE_TEXT_CAP, g_state.clicks);
    p0_wcat(b, P0_STATE_TEXT_CAP, L"  文本长度=");
    p0_append_u64(b, P0_STATE_TEXT_CAP, g_state.textLen);
    p0_wcat(b, P0_STATE_TEXT_CAP, L"  模态=");
    p0_append_u64(b, P0_STATE_TEXT_CAP, g_state.modalCount);
    p0_wcat(b, P0_STATE_TEXT_CAP, L"  线程发/收/退=");
    p0_append_u64(b, P0_STATE_TEXT_CAP, g_state.workerPostsSent);
    p0_wcat(b, P0_STATE_TEXT_CAP, L"/");
    p0_append_u64(b, P0_STATE_TEXT_CAP, g_state.workerPostsReceived);
    p0_wcat(b, P0_STATE_TEXT_CAP, L"/");
    p0_append_u64(b, P0_STATE_TEXT_CAP, g_state.workerExits);
}

static void p0_refresh_status(void)
{
    p0_build_status();
    if (g_status != 0) {
        SetWindowTextW(g_status, g_state.statusText);
    }
}

/* Read the edit control through the normal Win32 API. */
static int p0_read_edit(wchar_t *buf, int cap)
{
    int n = 0;
    if (buf != 0 && cap > 0) {
        buf[0] = 0;
        if (g_edit != 0) {
            n = GetWindowTextW(g_edit, buf, cap);
            if (n < 0) {
                n = 0;
            }
            if (n >= cap) {
                n = cap - 1;
            }
        }
    }
    g_state.textLen = (unsigned long long)n;
    g_state.textHash = p0_hash_text(buf);
    p0_wcopy(g_state.lastText, P0_STATE_TEXT_CAP, buf);
    return n;
}

/* Read the edit control through synchronous SendMessageW(WM_GETTEXT). */
static int p0_read_edit_send(wchar_t *buf, int cap)
{
    int n = 0;
    if (buf != 0 && cap > 0) {
        buf[0] = 0;
        if (g_edit != 0) {
            n = (int)SendMessageW(g_edit, WM_GETTEXT, (WPARAM)cap, (LPARAM)buf);
            if (n < 0) {
                n = 0;
            }
            if (n >= cap) {
                n = cap - 1;
            }
        }
    }
    g_state.textLen = (unsigned long long)n;
    g_state.textHash = p0_hash_text(buf);
    p0_wcopy(g_state.lastText, P0_STATE_TEXT_CAP, buf);
    return n;
}

/* ------------------------------------------------------------------ */
/* Worker thread                                                      */
/* ------------------------------------------------------------------ */

static DWORD WINAPI p0_worker_proc(LPVOID param)
{
    HWND hwnd = (HWND)param;

    /* Keep the thread observably alive; this is a guest code path. */
    Sleep(40);
    g_state.workerPostsSent++;
    PostMessageW(hwnd, WM_P0_WORKER_DONE, (WPARAM)GetCurrentThreadId(), 0);
    g_state.workerExits++;
    ExitThread(0);
    return 0; /* not reached */
}

/* ------------------------------------------------------------------ */
/* Window procedure (exported; used directly as the class WndProc)     */
/* ------------------------------------------------------------------ */

P0_API LRESULT CALLBACK P0WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    g_state.wndProcCalls++;
    g_state.lastMessage = (unsigned long long)msg;

    switch (msg) {
    case WM_CREATE:
        if (g_main == 0) {
            HMENU file_menu;
            HMENU menu_bar;
            HINSTANCE inst = (g_hinst != 0) ? g_hinst : GetModuleHandleW(0);

            g_main = hwnd;

            g_status = CreateWindowExW(
                0, L"STATIC", L"初始化中…",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                16, 14, 468, 22, hwnd, (HMENU)(UINT_PTR)IDC_STATIC_STATUS, inst, 0);

            g_label = CreateWindowExW(
                0, L"STATIC", L"业务状态：等待操作",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                16, 44, 468, 22, hwnd, (HMENU)(UINT_PTR)IDC_STATIC_LABEL, inst, 0);

            g_edit = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"初始文本",
                WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
                16, 78, 300, 28, hwnd, (HMENU)(UINT_PTR)IDC_EDIT_INPUT, inst, 0);

            g_button = CreateWindowExW(
                0, L"BUTTON", L"增加计数",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                332, 78, 152, 28, hwnd, (HMENU)(UINT_PTR)IDC_BTN_INCREMENT, inst, 0);

            file_menu = CreatePopupMenu();
            AppendMenuW(file_menu, MF_STRING, IDM_ABOUT, L"关于(&A)…");
            AppendMenuW(file_menu, MF_STRING, IDM_RUN_WORKER, L"启动工作线程(&W)");
            AppendMenuW(file_menu, MF_SEPARATOR, 0, 0);
            AppendMenuW(file_menu, MF_STRING, IDM_EXIT, L"退出(&X)");
            menu_bar = CreateMenu();
            AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)file_menu, L"文件(&F)");
            SetMenu(hwnd, menu_bar);

            g_state.lastCommand = 0;
            p0_read_edit(g_state.lastText, P0_STATE_TEXT_CAP);
            p0_refresh_status();
        }
        return 0;

    case WM_COMMAND:
        g_state.lastCommand = (unsigned long long)LOWORD(wParam);

        if (LOWORD(wParam) == IDC_BTN_INCREMENT && HIWORD(wParam) == BN_CLICKED) {
            wchar_t text[P0_STATE_TEXT_CAP];
            g_state.clicks++;
            p0_read_edit_send(text, P0_STATE_TEXT_CAP);
            p0_refresh_status();

            /* Synchronous control calls: these must reach the host and return. */
            SetWindowTextW(g_label, L"按钮回调已同步更新业务状态");
            SetWindowTextW(g_status, g_state.statusText);
            return 0;
        }

        if (LOWORD(wParam) == IDC_EDIT_INPUT && HIWORD(wParam) == EN_CHANGE) {
            wchar_t text[P0_STATE_TEXT_CAP];
            g_state.editChanges++;
            p0_read_edit(text, P0_STATE_TEXT_CAP);
            p0_refresh_status();
            return 0;
        }

        if (LOWORD(wParam) == IDM_ABOUT) {
            INT_PTR r = MessageBoxW(
                hwnd,
                L"这是同步模态测试。\n返回后业务模态计数加一。",
                L"Aevotrace 中文模态",
                MB_OKCANCEL | MB_ICONINFORMATION);
            g_state.modalResult = (unsigned long long)r;
            if (r == IDOK) {
                g_state.modalCount++;
            }
            p0_refresh_status();
            return 0;
        }

        if (LOWORD(wParam) == IDM_RUN_WORKER) {
            if (g_worker == 0) {
                DWORD tid = 0;
                g_worker = CreateThread(0, 0, p0_worker_proc, hwnd, 0, &tid);
                if (g_worker != 0) {
                    g_state.workerCreated++;
                    p0_refresh_status();
                }
            }
            return 0;
        }

        if (LOWORD(wParam) == IDM_EXIT) {
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;

    case WM_P0_WORKER_DONE:
        g_state.workerPostsReceived++;
        SetWindowTextW(g_label, L"工作线程 PostMessage 已送达");
        p0_refresh_status();
        return 0;

    case WM_P0_ECHO:
        g_state.syncEcho = (unsigned long long)wParam;
        p0_refresh_status();
        /* Host asserts this synchronous return value. */
        return (LRESULT)0x5A5A;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_main = 0;
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

/* ------------------------------------------------------------------ */
/* Exported entry points                                              */
/* ------------------------------------------------------------------ */

P0_API void P0Reset(void)
{
    unsigned char *p = (unsigned char *)&g_state;
    unsigned long long keep = g_state.resets + 1;
    unsigned long long i;
    for (i = 0; i < sizeof(g_state); i++) {
        p[i] = 0;
    }
    g_state.resets = keep;

    if (g_label != 0) {
        SetWindowTextW(g_label, L"业务状态：已重置");
    }
    if (g_status != 0) {
        SetWindowTextW(g_status, L"本程序为 P0-UI-01 x64 Guest");
    }
}

P0_API void P0GetState(P0State *out)
{
    unsigned char *dst = (unsigned char *)out;
    const unsigned char *src = (const unsigned char *)&g_state;
    unsigned long long i;
    if (out == 0) {
        return;
    }
    for (i = 0; i < sizeof(P0State); i++) {
        dst[i] = src[i];
    }
}

P0_API const wchar_t *P0VersionW(void)
{
    return P0_VERSION_TEXT;
}

/*
 * Negative probe for the native backend: MessageBeep is intentionally not in
 * the declared P0 import subset. The macOS host must report the missing API
 * instead of faking success or routing the call through the compat backend.
 */
P0_API int P0ProbeUnsupported(void)
{
    return (int)MessageBeep(0xFFFFFFFFu);
}

P0_API HWND P0CreateMainWindow(void)
{
    WNDCLASSEXW wc;
    HINSTANCE inst = (g_hinst != 0) ? g_hinst : GetModuleHandleW(0);
    HWND hwnd;

    if (g_main != 0) {
        return g_main;
    }

    {
        unsigned char *p = (unsigned char *)&wc;
        unsigned int i;
        for (i = 0; i < sizeof(wc); i++) {
            p[i] = 0;
        }
    }
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = P0WndProc;
    wc.hInstance = inst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = P0_WINDOW_CLASS;

    if (RegisterClassExW(&wc) == 0) {
        /* 1410 = ERROR_CLASS_ALREADY_EXISTS; acceptable on repeated calls. */
        if (GetLastError() != 1410) {
            return 0;
        }
    }

    hwnd = CreateWindowExW(
        0, P0_WINDOW_CLASS, P0_WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 190,
        0, 0, inst, 0);
    if (hwnd == 0) {
        return 0;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return hwnd;
}

P0_API int P0RunGui(void)
{
    MSG msg;
    HWND hwnd = P0CreateMainWindow();
    if (hwnd == 0) {
        return 2;
    }
    for (;;) {
        LONG r = GetMessageW(&msg, 0, 0, 0);
        if (r <= 0) {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

/* Custom entry point: /NODEFAULTLIB build. */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = inst;
    }
    return TRUE;
}
