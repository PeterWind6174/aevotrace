"""P0 compat backend: Windows-like chrome and controls drawn with Cocoa.

This is a P0 semantic harness, NOT the final compat runtime. It is an
independent backend (custom NSView subclasses and hit testing), native arm64,
and does not use Rosetta. Container/runtime integration (e.g. a bundled
compatible runtime) remains an open D3 decision and is not claimed here.
"""
from __future__ import annotations

import threading
import time

import objc
from AppKit import (
    NSApp,
    NSApplicationActivationPolicyRegular,
    NSBackingStoreBuffered,
    NSBezierPath,
    NSColor,
    NSFont,
    NSFontAttributeName,
    NSInsetRect,
    NSForegroundColorAttributeName,
    NSMakePoint,
    NSMakeRect,
    NSMenu,
    NSModalPanelRunLoopMode,
    NSTimer,
    NSView,
    NSWindow,
    NSWindowStyleMaskBorderless,
    NSRunLoop,
    NSRunLoopCommonModes,
    NSThread,
    NSDefaultRunLoopMode,
)
from Foundation import NSDate, NSString, NSObject

from . import guest_abi as abi
from .guest_pe import UnsupportedApi


def _color(r, g, b, a=1.0):
    return NSColor.colorWithCalibratedRed_green_blue_alpha_(r, g, b, a)


def point_for_event(view, event):
    return view.convertPoint_fromView_(event.locationInWindow(), None)


def draw_text(text, x, y, width, height, size=12.0, color=None, align="left"):
    color = color or NSColor.blackColor()
    attrs = {
        NSFontAttributeName: NSFont.systemFontOfSize_(size),
        NSForegroundColorAttributeName: color,
    }
    s = NSString.stringWithString_(text)
    sz = s.sizeWithAttributes_(attrs)
    xx = x
    if align == "center":
        xx = x + max(0.0, (width - sz.width) / 2.0)
    elif align == "right":
        xx = x + max(0.0, width - sz.width)
    s.drawAtPoint_withAttributes_(NSMakePoint(xx, y), attrs)


class CompatWindow(NSWindow):
    def canBecomeKeyWindow(self):
        return True

    def canBecomeMainWindow(self):
        return True


class CompatStatic(NSView):
    def isFlipped(self):
        return True

    def initWithFrame_(self, frame):
        self = objc.super(CompatStatic, self).initWithFrame_(frame)
        if self is None:
            return None
        self._text = ""
        return self

    def setString_(self, text):
        self._text = text
        self.setNeedsDisplay_(True)

    def drawRect_(self, rect):
        draw_text(self._text, 0, 2, self.bounds().size.width, self.bounds().size.height,
                  size=12.0)


class CompatButton(NSView):
    def isFlipped(self):
        return True

    def initWithFrame_(self, frame):
        self = objc.super(CompatButton, self).initWithFrame_(frame)
        if self is None:
            return None
        self._text = ""
        self._backend = None
        self._handle = 0
        self._pressed = False
        return self

    def setTitle_(self, text):
        self._text = text
        self.setNeedsDisplay_(True)

    def drawRect_(self, rect):
        bounds = self.bounds()
        base = _color(0.86, 0.86, 0.86) if not self._pressed else _color(0.72, 0.72, 0.72)
        base.set()
        NSBezierPath.fillRect_(bounds)
        _color(0.35, 0.35, 0.35).set()
        NSBezierPath.strokeRect_(NSInsetRect(bounds, 0.5, 0.5))
        draw_text(self._text, 0, 5, bounds.size.width, bounds.size.height,
                  size=12.0, align="center")

    def mouseDown_(self, event):
        self._pressed = True
        self.setNeedsDisplay_(True)

    def mouseUp_(self, event):
        was = self._pressed
        self._pressed = False
        self.setNeedsDisplay_(True)
        if not was:
            return
        point = self.convertPoint_fromView_(event.locationInWindow(), None)
        if self.bounds().containsPoint_(point):
            self.activate()

    def activate(self):
        if self._backend is not None:
            self._backend.on_button_clicked(self._handle)


class CompatEdit(NSView):
    def isFlipped(self):
        return True

    def initWithFrame_(self, frame):
        self = objc.super(CompatEdit, self).initWithFrame_(frame)
        if self is None:
            return None
        self._text = ""
        self._backend = None
        self._handle = 0
        return self

    def acceptsFirstResponder(self):
        return True

    def setString_(self, text):
        self._text = text
        self.setNeedsDisplay_(True)

    def string(self):
        return self._text

    def drawRect_(self, rect):
        bounds = self.bounds()
        _color(1.0, 1.0, 1.0).set()
        NSBezierPath.fillRect_(bounds)
        _color(0.45, 0.45, 0.45).set()
        NSBezierPath.strokeRect_(NSInsetRect(bounds, 0.5, 0.5))
        draw_text(self._text, 5, 5, bounds.size.width - 10, bounds.size.height,
                  size=12.0)
        if self.window() is not None and self.window().firstResponder() is self:
            tw = NSString.stringWithString_(self._text).sizeWithAttributes_({
                NSFontAttributeName: NSFont.systemFontOfSize_(12.0),
            }).width
            _color(0.1, 0.1, 0.1).set()
            NSBezierPath.fillRect_(NSMakeRect(5 + tw + 1, 6, 1, 14))

    def mouseDown_(self, event):
        self.window().makeFirstResponder_(self)

    def keyDown_(self, event):
        chars = str(event.characters())
        if not chars:
            return
        changed = False
        for ch in chars:
            code = ord(ch)
            if code in (8, 127):  # backspace / delete
                if self._text:
                    self._text = self._text[:-1]
                    changed = True
            elif code == 27:  # escape
                break
            elif code in (10, 13):
                continue
            elif code >= 32:
                self._text += ch
                changed = True
        if changed:
            self.setNeedsDisplay_(True)
            if self._backend is not None:
                self._backend.on_edit_changed(self._handle)


class CompatRootView(NSView):
    TITLE_H = 26.0
    MENU_H = 24.0

    def isFlipped(self):
        return True

    def initWithFrame_(self, frame):
        self = objc.super(CompatRootView, self).initWithFrame_(frame)
        if self is None:
            return None
        self._backend = None
        self._handle = 0
        self._title = ""
        self._menu_items = []
        self._menu_open = False
        self._popup_hits = []
        return self

    def setTitle_(self, title):
        self._title = title
        self.setNeedsDisplay_(True)

    def setMenuItems_(self, items):
        self._menu_items = items
        self._menu_open = False
        self.setNeedsDisplay_(True)

    def drawRect_(self, rect):
        bounds = self.bounds()
        w = bounds.size.width
        h = bounds.size.height
        _color(0.94, 0.94, 0.94).set()
        NSBezierPath.fillRect_(bounds)
        # Windows-like title bar.
        _color(0.08, 0.18, 0.42).set()
        NSBezierPath.fillRect_(NSMakeRect(0, 0, w, self.TITLE_H))
        draw_text(self._title, 10, 5, w - 48, 18, size=13.0,
                  color=NSColor.whiteColor())
        _color(0.72, 0.20, 0.20).set()
        NSBezierPath.fillRect_(NSMakeRect(w - 28, 4, 20, 18))
        draw_text("×", w - 28, 4, 20, 18, size=13.0,
                  color=NSColor.whiteColor(), align="center")
        # Menu bar.
        _color(0.91, 0.91, 0.91).set()
        NSBezierPath.fillRect_(NSMakeRect(0, self.TITLE_H, w, self.MENU_H))
        _color(0.65, 0.65, 0.65).set()
        NSBezierPath.fillRect_(NSMakeRect(0, self.TITLE_H + self.MENU_H - 1, w, 1))
        label = "文件(F)"
        if self._menu_items:
            label = self._menu_items[0].get("label", label)
        if self._menu_open:
            _color(0.72, 0.80, 0.92).set()
            NSBezierPath.fillRect_(NSMakeRect(4, self.TITLE_H + 2, 68, self.MENU_H - 4))
        draw_text(label, 12, self.TITLE_H + 4, 80, 18, size=12.0)
        self._popup_hits = []
        if self._menu_open and self._menu_items:
            items = self._menu_items[0].get("items", [])
            row_h = 26.0
            top = self.TITLE_H + self.MENU_H
            popup_w = 220.0
            popup_h = row_h * max(1, len(items))
            _color(1.0, 1.0, 1.0).set()
            NSBezierPath.fillRect_(NSMakeRect(4, top, popup_w, popup_h))
            _color(0.35, 0.35, 0.35).set()
            NSBezierPath.strokeRect_(NSMakeRect(4.5, top + 0.5, popup_w - 1, popup_h - 1))
            y = top
            for item in items:
                if item.get("kind") == "separator":
                    _color(0.75, 0.75, 0.75).set()
                    NSBezierPath.fillRect_(NSMakeRect(10, y + row_h / 2, popup_w - 12, 1))
                elif item.get("kind") == "item":
                    draw_text(item["label"], 16, y + 5, popup_w - 20, row_h - 4,
                              size=12.0)
                    self._popup_hits.append(
                        (NSMakeRect(4, y, popup_w, row_h), item["id"]))
                y += row_h

    def mouseDown_(self, event):
        p = point_for_event(self, event)
        w = self.bounds().size.width
        if p.y < self.TITLE_H and p.x >= w - 32:
            if self._backend is not None:
                self._backend.request_close(self._handle)
            return
        if self.TITLE_H <= p.y < self.TITLE_H + self.MENU_H and p.x < 76:
            self._menu_open = not self._menu_open
            self.setNeedsDisplay_(True)
            return
        if self._menu_open:
            for rect, item_id in self._popup_hits:
                if (rect.origin.x <= p.x <= rect.origin.x + rect.size.width and
                        rect.origin.y <= p.y <= rect.origin.y + rect.size.height):
                    self._menu_open = False
                    self.setNeedsDisplay_(True)
                    if self._backend is not None:
                        self._backend.on_menu_item(item_id)
                    return
            self._menu_open = False
            self.setNeedsDisplay_(True)


class CompatModalView(NSView):
    def isFlipped(self):
        return True

    def initWithFrame_(self, frame):
        self = objc.super(CompatModalView, self).initWithFrame_(frame)
        if self is None:
            return None
        self._owner = None
        self._text = ""
        self._caption = ""
        self._buttons = []
        return self

    def drawRect_(self, rect):
        bounds = self.bounds()
        _color(0.94, 0.94, 0.94).set()
        NSBezierPath.fillRect_(bounds)
        _color(0.08, 0.18, 0.42).set()
        NSBezierPath.fillRect_(NSMakeRect(0, 0, bounds.size.width, 26))
        draw_text(self._caption, 10, 5, bounds.size.width - 20, 18,
                  color=NSColor.whiteColor(), size=13.0)
        y = 42
        for line in self._text.split("\n"):
            draw_text(line, 18, y, bounds.size.width - 36, 20, size=12.0)
            y += 20
        for (label, result), rect in self._buttons:
            _color(0.86, 0.86, 0.86).set()
            NSBezierPath.fillRect_(rect)
            _color(0.35, 0.35, 0.35).set()
            NSBezierPath.strokeRect_(NSInsetRect(rect, 0.5, 0.5))
            draw_text(label, rect.origin.x, rect.origin.y + 5,
                      rect.size.width, rect.size.height, size=12.0, align="center")

    def mouseDown_(self, event):
        p = self.convertPoint_fromView_(event.locationInWindow(), None)
        for (label, result), rect in self._buttons:
            if (rect.origin.x <= p.x <= rect.origin.x + rect.size.width and
                    rect.origin.y <= p.y <= rect.origin.y + rect.size.height):
                if self._owner is not None:
                    self._owner.finish_(result)
                return


class CompatTimerHelper(NSObject):
    def initWithCallback_(self, callback):
        self = objc.super(CompatTimerHelper, self).init()
        if self is None:
            return None
        self._callback = callback
        return self

    def fire_(self, timer):
        self._callback()


class CompatBackend:
    name = "compat"

    def __init__(self, host, logger, evidence_dir: str):
        self.host = host
        self.logger = logger
        self.evidence_dir = evidence_dir
        self.windows: dict[int, NSWindow] = {}
        self.roots: dict[int, CompatRootView] = {}
        self.controls: dict[int, NSView] = {}
        self.menu_items: dict[int, str] = {}
        self.main_handle = 0
        self._automating = False
        self.backend_used = True

    def create_main_window(self, handle, title, x, y, width, height):
        w = max(width, 520)
        h = max(height, 190)
        window = CompatWindow.alloc().initWithContentRect_styleMask_backing_defer_(
            NSMakeRect(260, 300, w, h), NSWindowStyleMaskBorderless,
            NSBackingStoreBuffered, False)
        window.setReleasedWhenClosed_(False)
        root = CompatRootView.alloc().initWithFrame_(NSMakeRect(0, 0, w, h))
        root._backend = self
        root._handle = handle
        root.setTitle_(title)
        window.setContentView_(root)
        self.windows[handle] = window
        self.roots[handle] = root
        self.main_handle = handle
        self.logger.event("compat.window_created", handle=f"0x{handle:x}",
                          objc_class=str(window.className()), title=title,
                          thread=threading.current_thread().name,
                          main_thread=bool(NSThread.isMainThread()),
                          backend_note="P0 semantic harness; not final compat runtime")

    def create_control(self, handle, kind, text, style, x, y, width, height, ctrl_id):
        root = self.roots[self.main_handle]
        # Guest coordinates are client-area coordinates; this backend draws
        # its own Windows-like title and menu bars inside the content view.
        client_offset = root.TITLE_H + root.MENU_H
        rect = NSMakeRect(x, y + client_offset, width, height)
        if kind == "static":
            view = CompatStatic.alloc().initWithFrame_(rect)
            view.setString_(text)
        elif kind == "edit":
            view = CompatEdit.alloc().initWithFrame_(rect)
            view._backend = self
            view._handle = handle
            view.setString_(text)
        elif kind == "button":
            view = CompatButton.alloc().initWithFrame_(rect)
            view._backend = self
            view._handle = handle
            view.setTitle_(text)
        else:
            raise UnsupportedApi(f"compat backend: unsupported control kind {kind!r}")
        root.addSubview_(view)
        self.controls[handle] = view
        self.logger.event("compat.control_created", handle=f"0x{handle:x}",
                          control_kind=kind, objc_class=str(view.className()),
                          control_id=ctrl_id, text=text,
                          thread=threading.current_thread().name,
                          main_thread=bool(NSThread.isMainThread()))

    def show_window(self, handle):
        window = self.windows[handle]
        window.makeKeyAndOrderFront_(None)
        window.makeFirstResponder_(window.contentView())
        NSApp.activateIgnoringOtherApps_(True)
        self.logger.event("compat.show_window", handle=f"0x{handle:x}",
                          window_number=window.windowNumber(),
                          thread=threading.current_thread().name,
                          main_thread=bool(NSThread.isMainThread()))

    def close_window(self, handle):
        window = self.windows.get(handle)
        if window is not None:
            window.orderOut_(None)
            self.logger.event("compat.close_window", handle=f"0x{handle:x}")

    def window_number(self, handle):
        window = self.windows.get(handle)
        return window.windowNumber() if window is not None else None

    def pump(self, seconds: float):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            NSApp.nextEventMatchingMask_untilDate_inMode_dequeue_(
                (1 << 24) - 1, NSDate.dateWithTimeIntervalSinceNow_(0.02),
                NSDefaultRunLoopMode, True)
            NSApp.updateWindows()
            self.host.drain_posted_messages()

    def set_text(self, handle, text):
        info = self.host.windows[handle]
        if info.kind == "main":
            self.roots[handle].setTitle_(text)
            self.windows[handle].setTitle_(text)
        elif info.kind == "button":
            self.controls[handle].setTitle_(text)
        elif info.kind in ("static", "edit"):
            self.controls[handle].setString_(text)
        else:
            raise UnsupportedApi(f"compat set_text: unknown kind {info.kind}")

    def get_text(self, handle):
        info = self.host.windows[handle]
        if info.kind == "main":
            return self.roots[handle]._title
        if info.kind == "button":
            return self.controls[handle]._text
        if info.kind == "edit":
            return self.controls[handle].string()
        if info.kind == "static":
            return self.controls[handle]._text
        raise UnsupportedApi(f"compat get_text: unknown kind {info.kind}")

    def set_menu(self, handle, items):
        if handle != self.main_handle:
            raise UnsupportedApi("compat menu is only supported on the main window")
        self.roots[handle].setMenuItems_(items)
        for top in items:
            for child in top.get("items", []):
                if child.get("kind") == "item":
                    self.menu_items[child["id"]] = child["label"]
        self.logger.event("compat.menu_set", items=list(self.menu_items.keys()))

    def click_menu(self, item_id: int):
        if item_id not in self.menu_items:
            raise UnsupportedApi(f"compat menu item {item_id} not present")
        self._automating = True
        try:
            self.logger.event("automation.menu_click", item_id=item_id,
                              label=self.menu_items[item_id],
                              path="compat menu hit-test path")
            self.on_menu_item(item_id)
        finally:
            self._automating = False

    def click_button(self, handle: int):
        self._automating = True
        try:
            self.logger.event("automation.button_click", handle=f"0x{handle:x}",
                              path="CompatButton.mouseUp activation path")
            self.controls[handle].activate()
        finally:
            self._automating = False

    def set_edit_text(self, handle: int, text: str):
        view = self.controls[handle]
        view.setString_(text)
        self.logger.event("automation.edit_text", handle=f"0x{handle:x}",
                          text=text, path="CompatEdit.set_string + change path")
        self.on_edit_changed(handle)

    def on_button_clicked(self, handle: int):
        info = self.host.windows[handle]
        self.logger.event("compat.button_action", handle=f"0x{handle:x}",
                          control_id=info.control_id, automation=self._automating,
                          thread=threading.current_thread().name,
                          main_thread=bool(NSThread.isMainThread()))
        self.host.dispatch_command(info.control_id, abi.BN_CLICKED,
                                   source="compat custom button")

    def on_edit_changed(self, handle: int):
        info = self.host.windows[handle]
        self.host.dispatch_command(info.control_id, abi.EN_CHANGE,
                                   source="compat custom edit")

    def on_menu_item(self, item_id: int):
        self.logger.event("compat.menu_action", item_id=item_id,
                          automation=self._automating,
                          thread=threading.current_thread().name,
                          main_thread=bool(NSThread.isMainThread()))
        self.host.dispatch_menu(item_id, source="compat custom menu")

    def request_close(self, handle: int):
        self.logger.event("compat.title_close", handle=f"0x{handle:x}")
        self.host.send_sync(handle, abi.WM_CLOSE, 0, 0,
                            reason="compat title-bar close")

    def show_message_box(self, parent_handle, text, caption, style, capture):
        parent = self.windows.get(parent_handle)
        modal = CompatWindow.alloc().initWithContentRect_styleMask_backing_defer_(
            NSMakeRect(0, 0, 440, 190), NSWindowStyleMaskBorderless,
            NSBackingStoreBuffered, False)
        modal.setReleasedWhenClosed_(False)
        view = CompatModalView.alloc().initWithFrame_(NSMakeRect(0, 0, 440, 190))
        view._owner = None
        view._text = text
        view._caption = caption
        ok_rect = NSMakeRect(440 - 200, 150, 80, 28)
        cancel_rect = NSMakeRect(440 - 104, 150, 80, 28)
        view._buttons = [(("确定", abi.IDOK), ok_rect)]
        if style & abi.MB_OKCANCEL:
            view._buttons.append((("取消", abi.IDCANCEL), cancel_rect))
        modal.setContentView_(view)
        if parent is not None:
            frame = parent.frame()
            modal.setFrameOrigin_(NSMakePoint(
                frame.origin.x + (frame.size.width - 440) / 2,
                frame.origin.y + (frame.size.height - 190) / 2))
        modal.makeKeyAndOrderFront_(None)
        view._owner = _CompatModalOwner.alloc().initWithWindow_(modal)
        result = {"code": abi.IDCANCEL}

        def dismiss():
            number = modal.windowNumber()
            self.logger.event("compat.modal_shown", window_number=number,
                              caption=caption)
            if capture is not None:
                capture(f"03-modal-{self.name}.png", window_number=number)
            view._owner.finish_(abi.IDOK)

        helper = CompatTimerHelper.alloc().initWithCallback_(dismiss)
        timer = NSTimer.timerWithTimeInterval_target_selector_userInfo_repeats_(
            0.7, helper, "fire:", None, False)
        NSRunLoop.currentRunLoop().addTimer_forMode_(timer, NSRunLoopCommonModes)
        NSRunLoop.currentRunLoop().addTimer_forMode_(timer, NSModalPanelRunLoopMode)
        self.logger.event("compat.modal_run", caption=caption, style=f"0x{style:x}")
        result["code"] = NSApp.runModalForWindow_(modal)
        modal.orderOut_(None)
        self.logger.event("compat.modal_result", result=result["code"])
        return result["code"]

    def proof(self) -> dict:
        windows = [{"handle": f"0x{h:x}", "objc_class": str(w.className())}
                   for h, w in self.windows.items()]
        controls = [{"handle": f"0x{h:x}", "kind": self.host.windows[h].kind,
                     "objc_class": str(v.className())}
                    for h, v in self.controls.items()]
        for control in controls:
            if not control["objc_class"].startswith("Compat"):
                raise UnsupportedApi(
                    f"compat backend used a non-compat control {control}")
        return {
            "backend": "compat",
            "api": "custom Cocoa drawing/hit-testing",
            "windows": windows,
            "controls": controls,
            "native_classes_used": False,
            "main_thread": bool(NSThread.isMainThread()),
            "creation_thread": threading.current_thread().name,
            "note": ("P0 semantic harness, not the final compat runtime; "
                     "target arm64, no Rosetta; D3 runtime selection unresolved"),
        }


class _CompatModalOwner(NSObject):
    def initWithWindow_(self, window):
        self = objc.super(_CompatModalOwner, self).init()
        if self is None:
            return None
        self._window = window
        self._finished = False
        return self

    def finish_(self, code):
        if self._finished:
            return
        self._finished = True
        NSApp.stopModalWithCode_(code)
