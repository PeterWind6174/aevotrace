/*
 * P0-UI-01 guest fixture: shared constants and ABI-visible business state.
 *
 * This header is used by:
 *   - p0_gui_test.c   (Windows x64 DLL, the converted guest input)
 *   - p0_gui_driver.c (Windows x64 driver, interactive + non-interactive check)
 *   - spikes/p0-ui/harness/guest_abi.py (mirrors the struct layout; verify me)
 */
#ifndef P0_GUI_TEST_H
#define P0_GUI_TEST_H

#include <windows.h>

#ifdef P0_GUI_EXPORTS
#define P0_API __declspec(dllexport)
#else
#define P0_API __declspec(dllimport)
#endif

#define P0_WINDOW_CLASS   L"AevotraceP0Window"
#define P0_WINDOW_TITLE   L"Aevotrace P0-UI-01 中文界面"

#define IDC_BTN_INCREMENT 1001
#define IDC_EDIT_INPUT    1002
#define IDC_STATIC_LABEL  1003
#define IDC_STATIC_STATUS 1004

#define IDM_ABOUT         2001
#define IDM_RUN_WORKER    2002
#define IDM_EXIT          2003

#define WM_P0_WORKER_DONE (WM_APP + 1)
#define WM_P0_ECHO        (WM_APP + 2)

#define P0_STATE_TEXT_CAP 128
#define P0_VERSION_TEXT   L"Aevotrace P0-UI-01 guest v1"

/* Keep this layout in sync with harness/guest_abi.py. */
typedef struct P0State {
    unsigned long long clicks;
    unsigned long long editChanges;
    unsigned long long textLen;
    unsigned long long textHash;
    unsigned long long modalCount;
    unsigned long long modalResult;
    unsigned long long workerCreated;
    unsigned long long workerPostsSent;
    unsigned long long workerPostsReceived;
    unsigned long long workerExits;
    unsigned long long syncEcho;
    unsigned long long wndProcCalls;
    unsigned long long lastCommand;
    unsigned long long lastMessage;
    unsigned long long resets;
    wchar_t lastText[P0_STATE_TEXT_CAP];
    wchar_t statusText[P0_STATE_TEXT_CAP];
} P0State;

#endif /* P0_GUI_TEST_H */
