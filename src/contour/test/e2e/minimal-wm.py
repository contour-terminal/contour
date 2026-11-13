#! /usr/bin/env python3
"""The smallest window manager the e2e-display workflows need, on the private Xvfb sidebar-workflows.py starts.

A bare X server has no window manager, and a frameless window asks one for two things a test must see work:
its moves (QWindow::startSystemMove sends _NET_WM_MOVERESIZE, which Qt only sends where the manager says it
takes it) and its focus. So this one maps what asks to be mapped, focuses and raises it, passes configure
requests through, and moves a window while the button that started a _NET_WM_MOVERESIZE is held. Nothing else:
no frames, no decorations, no workspaces.

libX11 through ctypes, so it needs nothing beyond what an X client already has. It ends on SIGTERM; the harness
kills it by its PID.
"""

import ctypes
import ctypes.util
import sys

x11 = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")

Window = ctypes.c_ulong
Atom = ctypes.c_ulong
Time = ctypes.c_ulong
Display = ctypes.c_void_p


class Any(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                ("display", Display), ("window", Window)]


class MapRequest(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                ("display", Display), ("parent", Window), ("window", Window)]


class ConfigureRequest(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                ("display", Display), ("parent", Window), ("window", Window), ("x", ctypes.c_int),
                ("y", ctypes.c_int), ("width", ctypes.c_int), ("height", ctypes.c_int),
                ("border_width", ctypes.c_int), ("above", Window), ("detail", ctypes.c_int),
                ("value_mask", ctypes.c_ulong)]


class ClientMessage(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                ("display", Display), ("window", Window), ("message_type", Atom), ("format", ctypes.c_int),
                ("l", ctypes.c_long * 5)]


class Pointer(ctypes.Structure):  # XMotionEvent and XButtonEvent alike, up to the fields read here
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                ("display", Display), ("window", Window), ("root", Window), ("subwindow", Window),
                ("time", Time), ("x", ctypes.c_int), ("y", ctypes.c_int), ("x_root", ctypes.c_int),
                ("y_root", ctypes.c_int), ("state", ctypes.c_uint), ("button", ctypes.c_uint)]


class XEvent(ctypes.Union):
    _fields_ = [("type", ctypes.c_int), ("any", Any), ("map", MapRequest), ("configure", ConfigureRequest),
                ("client", ClientMessage), ("pointer", Pointer), ("pad", ctypes.c_long * 24)]


class WindowChanges(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int), ("width", ctypes.c_int), ("height", ctypes.c_int),
                ("border_width", ctypes.c_int), ("sibling", Window), ("stack_mode", ctypes.c_int)]


x11.XOpenDisplay.restype = Display
x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
x11.XDefaultRootWindow.restype = Window
x11.XDefaultRootWindow.argtypes = [Display]
x11.XInternAtom.restype = Atom
x11.XInternAtom.argtypes = [Display, ctypes.c_char_p, ctypes.c_int]
x11.XCreateSimpleWindow.restype = Window
x11.XCreateSimpleWindow.argtypes = [Display, Window, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint,
                                    ctypes.c_uint, ctypes.c_ulong, ctypes.c_ulong]
x11.XChangeProperty.argtypes = [Display, Window, Atom, Atom, ctypes.c_int, ctypes.c_int, ctypes.c_void_p,
                                ctypes.c_int]
x11.XSelectInput.argtypes = [Display, Window, ctypes.c_long]
x11.XNextEvent.argtypes = [Display, ctypes.POINTER(XEvent)]
x11.XMapWindow.argtypes = [Display, Window]
x11.XRaiseWindow.argtypes = [Display, Window]
x11.XSetInputFocus.argtypes = [Display, Window, ctypes.c_int, Time]
x11.XConfigureWindow.argtypes = [Display, Window, ctypes.c_uint, ctypes.POINTER(WindowChanges)]
x11.XMoveWindow.argtypes = [Display, Window, ctypes.c_int, ctypes.c_int]
x11.XGrabPointer.argtypes = [Display, Window, ctypes.c_int, ctypes.c_uint, ctypes.c_int, ctypes.c_int, Window,
                             ctypes.c_ulong, Time]
x11.XUngrabPointer.argtypes = [Display, Time]
x11.XSync.argtypes = [Display, ctypes.c_int]
x11.XTranslateCoordinates.argtypes = [Display, Window, Window, ctypes.c_int, ctypes.c_int,
                                      ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                                      ctypes.POINTER(Window)]

ERROR_HANDLER = ctypes.CFUNCTYPE(ctypes.c_int, Display, ctypes.c_void_p)
x11.XSetErrorHandler.argtypes = [ERROR_HANDLER]

BUTTON_RELEASE, MOTION_NOTIFY, MAP_REQUEST, CONFIGURE_REQUEST, CLIENT_MESSAGE = 5, 6, 20, 23, 33
SUBSTRUCTURE_REDIRECT, SUBSTRUCTURE_NOTIFY = 1 << 20, 1 << 19
POINTER_MOTION, BUTTON_RELEASE_MASK = 1 << 6, 1 << 3
GRAB_ASYNC, REVERT_TO_POINTER_ROOT, PROP_REPLACE, XA_ATOM, XA_WINDOW = 1, 1, 0, 4, 33
MOVERESIZE_MOVE, MOVERESIZE_CANCEL = 8, 11


def main():
    display = x11.XOpenDisplay(None)
    if not display:
        print("minimal-wm: cannot open the display", file=sys.stderr)
        return 1
    # A window that went away between its request and the answer is not an error worth ending over.
    ignore_errors = ERROR_HANDLER(lambda _display, _event: 0)
    x11.XSetErrorHandler(ignore_errors)

    def atom(name):
        return x11.XInternAtom(display, name.encode(), 0)

    root = x11.XDefaultRootWindow(display)
    x11.XSelectInput(display, root, SUBSTRUCTURE_REDIRECT | SUBSTRUCTURE_NOTIFY)

    move_resize = atom("_NET_WM_MOVERESIZE")
    active_window = atom("_NET_ACTIVE_WINDOW")
    supporting = atom("_NET_SUPPORTING_WM_CHECK")
    check = Window(x11.XCreateSimpleWindow(display, root, -1, -1, 1, 1, 0, 0, 0))
    for target in (root, check.value):
        x11.XChangeProperty(display, target, supporting, XA_WINDOW, 32, PROP_REPLACE, ctypes.byref(check), 1)
    name = b"minimal-wm"
    x11.XChangeProperty(display, check.value, atom("_NET_WM_NAME"), atom("UTF8_STRING"), 8, PROP_REPLACE,
                        name, len(name))
    supported = (Atom * 3)(move_resize, active_window, supporting)
    x11.XChangeProperty(display, root, atom("_NET_SUPPORTED"), XA_ATOM, 32, PROP_REPLACE, supported, 3)
    x11.XSync(display, 0)

    moving = None  # (window, pointer x, pointer y, window x, window y) while a move runs
    event = XEvent()
    while True:
        x11.XNextEvent(display, ctypes.byref(event))
        kind = event.type
        if kind == MAP_REQUEST:
            window = event.map.window
            x11.XMapWindow(display, window)
            x11.XRaiseWindow(display, window)
            x11.XSetInputFocus(display, window, REVERT_TO_POINTER_ROOT, 0)
        elif kind == CONFIGURE_REQUEST:
            request = event.configure
            changes = WindowChanges(request.x, request.y, request.width, request.height, request.border_width,
                                    request.above, request.detail)
            x11.XConfigureWindow(display, request.window, request.value_mask & 0x7F, ctypes.byref(changes))
        elif kind == CLIENT_MESSAGE and event.client.message_type == active_window:
            x11.XRaiseWindow(display, event.client.window)
            x11.XSetInputFocus(display, event.client.window, REVERT_TO_POINTER_ROOT, 0)
        elif kind == CLIENT_MESSAGE and event.client.message_type == move_resize:
            direction = event.client.l[2]
            if direction == MOVERESIZE_CANCEL and moving:
                x11.XUngrabPointer(display, 0)
                moving = None
            elif direction == MOVERESIZE_MOVE:
                wx, wy, child = ctypes.c_int(), ctypes.c_int(), Window()
                x11.XTranslateCoordinates(display, event.client.window, root, 0, 0, ctypes.byref(wx),
                                          ctypes.byref(wy), ctypes.byref(child))
                grabbed = x11.XGrabPointer(display, root, 0, POINTER_MOTION | BUTTON_RELEASE_MASK, GRAB_ASYNC,
                                           GRAB_ASYNC, 0, 0, 0)
                if grabbed == 0:
                    moving = (event.client.window, event.client.l[0], event.client.l[1], wx.value, wy.value)
        elif kind == MOTION_NOTIFY and moving:
            window, px, py, wx, wy = moving
            x11.XMoveWindow(display, window, wx + event.pointer.x_root - px, wy + event.pointer.y_root - py)
        elif kind == BUTTON_RELEASE and moving:
            x11.XUngrabPointer(display, 0)
            moving = None
        x11.XSync(display, 0)


if __name__ == "__main__":
    sys.exit(main())
