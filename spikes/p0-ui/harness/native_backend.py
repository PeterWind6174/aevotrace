"""Real AppKit backend for P0-UI-01.

This backend uses NSWindow/NSButton/NSTextField/NSMenu and NSAlert. There is
no fallback to the compat backend: if a requested GUI operation cannot be
served here the backend raises and the run fails with evidence.
"""
from __future__ import annotations

import objc
from AppKit import (
    NSAlert,
    NSAlertFirstButtonReturn,
    NSAlertSecondButtonReturn,
    NSApp,
    NSApplicationActivationPolicyRegular,
    NSBackingStoreBuffered,
    NSBezelStyleRounded,
    NSButton,
    NSButtonTypeMomentaryPushIn,
    NSColor,
    NSControlTextDidChangeNotification,
    NSFont,
    NSMakeRect,
    NSMenu,
    NSMenuItem,
    NSModalPanelRunLoopMode,
    NSTextField,
    NSTimer,
    NSView,
    NSWindow,
    NSWindowStyleMaskClosable,
    NSWindowStyleMaskMiniaturizable,
    NSWindowStyleMaskResizable,
    NSWindowStyleMaskTitled,
    NSRunLoop,
    NSRunLoopCommonModes,
    NSDefaultRunLoopMode,
    NSThread,
)
from Foundation import NSDate, NSNotification, NSObject
import threading
import time

from . import guest_abi as abi
from .errors import UnsupportedApi


class FlippedView(NSView):
    def isFlipped(self):
        return True


class NativeActionTarget(NSObject):
    def initWithBackend_(self, backend):
        self = objc.super(NativeActionTarget, self).init()
        if self is None:
            return None
        self._backend = backend
        return self

    def buttonClicked_(self, sender):
        self._backend.on_button_clicked(sender)

    def menuAction_(self, sender):
        self._backend.on_menu_action(sender)


class NativeEditDelegate(NSObject):
    def initWithBackend_(self, backend):
        self = objc.super(NativeEditDelegate, self).init()
        if self is None:
            return None
        self._backend = backend
        return self

    def controlTextDidChange_(self, note):
        self._backend.on_edit_changed(note.object())


class NativeTimerHelper(NSObject):
    def initWithCallback_(self, callback):
        self = objc.super(NativeTimerHelper, self).init()
        if self is None:
            return None
        self._callback = callback
        return self

    def fire_(self, timer):
        self._callback()


class NativeWindowDelegate(NSObject):
    def initWithBackend_(self, backend):
        self = objc.super(NativeWindowDelegate, self).init()
        if self is None:
            return None
        self._backend = backend
        return self

    def windowShouldClose_(self, sender):
        # Ask the guest first: WM_CLOSE is a guest decision. DestroyWindow
        # from the guest then calls back into close_window.
        self._backend.logger.event("native.window_should_close",
                                   handle=f"0x{self._backend.main_handle:x}")
        self._backend.host.send_sync(self._backend.main_handle, abi.WM_CLOSE, 0, 0,
                                     reason="native window close button")
        return False


class NativeBackend:
    name = "native"

    def __init__(self, host, logger, evidence_dir: str):
        self.host = host
        self.logger = logger
        self.evidence_dir = evidence_dir
        self.windows: dict[int, NSWindow] = {}
        self.controls: dict[int, object] = {}
        self.window_content: dict[int, NSView] = {}
        self.action_target = NativeActionTarget.alloc().initWithBackend_(self)
        self.edit_delegate = NativeEditDelegate.alloc().initWithBackend_(self)
        self.button_handles: dict[int, int] = {}
        self.menu_items: dict[int, tuple[object, str]] = {}
        self.main_menu = None
        self.main_handle = 0
        self.window_delegates = {}
        self._automating = False
        self.backend_used = True

    # -- lifecycle -----------------------------------------------------
    def create_main_window(self, handle, title, x, y, width, height):
        style = (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                 NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
        rect = NSMakeRect(260, 300, max(width, 420), max(height, 190))
        window = NSWindow.alloc().initWithContentRect_styleMask_backing_defer_(
            rect, style, NSBackingStoreBuffered, False)
        window.setTitle_(title)
        window.setReleasedWhenClosed_(False)
        content = FlippedView.alloc().initWithFrame_(
            NSMakeRect(0, 0, rect.size.width, rect.size.height))
        window.setContentView_(content)
        delegate = NativeWindowDelegate.alloc().initWithBackend_(self)
        window.setDelegate_(delegate)
        self.window_delegates[handle] = delegate
        self.windows[handle] = window
        self.window_content[handle] = content
        self.main_handle = handle
        self.logger.event("native.window_created", handle=f"0x{handle:x}",
                          objc_class=str(window.className()), title=title,
                          thread=threading.current_thread().name,
                          main_thread=bool(NSThread.isMainThread()))

    def create_control(self, handle, kind, text, style, x, y, width, height, ctrl_id):
        content = self.window_content[self.host.main_hwnd]
        rect = NSMakeRect(x, y, width, height)
        if kind == "static":
            view = NSTextField.labelWithString_(text)
            view.setFrame_(rect)
            view.setFont_(NSFont.systemFontOfSize_(13))
            view.setSelectable_(False)
        elif kind == "edit":
            view = NSTextField.alloc().initWithFrame_(rect)
            view.setStringValue_(text)
            view.setBezeled_(True)
            view.setBezelStyle_(1)
            view.setEditable_(True)
            view.setSelectable_(True)
            view.setFont_(NSFont.systemFontOfSize_(13))
            view.setDelegate_(self.edit_delegate)
            self.edit_handle = handle
        elif kind == "button":
            view = NSButton.alloc().initWithFrame_(rect)
            view.setTitle_(text)
            view.setBezelStyle_(NSBezelStyleRounded)
            view.setButtonType_(NSButtonTypeMomentaryPushIn)
            view.setTarget_(self.action_target)
            view.setAction_("buttonClicked:")
            self.button_handles[objc.pyobjc_id(view)] = handle
        else:
            raise UnsupportedApi(f"native backend: unsupported control kind {kind!r}")
        content.addSubview_(view)
        self.controls[handle] = view
        self.logger.event("native.control_created", handle=f"0x{handle:x}",
                          control_kind=kind, objc_class=str(view.className()),
                          control_id=ctrl_id, text=text,
                          thread=threading.current_thread().name,
                          main_thread=bool(NSThread.isMainThread()))

    def show_window(self, handle):
        window = self.windows[handle]
        window.makeKeyAndOrderFront_(None)
        window.makeFirstResponder_(window.contentView())
        NSApp.activateIgnoringOtherApps_(True)
        self.logger.event("native.show_window", handle=f"0x{handle:x}",
                          window_number=window.windowNumber(),
                          thread=threading.current_thread().name,
                          main_thread=bool(NSThread.isMainThread()))

    def close_window(self, handle):
        window = self.windows.get(handle)
        if window is not None:
            window.orderOut_(None)
            self.logger.event("native.close_window", handle=f"0x{handle:x}")

    def window_number(self, handle):
        window = self.windows.get(handle)
        return window.windowNumber() if window is not None else None

    def pump(self, seconds: float):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            NSApp.nextEventMatchingMask_untilDate_inMode_dequeue_(
                (1 << 24) - 1,  # NSEventMaskAny
                NSDate.dateWithTimeIntervalSinceNow_(0.02),
                NSDefaultRunLoopMode,
                True,
            )
            NSApp.updateWindows()
            self.host.drain_posted_messages()

    # -- text ----------------------------------------------------------
    def set_text(self, handle, text):
        info = self.host.windows[handle]
        if info.kind == "main":
            self.windows[handle].setTitle_(text)
        elif info.kind == "button":
            self.controls[handle].setTitle_(text)
        elif info.kind in ("static", "edit"):
            self.controls[handle].setStringValue_(text)
        else:
            raise UnsupportedApi(f"native set_text: unknown kind {info.kind}")

    def get_text(self, handle):
        info = self.host.windows[handle]
        if info.kind == "main":
            return str(self.windows[handle].title())
        if info.kind == "button":
            return str(self.controls[handle].title())
        if info.kind in ("static", "edit"):
            return str(self.controls[handle].stringValue())
        raise UnsupportedApi(f"native get_text: unknown kind {info.kind}")

    # -- menu ----------------------------------------------------------
    def set_menu(self, handle, items):
        if handle != self.host.main_hwnd:
            raise UnsupportedApi("native menu is only supported on the main window")
        main_menu = NSMenu.alloc().initWithTitle_("AevotraceP0")
        for top in items:
            if top.get("kind") != "popup":
                continue
            top_item = NSMenuItem.alloc().initWithTitle_action_keyEquivalent_(
                top["label"], None, "")
            submenu = NSMenu.alloc().initWithTitle_(top["label"])
            for child in top.get("items", []):
                if child.get("kind") == "separator":
                    submenu.addItem_(NSMenuItem.separatorItem())
                elif child.get("kind") == "item":
                    item = NSMenuItem.alloc().initWithTitle_action_keyEquivalent_(
                        child["label"], "menuAction:", "")
                    item.setTarget_(self.action_target)
                    item.setTag_(child["id"])
                    submenu.addItem_(item)
                    self.menu_items[child["id"]] = (item, child["label"])
            top_item.setSubmenu_(submenu)
            main_menu.addItem_(top_item)
        NSApp.setMainMenu_(main_menu)
        self.main_menu = main_menu
        self.logger.event("native.menu_set", items=list(self.menu_items.keys()))

    def click_menu(self, item_id: int):
        entry = self.menu_items.get(item_id)
        if entry is None:
            raise UnsupportedApi(f"native menu item {item_id} not present")
        item, label = entry
        self._automating = True
        try:
            action = item.action()
            target = item.target()
            self.logger.event("automation.menu_click", item_id=item_id,
                              label=label, path="NSApp.sendAction:to:from:")
            NSApp.sendAction_to_from_(action, target, item)
        finally:
            self._automating = False

    def on_menu_action(self, sender):
        item_id = int(sender.tag())
        self.logger.event("native.menu_action", item_id=item_id,
                          automation=self._automating,
                          thread=threading.current_thread().name,
                          main_thread=bool(NSThread.isMainThread()))
        self.host.dispatch_menu(item_id, source="native NSMenu action")

    # -- button / edit automation + events -----------------------------
    def click_button(self, handle: int):
        view = self.controls[handle]
        self._automating = True
        try:
            self.logger.event("automation.button_click", handle=f"0x{handle:x}",
                              path="NSButton.performClick:")
            view.performClick_(None)
        finally:
            self._automating = False

    def on_button_clicked(self, sender):
        handle = self.button_handles.get(objc.pyobjc_id(sender))
        if handle is None:
            raise UnsupportedApi("native button action from unknown NSButton")
        info = self.host.windows[handle]
        self.logger.event("native.button_action", handle=f"0x{handle:x}",
                          control_id=info.control_id, automation=self._automating,
                          thread=threading.current_thread().name,
                          main_thread=bool(NSThread.isMainThread()))
        self.host.dispatch_command(info.control_id, abi.BN_CLICKED,
                                   source="native NSButton action")

    def set_edit_text(self, handle: int, text: str):
        view = self.controls[handle]
        view.setStringValue_(text)
        self.logger.event("automation.edit_text", handle=f"0x{handle:x}",
                          text=text, path="NSTextField.setStringValue + delegate")
        # Route through the same delegate AppKit uses for typed edits.
        note = NSNotification.notificationWithName_object_(
            NSControlTextDidChangeNotification, view)
        self.edit_delegate.controlTextDidChange_(note)

    def on_edit_changed(self, sender):
        handle = getattr(self, "edit_handle", None)
        if handle is None:
            raise UnsupportedApi("native edit change with no registered edit control")
        info = self.host.windows[handle]
        self.logger.event("native.edit_changed", handle=f"0x{handle:x}",
                          control_id=info.control_id,
                          thread=threading.current_thread().name,
                          main_thread=bool(NSThread.isMainThread()))
        self.host.dispatch_command(info.control_id, abi.EN_CHANGE,
                                   source="native NSTextField delegate")

    # -- modal ---------------------------------------------------------
    def show_message_box(self, parent_handle, text, caption, style, capture):
        alert = NSAlert.alloc().init()
        alert.setMessageText_(caption)
        alert.setInformativeText_(text)
        alert.addButtonWithTitle_("确定")
        if style & abi.MB_OKCANCEL:
            alert.addButtonWithTitle_("取消")
        parent = self.windows.get(parent_handle)
        if parent is not None:
            alert.window().setLevel_(parent.level() + 1)

        def dismiss():
            number = alert.window().windowNumber()
            self.logger.event("native.modal_shown", window_number=number,
                              caption=caption)
            if capture is not None:
                capture(f"03-modal-{self.name}.png", window_number=number)
            NSApp.stopModalWithCode_(NSAlertFirstButtonReturn)

        helper = NativeTimerHelper.alloc().initWithCallback_(dismiss)
        timer = NSTimer.timerWithTimeInterval_target_selector_userInfo_repeats_(
            0.7, helper, "fire:", None, False)
        NSRunLoop.currentRunLoop().addTimer_forMode_(timer, NSRunLoopCommonModes)
        NSRunLoop.currentRunLoop().addTimer_forMode_(timer, NSModalPanelRunLoopMode)
        self.logger.event("native.modal_run", caption=caption, style=f"0x{style:x}",
                          thread=threading.current_thread().name,
                          main_thread=bool(NSThread.isMainThread()))
        response = alert.runModal()
        if response == NSAlertFirstButtonReturn:
            return abi.IDOK
        if response == NSAlertSecondButtonReturn:
            return abi.IDCANCEL
        raise UnsupportedApi(f"native NSAlert returned unexpected response {response}")

    # -- proof ---------------------------------------------------------
    def proof(self) -> dict:
        windows = []
        controls = []
        for handle, window in self.windows.items():
            windows.append({"handle": f"0x{handle:x}",
                            "objc_class": str(window.className())})
        for handle, view in self.controls.items():
            cls = str(view.className())
            controls.append({"handle": f"0x{handle:x}",
                             "kind": self.host.windows[handle].kind,
                             "objc_class": cls})
            if cls.startswith("Compat"):
                raise UnsupportedApi(f"native backend instantiated compat class {cls}")
        return {
            "backend": "native",
            "api": "AppKit",
            "windows": windows,
            "controls": controls,
            "main_menu_is_nsmenu": isinstance(self.main_menu, NSMenu),
            "compat_classes_used": False,
            "main_thread": bool(NSThread.isMainThread()),
            "creation_thread": threading.current_thread().name,
            "note": "NSWindow/NSButton/NSTextField/NSMenu/NSAlert objects above",
        }
