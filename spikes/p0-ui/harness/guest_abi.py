"""ABI constants for the P0-UI-01 Windows guest fixture.

The struct layout mirrors guest/p0_gui_test.h. It is intentionally explicit:
this spike is testing the ABI boundary, not using ctypes auto-magic.
"""
from __future__ import annotations

import struct

P0_WINDOW_CLASS = "AevotraceP0Window"
P0_WINDOW_TITLE = "Aevotrace P0-UI-01 中文界面"

IDC_BTN_INCREMENT = 1001
IDC_EDIT_INPUT = 1002
IDC_STATIC_LABEL = 1003
IDC_STATIC_STATUS = 1004

IDM_ABOUT = 2001
IDM_RUN_WORKER = 2002
IDM_EXIT = 2003

WM_CREATE = 0x0001
WM_DESTROY = 0x0002
WM_CLOSE = 0x0010
WM_SETTEXT = 0x000C
WM_GETTEXT = 0x000D
WM_GETTEXTLENGTH = 0x000E
WM_COMMAND = 0x0111
WM_QUIT = 0x0012
WM_APP = 0x8000
WM_P0_WORKER_DONE = WM_APP + 1
WM_P0_ECHO = WM_APP + 2

WM_USER = 0x0400
EM_SETSEL = WM_USER + 0
EM_REPLACESEL = WM_USER + 2

BN_CLICKED = 0
EN_CHANGE = 0x0300

IDOK = 1
IDCANCEL = 2
MB_OK = 0x00000000
MB_OKCANCEL = 0x00000001
MB_ICONINFORMATION = 0x00000040

# P0State: 15 unsigned long long + 2 * 128 UTF-16 code units.
P0_STATE_QWORDS = 15
P0_STATE_TEXT_CAP = 128
P0_STATE_SIZE = P0_STATE_QWORDS * 8 + 2 * P0_STATE_TEXT_CAP * 2
P0_STATE_FORMAT = "<15Q" + (f"{2 * P0_STATE_TEXT_CAP}s" * 2)

P0_STATE_FIELDS = (
    "clicks",
    "edit_changes",
    "text_len",
    "text_hash",
    "modal_count",
    "modal_result",
    "worker_created",
    "worker_posts_sent",
    "worker_posts_received",
    "worker_exits",
    "sync_echo",
    "wndproc_calls",
    "last_command",
    "last_message",
    "resets",
)

MIDI_ID_MASK = 0xFFFF
CONTROL_ID_MASK = 0xFFFF

MSG_NAMES = {
    0x0001: "WM_CREATE",
    0x0002: "WM_DESTROY",
    0x0003: "WM_MOVE",
    0x0005: "WM_SIZE",
    0x0006: "WM_ACTIVATE",
    0x0007: "WM_SETFOCUS",
    0x0008: "WM_KILLFOCUS",
    0x000C: "WM_SETTEXT",
    0x000D: "WM_GETTEXT",
    0x000E: "WM_GETTEXTLENGTH",
    0x0010: "WM_CLOSE",
    0x0012: "WM_QUIT",
    0x0111: "WM_COMMAND",
    0x0113: "WM_TIMER",
    0x0201: "WM_LBUTTONDOWN",
    0x0202: "WM_LBUTTONUP",
    0x8001: "WM_P0_WORKER_DONE",
    0x8002: "WM_P0_ECHO",
}

COMMAND_NAMES = {
    IDC_BTN_INCREMENT: "IDC_BTN_INCREMENT",
    IDC_EDIT_INPUT: "IDC_EDIT_INPUT",
    IDM_ABOUT: "IDM_ABOUT",
    IDM_RUN_WORKER: "IDM_RUN_WORKER",
    IDM_EXIT: "IDM_EXIT",
}


def msg_name(msg: int) -> str:
    return MSG_NAMES.get(msg, f"0x{msg:04X}")


def command_name(cmd: int) -> str:
    return COMMAND_NAMES.get(cmd, f"cmd={cmd}")


def decode_utf16(raw: bytes) -> str:
    """Decode a fixed-size UTF-16LE buffer up to the first NUL."""
    end = len(raw)
    for i in range(0, len(raw) - 1, 2):
        if raw[i] == 0 and raw[i + 1] == 0:
            end = i
            break
    return raw[:end].decode("utf-16-le", errors="replace")


def parse_state(raw: bytes) -> dict:
    if len(raw) < P0_STATE_SIZE:
        raise ValueError(f"state buffer too short: {len(raw)} < {P0_STATE_SIZE}")
    values = struct.unpack(P0_STATE_FORMAT, raw[:P0_STATE_SIZE])
    result = {name: values[i] for i, name in enumerate(P0_STATE_FIELDS)}
    result["last_text"] = decode_utf16(values[P0_STATE_QWORDS])
    result["status_text"] = decode_utf16(values[P0_STATE_QWORDS + 1])
    return result
