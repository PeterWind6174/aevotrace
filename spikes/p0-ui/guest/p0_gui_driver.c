/*
 * P0-UI-01 Windows reference driver.
 *
 * Modes:
 *   /check  load the DLL, resolve exports, call P0Reset/P0GetState.
 *           Does not create a window; suitable for a non-interactive SSH
 *           session and for ABI/export sanity.
 *   /auto   create the real window, drive button/edit/worker messages, print
 *           the guest business state. Requires a window station that permits
 *           window creation; if that fails, GUI evidence is marked pending.
 *   (none)  call P0RunGui(): interactive reference run for a user desktop.
 */
#include <windows.h>
#include <stdio.h>
#include <fcntl.h>
#include <io.h>
#include "p0_gui_test.h"

typedef void        (WINAPI *P0ResetFn)(void);
typedef void        (WINAPI *P0GetStateFn)(P0State *out);
typedef HWND        (WINAPI *P0CreateMainWindowFn)(void);
typedef int         (WINAPI *P0RunGuiFn)(void);
typedef LRESULT     (WINAPI *P0WndProcFn)(HWND, UINT, WPARAM, LPARAM);
typedef const wchar_t *(WINAPI *P0VersionWFn)(void);
typedef int         (WINAPI *P0ProbeUnsupportedFn)(void);

typedef struct P0Api {
    HMODULE dll;
    P0ResetFn reset;
    P0GetStateFn get_state;
    P0CreateMainWindowFn create_main;
    P0RunGuiFn run_gui;
    P0WndProcFn wnd_proc;
    P0VersionWFn version;
    P0ProbeUnsupportedFn probe_unsupported;
} P0Api;

static int build_dll_path(wchar_t *out, DWORD cap)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    DWORD i;
    if (n == 0 || n >= MAX_PATH) {
        return 0;
    }
    for (i = n; i > 0; i--) {
        if (exe[i - 1] == L'\\' || exe[i - 1] == L'/') {
            break;
        }
    }
    exe[i] = 0;
    if (i + 15 >= cap) {
        return 0;
    }
    lstrcpyW(out, exe);
    lstrcatW(out, L"p0_gui_test.dll");
    return 1;
}

static int load_api(P0Api *api)
{
    wchar_t path[MAX_PATH];
    if (!build_dll_path(path, MAX_PATH)) {
        wprintf(L"FAIL: cannot build DLL path\n");
        return 0;
    }
    api->dll = LoadLibraryW(path);
    if (api->dll == NULL) {
        wprintf(L"FAIL: LoadLibraryW(%ls) error=%lu\n", path, (unsigned long)GetLastError());
        return 0;
    }
    api->reset = (P0ResetFn)GetProcAddress(api->dll, "P0Reset");
    api->get_state = (P0GetStateFn)GetProcAddress(api->dll, "P0GetState");
    api->create_main = (P0CreateMainWindowFn)GetProcAddress(api->dll, "P0CreateMainWindow");
    api->run_gui = (P0RunGuiFn)GetProcAddress(api->dll, "P0RunGui");
    api->wnd_proc = (P0WndProcFn)GetProcAddress(api->dll, "P0WndProc");
    api->version = (P0VersionWFn)GetProcAddress(api->dll, "P0VersionW");
    api->probe_unsupported = (P0ProbeUnsupportedFn)GetProcAddress(api->dll, "P0ProbeUnsupported");
    if (api->reset == NULL || api->get_state == NULL || api->create_main == NULL ||
        api->run_gui == NULL || api->wnd_proc == NULL || api->version == NULL ||
        api->probe_unsupported == NULL) {
        wprintf(L"FAIL: missing export (reset=%p get=%p create=%p run=%p wnd=%p ver=%p)\n",
                api->reset, api->get_state, api->create_main, api->run_gui,
                api->wnd_proc, api->version);
        return 0;
    }
    return 1;
}

static void dump_state_utf8(const P0State *s)
{
    char text[4096];
    char last[P0_STATE_TEXT_CAP * 4];
    char status[P0_STATE_TEXT_CAP * 4];
    int n = 0;
    HANDLE h;
    DWORD written = 0;

    last[0] = 0;
    status[0] = 0;
    WideCharToMultiByte(CP_UTF8, 0, s->lastText, -1, last, sizeof(last), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, s->statusText, -1, status, sizeof(status), NULL, NULL);
    n += sprintf(text + n, "clicks=%llu\n", s->clicks);
    n += sprintf(text + n, "editChanges=%llu\n", s->editChanges);
    n += sprintf(text + n, "textLen=%llu\n", s->textLen);
    n += sprintf(text + n, "textHash=%016llX\n", s->textHash);
    n += sprintf(text + n, "modalCount=%llu\n", s->modalCount);
    n += sprintf(text + n, "modalResult=%llu\n", s->modalResult);
    n += sprintf(text + n, "workerCreated=%llu\n", s->workerCreated);
    n += sprintf(text + n, "workerPostsSent=%llu\n", s->workerPostsSent);
    n += sprintf(text + n, "workerPostsReceived=%llu\n", s->workerPostsReceived);
    n += sprintf(text + n, "workerExits=%llu\n", s->workerExits);
    n += sprintf(text + n, "syncEcho=%llu\n", s->syncEcho);
    n += sprintf(text + n, "wndProcCalls=%llu\n", s->wndProcCalls);
    n += sprintf(text + n, "resets=%llu\n", s->resets);
    n += sprintf(text + n, "lastText=%s\n", last);
    n += sprintf(text + n, "status=%s\n", status);

    h = CreateFileW(L"state_utf8.txt", GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, text, (DWORD)n, &written, NULL);
        CloseHandle(h);
    }
}

static void print_state(const P0State *s)
{
    wprintf(L"STATE clicks=%llu editChanges=%llu textLen=%llu textHash=%016llX\n",
            s->clicks, s->editChanges, s->textLen, s->textHash);
    wprintf(L"STATE modal=%llu modalResult=%llu workerCreated=%llu postsSent=%llu postsRecv=%llu exits=%llu\n",
            s->modalCount, s->modalResult, s->workerCreated,
            s->workerPostsSent, s->workerPostsReceived, s->workerExits);
    wprintf(L"STATE syncEcho=%llu wndProcCalls=%llu lastCommand=%llu lastMessage=0x%llX resets=%llu\n",
            s->syncEcho, s->wndProcCalls, s->lastCommand, s->lastMessage, s->resets);
    wprintf(L"STATE lastText=%ls\n", s->lastText);
    wprintf(L"STATE status=%ls\n", s->statusText);
}

static int pump_for(DWORD milliseconds)
{
    DWORD start = GetTickCount();
    MSG msg;
    while ((GetTickCount() - start) < milliseconds) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                return 0;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
    return 1;
}

static DWORD WINAPI auto_close_modal(LPVOID param)
{
    HWND dialog;
    (void)param;
    Sleep(700);
    dialog = FindWindowW(L"#32770", L"Aevotrace 中文模态");
    if (dialog != NULL) {
        PostMessageW(dialog, WM_COMMAND, IDOK, 0);
    }
    return 0;
}

static int run_check(P0Api *api)
{
    P0State st;
    const wchar_t *version;
    printf("CHECK mode: no window creation\n");
    memset(&st, 0xCC, sizeof(st));
    api->reset();
    api->get_state(&st);
    version = api->version();
    wprintf(L"CHECK version=%ls\n", version);
    wprintf(L"CHECK resets=%llu clicks=%llu wndProcCalls=%llu\n",
            st.resets, st.clicks, st.wndProcCalls);
    if (st.resets != 1 || st.clicks != 0) {
        printf("CHECK FAIL: unexpected state\n");
        return 3;
    }
    printf("CHECK OK: exports resolved and ABI state readable\n");
    return 0;
}

static int run_auto(P0Api *api)
{
    HWND hwnd;
    HWND edit;
    HWND button;
    P0State st;

    api->reset();
    hwnd = api->create_main();
    if (hwnd == NULL) {
        wprintf(L"AUTO FAIL: CreateWindowExW failed error=%lu (GUI pending manual desktop)\n",
                (unsigned long)GetLastError());
        return 4;
    }
    printf("AUTO window created\n");
    pump_for(150);

    edit = GetDlgItem(hwnd, IDC_EDIT_INPUT);
    button = GetDlgItem(hwnd, IDC_BTN_INCREMENT);
    if (edit == NULL || button == NULL) {
        wprintf(L"AUTO FAIL: controls missing edit=%p button=%p\n", edit, button);
        return 5;
    }

    /* Add an edit change and a button click through the Win32 message path. */
    SetWindowTextW(edit, L"Windows 参考文本");
    pump_for(50);
    SendMessageW(hwnd, WM_COMMAND,
                 MAKEWPARAM(IDC_BTN_INCREMENT, BN_CLICKED), (LPARAM)button);

    /* Worker thread: menu command -> CreateThread -> PostMessage -> ExitThread. */
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDM_RUN_WORKER, 0), 0);
    pump_for(500);

    /* Modal scene: MessageBoxW blocks until the helper thread returns IDOK. */
    {
        HANDLE closer = CreateThread(NULL, 0, auto_close_modal, NULL, 0, NULL);
        if (closer != NULL) {
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDM_ABOUT, 0), 0);
            WaitForSingleObject(closer, 3000);
            CloseHandle(closer);
        }
    }
    pump_for(100);

    /* Host -> guest synchronous message with a checked return value. */
    {
        LRESULT r = SendMessageW(hwnd, WM_P0_ECHO, 0x5A5A, 0);
        wprintf(L"AUTO echoReturn=0x%llX (expected 0x5A5A)\n", (unsigned long long)r);
        if (r != (LRESULT)0x5A5A) {
            printf("AUTO FAIL: echo return mismatch\n");
            return 6;
        }
    }

    api->get_state(&st);
    print_state(&st);
    dump_state_utf8(&st);

    if (st.clicks < 1 || st.editChanges < 1 || st.workerPostsReceived < 1 ||
        st.workerExits < 1 || st.syncEcho != 0x5A5A || st.modalCount != 1 ||
        st.modalResult != IDOK) {
        printf("AUTO FAIL: business state incomplete\n");
        return 7;
    }

    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    pump_for(100);
    printf("AUTO OK: Windows reference state reached\n");
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    P0Api api;
    int rc;
    memset(&api, 0, sizeof(api));

    if (argc > 1 && (_wcsicmp(argv[1], L"/check") == 0)) {
        if (!load_api(&api)) {
            return 2;
        }
        rc = run_check(&api);
        FreeLibrary(api.dll);
        return rc;
    }
    if (argc > 1 && (_wcsicmp(argv[1], L"/auto") == 0)) {
        if (!load_api(&api)) {
            return 2;
        }
        rc = run_auto(&api);
        FreeLibrary(api.dll);
        return rc;
    }

    if (!load_api(&api)) {
        return 2;
    }
    printf("Interactive GUI: close the window or use 文件/退出\n");
    rc = api.run_gui();
    FreeLibrary(api.dll);
    return rc;
}
