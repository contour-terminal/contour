#! /usr/bin/env python3
"""The sidebar's workflows, end to end, on a real X server: the contour_e2e_display_* tests (label e2e-display).

Each case starts its own Xvfb, a minimal window manager (minimal-wm.py, for the window moves and the focus a
frameless window asks of one), a private daemon and `contour client` on it, then drives the window with real X
pointer and keyboard events through xdotool -- so a drag runs QDrag's real drag loop, which no offscreen test
can -- and asserts what happened through the control API (`contour api call workspace.list`). What the window
draws is asserted off the X server's own pixels, in the one place the control API cannot see: that every terminal
pane a case ends showing draws its terminal (expect_panes_draw), told by colours only the terminal uses.

A multi-daemon case (`multi_daemon_*`) starts two more local daemons in the same private runtime directory before the
client, each with a workspace of its own, so the window's sidebar lists three daemons; a jump it wants held under way
stops the target daemon's server (SIGSTOP, by the PID of the recorded daemon's child) until it releases it.

Where things are on screen comes from the window itself: CONTOUR_TEST_GEOMETRY_FILE makes the GUI write every
named item's screen rectangle there (window::GeometryDump), so no pixel position is hard-coded here.

Usage: sidebar-workflows.py /path/to/contour CASE      (or `--list` for the case names)

Opt-in: runs only with CONTOUR_E2E_DISPLAY=1 in the environment (CI's e2e-display step sets it), so a plain
`ctest` never starts X servers and GUI clients. Exits 77 -- CTest's SKIP_RETURN_CODE -- with a message when it
is not set, or when Xvfb or xdotool is missing. Every process it starts
is recorded by its PID and killed by that PID; nothing is ever killed by name.
"""

import ctypes
import ctypes.util
import json
import os
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import traceback

SKIP = 77
# The variable that turns the cases on; without it every case skips.
OPT_IN = "CONTOUR_E2E_DISPLAY"
HERE = os.path.dirname(os.path.abspath(__file__))
LABEL = "e2e-display"


class Failure(Exception):
    """A workflow did not end as it should."""


def log(message):
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def deadline_loop(seconds, what, probe, interval=0.1):
    """Calls probe() until it returns something truthy, for at most `seconds`; raises Failure naming `what`."""
    begin = time.monotonic()
    end = begin + seconds
    last = None
    while time.monotonic() < end:
        last = probe()
        if last:
            if time.monotonic() - begin > 3:
                log(f"waited {time.monotonic() - begin:.1f} s for {what}")
            return last
        time.sleep(interval)
    raise Failure(f"timed out after {seconds} s waiting for {what}")


class Desktop:
    """One private X server, window manager, daemon and client; torn down by PID in any case."""

    def __init__(self, contour, tab_placement="sidebar", in_process=False, others=()):
        self.contour = contour
        # The labels of the local daemons started beside the window's own, each with one workspace of its own
        # (OTHERS): the sidebar lists every local daemon that runs, so they head sections of their own.
        self.others = tuple(others)
        # No daemon: the window's sessions live in the GUI process, which serves no control API, so a case
        # reads everything it asserts from the geometry dump.
        self.in_process = in_process
        self.procs = []  # (name, Popen), in start order
        self.held = set()  # the PIDs of the daemon servers a case stopped (hold), resumed at the latest by stop()
        self.root = tempfile.mkdtemp(prefix="xdg", dir="/tmp")  # short: it holds the daemon's socket
        self.display = None
        self.drags = 0
        self.before_drop = None  # the dump just before the last drag's release
        self.bus = None  # the private session bus's address, once started
        self.geometry_file = os.path.join(self.root, "geometry.json")
        for sub in ("run", "config/contour", "state", "cache", "home"):
            os.makedirs(os.path.join(self.root, sub), exist_ok=True)
        os.chmod(os.path.join(self.root, "run"), 0o700)
        with open(os.path.join(self.root, "config/contour/contour.yml"), "w") as config:
            config.write(CONFIG.format(tab_placement=tab_placement,
                                       daemon_mode="in_process" if in_process else "auto",
                                       background=hex_colour(TERMINAL_BACKGROUND),
                                       foreground=hex_colour(TERMINAL_FOREGROUND)))
            if self.others:
                config.write(MULTI_DAEMON_BINDINGS)

    # {{{ environment
    def env(self, gui=False, daemon=LABEL):
        env = {
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "HOME": os.path.join(self.root, "home"),
            "XDG_RUNTIME_DIR": os.path.join(self.root, "run"),
            "XDG_CONFIG_HOME": os.path.join(self.root, "config"),
            "XDG_STATE_HOME": os.path.join(self.root, "state"),
            "XDG_CACHE_HOME": os.path.join(self.root, "cache"),
            "LANG": "C.UTF-8",
            "LC_ALL": "C.UTF-8",
            "TERM": "xterm-256color",
        }
        # Sanitizer settings pass through (LSAN_OPTIONS from CTest); nothing of the developer's session does:
        # no CONTOUR_PANE/DAEMON/MUX, no session bus, no Wayland socket, no other display.
        for name in ("ASAN_OPTIONS", "LSAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS", "LIBGL_ALWAYS_SOFTWARE",
                     "QT_LOGGING_RULES"):
            if name in os.environ:
                env[name] = os.environ[name]
        # The control verbs reach the private daemon through it (or, with `daemon`, one of the others). An
        # in-process window is given none to find.
        if not self.in_process:
            env["CONTOUR_DAEMON"] = self.socket(daemon)
        if gui:
            if self.bus:
                env["DBUS_SESSION_BUS_ADDRESS"] = self.bus
            env["DISPLAY"] = self.display
            env["QT_QPA_PLATFORM"] = "xcb"
            env["LIBGL_ALWAYS_SOFTWARE"] = "1"
            env["CONTOUR_TEST_GEOMETRY_FILE"] = self.geometry_file
            env["QT_ACCESSIBILITY"] = "0"
        return env
    # }}}

    # {{{ processes
    def spawn(self, name, argv, env, **kwargs):
        logfile = open(os.path.join(self.root, f"{name}.log"), "wb")
        proc = subprocess.Popen(argv, env=env, stdout=logfile, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, start_new_session=True, **kwargs)
        logfile.close()
        self.procs.append((name, proc))
        log(f"started {name} (pid {proc.pid})")
        return proc

    def alive(self, name):
        return any(n == name and p.poll() is None for n, p in self.procs)

    def spawn_reporting(self, name, argv, what, seconds=30):
        """Starts a process that writes one line to the file descriptor its argv names as {fd} (Xvfb's display
        number, the bus's address), and returns that line."""
        read_fd, write_fd = os.pipe()
        try:
            self.spawn(name, [arg.format(fd=write_fd) for arg in argv], self.env(), pass_fds=(write_fd,))
            os.close(write_fd)
            line = b""
            end = time.monotonic() + seconds
            while not line.endswith(b"\n"):
                ready, _, _ = select.select([read_fd], [], [], max(end - time.monotonic(), 0))
                chunk = os.read(read_fd, 256) if ready else b""
                if not chunk:
                    raise Failure(f"{name} did not report {what} within {seconds} s:\n" + self.read_log(name))
                line += chunk
            return line.decode().strip()
        finally:
            os.close(read_fd)

    def start_xvfb(self):
        number = self.spawn_reporting("xvfb", ["Xvfb", "-displayfd", "{fd}", "-screen", "0", "1600x1000x24",
                                               "-nolisten", "tcp", "-noreset"], "its display")
        self.display = ":" + number
        log(f"Xvfb on {self.display}")

    def start_bus(self):
        """A private session bus with no services for the client, as scripts/run-display-tests.sh gives its
        tests: without one, the first D-Bus call autolaunches a bus on the X display, which activates the
        desktop's portals -- processes this harness never started and could not kill by their PIDs."""
        if shutil.which("dbus-daemon") is None:
            return
        config = os.path.join(self.root, "bus.conf")
        with open(config, "w") as f:
            f.write(BUS_CONFIG.format(dir=self.root))
        self.bus = self.spawn_reporting("bus", ["dbus-daemon", f"--config-file={config}", "--nofork",
                                                "--print-address={fd}"], "its address")

    def socket(self, label):
        """The control socket of the daemon labelled so, in the private runtime directory discovery lists."""
        return os.path.join(self.root, "run", "contour", label)

    def start_other(self, label):
        """Starts the local daemon `label` beside the window's own, and gives it one workspace whose tab is named
        `<label>-t` (`<label>-ws` the workspace): what its section in the sidebar lists."""
        self.spawn(f"daemon-{label}", [self.contour, "daemon", "--name", label], self.env(daemon=label))
        deadline_loop(30, f"the daemon {label}", lambda: self.cli_ok("ls", daemon=label))
        workspace = self.cli("workspace", "new", daemon=label)
        # A daemon no client attached to yet holds one empty workspace; the new one is what the section lists.
        for empty in self.workspaces(daemon=label):
            if empty["id"] != workspace and not empty["tabs"]:
                self.cli("workspace", "close", empty["id"], daemon=label)
        self.cli("rename", workspace, f"{label}-ws", daemon=label)
        tab = next(w for w in self.workspaces(daemon=label) if w["id"] == workspace)["tabs"][0]
        self.cli("rename", tab["id"], f"{label}-t", daemon=label)

    def start(self):
        self.start_bus()
        self.start_xvfb()
        for label in self.others:
            self.start_other(label)
        self.spawn("wm", [sys.executable, "-I", os.path.join(HERE, "minimal-wm.py")], self.env(gui=True))
        deadline_loop(15, "the window manager", lambda: self.wm_ready())
        debug = ["--log", os.environ["CONTOUR_E2E_DEBUG"]] if os.environ.get("CONTOUR_E2E_DEBUG") else []
        if self.in_process:
            self.spawn("client", [self.contour, "terminal", *debug], self.env(gui=True))
        else:
            self.spawn("daemon", [self.contour, "daemon", "--name", LABEL], self.env())
            deadline_loop(30, "the daemon", lambda: self.cli_ok("ls"))
            self.spawn("client", [self.contour, "client", "--daemon", "name:" + LABEL, *debug], self.env(gui=True))
        deadline_loop(60, "the client's window", self.window_id, interval=0.25)
        deadline_loop(60, "the client's geometry dump", lambda: self.find("sidebarTree", required=False),
                      interval=0.25)

    def wm_ready(self):
        if not self.alive("wm"):
            raise Failure("the window manager exited:\n" + self.read_log("wm"))
        result = self.run(["xprop", "-root", "_NET_SUPPORTING_WM_CHECK"], gui=True, check=False)
        return "window id" in result.stdout

    def read_log(self, name):
        try:
            with open(os.path.join(self.root, f"{name}.log"), errors="replace") as f:
                return f.read()[-6000:]
        except OSError:
            return ""

    def stop(self):
        # A daemon a case held stopped (hold()) runs again, so it can take its SIGTERM.
        for pid in self.held:
            try:
                os.kill(pid, signal.SIGCONT)
            except ProcessLookupError:
                pass
        # Client first, so it does not reconnect, then the daemon, the window manager and the server.
        for name, proc in reversed(self.procs):
            if proc.poll() is None:
                try:
                    os.kill(proc.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    proc.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    log(f"{name} (pid {proc.pid}) ignored SIGTERM; killing it")
                    try:
                        os.kill(proc.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    try:
                        proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        log(f"{name} (pid {proc.pid}) did not die")
        if os.environ.get("CONTOUR_E2E_KEEP"):
            log(f"kept {self.root}")
        else:
            shutil.rmtree(self.root, ignore_errors=True)
    # }}}

    # {{{ commands
    def run(self, argv, gui=False, check=True, timeout=30, daemon=LABEL):
        result = subprocess.run(argv, env=self.env(gui=gui, daemon=daemon), capture_output=True, text=True,
                                timeout=timeout, stdin=subprocess.DEVNULL)
        if check and result.returncode != 0:
            raise Failure(f"{' '.join(argv)} exited {result.returncode}: {result.stderr.strip()}")
        return result

    def cli(self, *args, check=True, daemon=LABEL):
        return self.run([self.contour, *args], check=check, daemon=daemon).stdout.strip()

    def cli_ok(self, *args, daemon=LABEL):
        process = "daemon" if daemon == LABEL else f"daemon-{daemon}"
        if not self.alive(process):
            raise Failure(f"the daemon {daemon} exited:\n" + self.read_log(process))
        try:
            return self.run([self.contour, *args], check=False, daemon=daemon).returncode == 0
        except subprocess.TimeoutExpired:
            return False

    def api(self, method, request=None, daemon=LABEL):
        out = self.run([self.contour, "api", "call", method, json.dumps(request or {})], daemon=daemon).stdout
        return json.loads(out)

    def workspaces(self, daemon=LABEL):
        return self.api("workspace.list", daemon=daemon)["workspaces"]

    def xdotool(self, *args):
        return self.run(["xdotool", *[str(a) for a in args]], gui=True).stdout.strip()

    def window_id(self):
        if not self.alive("client"):
            raise Failure("the client exited:\n" + self.read_log("client"))
        result = self.run(["xdotool", "search", "--onlyvisible", "--pid", str(self.pid("client"))], gui=True,
                          check=False)
        ids = result.stdout.split()
        return ids[0] if ids else None

    def pid(self, name):
        return next(p.pid for n, p in self.procs if n == name)

    def server_pid(self, label):
        """The PID of the process that serves the daemon `label`: `contour daemon` keeps its server as a child
        (`daemon keeper-serve`), which the PID this harness recorded is the parent of."""
        keeper = self.pid("daemon" if label == LABEL else f"daemon-{label}")
        with open(f"/proc/{keeper}/task/{keeper}/children") as f:
            children = [int(c) for c in f.read().split()]
        for child in children:
            with open(f"/proc/{child}/cmdline", "rb") as f:
                if b"keeper-serve" in f.read():
                    return child
        raise Failure(f"the daemon {label} (pid {keeper}) has no server child")

    def hold(self, label):
        """Stops the daemon `label`'s server (SIGSTOP), so a jump to it stays under way until release()."""
        pid = self.server_pid(label)
        os.kill(pid, signal.SIGSTOP)
        self.held.add(pid)
        log(f"held {label} (pid {pid})")

    def release(self, label):
        pid = self.server_pid(label)
        os.kill(pid, signal.SIGCONT)
        self.held.discard(pid)
        log(f"released {label} (pid {pid})")
    # }}}

    # {{{ geometry
    def geometry(self):
        """The newest dump: {"windows": [{"x","y","width","height","items": [...]}]}."""
        try:
            with open(self.geometry_file) as f:
                return json.load(f)
        except (OSError, ValueError):
            return {"windows": []}

    def quiet(self, seconds=10, still=0.5):
        """Waits until the dump has not changed for `still` seconds: a row the list is still moving into place
        (its add and displace transitions) is not where the dump last saw it."""
        def unchanged():
            try:
                return time.time() - os.stat(self.geometry_file).st_mtime >= still
            except OSError:
                return False
        deadline_loop(seconds, "the window to stop moving", unchanged, interval=0.05)

    def items(self, name=None, dump=None, **props):
        found = []
        for window in (dump or self.geometry()).get("windows", []):
            for item in window.get("items", []):
                if name is not None and item.get("name") != name:
                    continue
                if all(item.get("props", {}).get(k) == v for k, v in props.items()):
                    found.append(item)
        return found

    def find(self, name, required=True, dump=None, **props):
        found = self.items(name, dump=dump, **props)
        if not found and required:
            raise Failure(f"no visible item {name} {props or ''} on screen")
        return found[0] if found else None

    def wait_item(self, name, seconds=15, **props):
        deadline_loop(seconds, f"item {name} {props or ''}", lambda: self.find(name, required=False, **props))
        self.quiet()
        return self.find(name, **props)

    def wait_gone(self, name, seconds=15, **props):
        return deadline_loop(seconds, f"item {name} {props or ''} to go",
                             lambda: self.find(name, required=False, **props) is None)
    # }}}

    # {{{ input
    @staticmethod
    def centre(item, fx=0.5, fy=0.5):
        return (int(item["x"] + item["width"] * fx), int(item["y"] + item["height"] * fy))

    def move(self, point):
        # Not --sync: it waits for the pointer to move, which it never does when it is already there.
        self.xdotool("mousemove", point[0], point[1])
        time.sleep(0.05)

    def click(self, point, button=1):
        self.move(point)
        time.sleep(0.15)
        self.xdotool("click", button)
        time.sleep(0.3)

    def drag(self, start, end, steps=24, hold=0.25):
        """A real press, a stepped move and a release, so Qt starts its drag and QDrag's loop follows the pointer."""
        self.move(start)
        time.sleep(0.2)
        self.xdotool("mousedown", 1)
        time.sleep(hold)
        for step in range(1, steps + 1):
            x = int(start[0] + (end[0] - start[0]) * step / steps)
            y = int(start[1] + (end[1] - start[1]) * step / steps)
            self.xdotool("mousemove", x, y)
            time.sleep(0.03)
        # Linger over the target, so the drop area sees the hover before the release.
        for wiggle in (1, -1, 0):
            self.xdotool("mousemove", end[0] + wiggle, end[1])
            time.sleep(0.15)
        log(f"drag {start} -> {end}: the pointer is at {self.xdotool('getmouselocation')}")
        # What the screen showed just before the drop: kept beside the logs (CONTOUR_E2E_KEEP) for a failure.
        self.drags += 1
        self.before_drop = os.path.join(self.root, f"before-drop-{self.drags}.json")
        shutil.copyfile(self.geometry_file, self.before_drop)
        self.xdotool("mouseup", 1)
        time.sleep(0.6)

    def key(self, *keys):
        self.xdotool("key", "--clearmodifiers", *keys)
        time.sleep(0.25)

    def type(self, text):
        """Types `text` into the focused window, as the keyboard would."""
        self.xdotool("type", "--clearmodifiers", "--delay", 20, text)
        time.sleep(0.25)
    # }}}

    # {{{ pixels
    def grab(self, item):
        """The pixels the X server shows in `item`'s rectangle, as rows of (r, g, b): what is on screen there,
        read straight off the root window (XGetImage), so a window that left the rectangle undrawn shows what
        lies behind it. libX11 through ctypes, so the check needs nothing Xvfb does not bring along."""
        x11 = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XDefaultRootWindow.restype = ctypes.c_ulong
        x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        x11.XGetImage.restype = ctypes.POINTER(XImage)
        x11.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint,
                                  ctypes.c_uint, ctypes.c_ulong, ctypes.c_int]
        x11.XDestroyImage.argtypes = [ctypes.POINTER(XImage)]
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        display = x11.XOpenDisplay(self.display.encode())
        if not display:
            raise Failure(f"cannot open the X display {self.display}")
        try:
            x, y, width, height = (int(item[k]) for k in ("x", "y", "width", "height"))
            image = x11.XGetImage(display, x11.XDefaultRootWindow(display), x, y, width, height,
                                  ctypes.c_ulong(-1).value, 2)  # AllPlanes, ZPixmap
            if not image:
                raise Failure(f"cannot read the screen at {x},{y} {width}x{height}")
            try:
                im = image.contents
                if im.bits_per_pixel != 32:
                    raise Failure(f"the screen has {im.bits_per_pixel} bits per pixel; the grab reads 32")
                data = ctypes.string_at(im.data, im.bytes_per_line * im.height)
                # 32-bit ZPixmap on a little-endian server (Xvfb's): B, G, R, unused.
                return [[(data[row * im.bytes_per_line + col * 4 + 2], data[row * im.bytes_per_line + col * 4 + 1],
                          data[row * im.bytes_per_line + col * 4]) for col in range(im.width)]
                        for row in range(im.height)]
            finally:
                x11.XDestroyImage(image)
        finally:
            x11.XCloseDisplay(display)

    def save_grab(self, pixels, name):
        """Writes `pixels` as a PPM beside the logs (kept with CONTOUR_E2E_KEEP), for a failure to be looked at."""
        path = os.path.join(self.root, f"{name}.ppm")
        with open(path, "wb") as f:
            f.write(f"P6 {len(pixels[0])} {len(pixels)} 255\n".encode())
            f.write(bytes(channel for row in pixels for pixel in row for channel in pixel))
        return path
    # }}}


class XImage(ctypes.Structure):
    """libX11's XImage, as far as its pixels are read (the function table after it is never touched)."""
    _fields_ = [("width", ctypes.c_int), ("height", ctypes.c_int), ("xoffset", ctypes.c_int),
                ("format", ctypes.c_int), ("data", ctypes.c_void_p), ("byte_order", ctypes.c_int),
                ("bitmap_unit", ctypes.c_int), ("bitmap_bit_order", ctypes.c_int), ("bitmap_pad", ctypes.c_int),
                ("depth", ctypes.c_int), ("bytes_per_line", ctypes.c_int), ("bits_per_pixel", ctypes.c_int)]


BUS_CONFIG = """\
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus-busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:dir={dir}</listen>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
"""

CONFIG = """\
defaults_edition: native2026
sidebar:
  visible: true
tab_placement: {tab_placement}
daemon:
  mode: {daemon_mode}
default_profile: main
profiles:
  main:
    shell: /bin/sh
    colors: e2e
color_schemes:
  e2e:
    default:
      background: '{background}'
      foreground: '{foreground}'
"""

# The local daemons a multi-daemon case starts beside the window's own (Desktop.others).
OTHERS = ("e2e-one", "e2e-two")

# What a multi-daemon case binds: `input_mapping` replaces the built-in bindings, so every chord these cases press
# is stated here, CycleDaemon's included (it has no default chord).
MULTI_DAEMON_BINDINGS = """\
input_mapping:
  - { mods: [Control, Shift], key: D, action: FocusSidebar }
  - { mods: [Control, Shift], key: J, action: JumpToNextAttention }
  - { mods: [Control, Shift, Alt], key: J, action: JumpBack }
  - { mods: [Control, Shift, Alt], key: RightArrow, action: CycleDaemon, direction: next }
"""

# The terminal's colours, which nothing else on the screen has: a pane that draws shows the background over
# most of it and the foreground somewhere (its prompt); one that does not shows neither (expect_panes_draw).
TERMINAL_BACKGROUND = (0x2B, 0x1A, 0x5C)
TERMINAL_FOREGROUND = (0xF0, 0xE6, 0x50)


def hex_colour(rgb):
    return "#" + "".join(f"{c:02X}" for c in rgb)


# {{{ helpers over the control API and the dump
WORKSPACE, TAB, PANE = 0, 1, 2  # window::SidebarRowKind
COLLAPSED, EXPANDED = 1, 2  # window::RowExpansion


def state(d):
    """[(workspace name, [(tab name, [pane id, ...]), ...]), ...] in order."""
    return [(w["name"], [(t["name"], [p["id"] for p in t["panes"]]) for t in w["tabs"]])
            for w in d.workspaces()]


def expect(condition, message, d=None):
    if not condition:
        detail = json.dumps(state(d), indent=1) if d else ""
        raise Failure(f"{message}\nstate: {detail}")


def settle(d, what, predicate, seconds=15):
    """Waits for the daemon's state to satisfy predicate(state)."""
    return deadline_loop(seconds, what, lambda: predicate(state(d)), interval=0.2)


def workspace_named(d, name):
    return next(w for w in d.workspaces() if w["name"] == name)


def row(d, title, kind, seconds=15, **props):
    """The sidebar's row titled so; with props, once it has them too (`selected=True`: the row the window
    shows)."""
    return d.wait_item("sidebarRow", seconds, title=title, kind=kind, **props)


def items_when(d, count, name, seconds=15, **props):
    """The visible items named so, once there are `count` of them and the window has stopped moving."""
    deadline_loop(seconds, f"{count} items {name} {props or ''}", lambda: len(d.items(name, **props)) == count)
    d.quiet()
    found = d.items(name, **props)
    expect(len(found) == count, f"{len(found)} items {name}, not {count}")
    return found


def near(pixel, colour, tolerance):
    return all(abs(a - b) <= tolerance for a, b in zip(pixel, colour))


def pane_drawing(d, pane):
    """What is on screen in a terminal pane's rectangle: (share of pixels in the terminal's background, count
    of pixels of its text, the pixels). Its foreground is a light yellow, which nothing else on the screen is,
    and which the unfocused dim keeps yellow: a channel well below the other two."""
    pixels = d.grab(pane)
    flat = [pixel for row in pixels for pixel in row]
    background = sum(1 for pixel in flat if near(pixel, TERMINAL_BACKGROUND, 40)) / max(len(flat), 1)
    text = sum(1 for r, g, b in flat if r > 110 and g > 100 and b < min(r, g) - 50)
    return background, text, pixels


def expect_panes_draw(d, seconds=15):
    """Every terminal pane the window shows draws: it is visible with a size, and what the screen shows in its
    rectangle is the terminal's background over most of it and its text (the prompt) somewhere -- not the
    window's clear colour or whatever lies behind the window, which is what an undrawn pane leaves.

    Read off the X server's own pixels (Desktop.grab), as nothing in the window's item tree can tell a display
    that renders from one that is merely there. For every case that ends showing another workspace or tab."""
    def drawn():
        panes = d.items("terminalPane")
        if not panes or any(p["width"] <= 0 or p["height"] <= 0 for p in panes):
            return None
        for pane in panes:
            background, text, _ = pane_drawing(d, pane)
            if background < 0.5 or text < 10:
                return None
        return panes
    try:
        deadline_loop(seconds, "every terminal pane to draw", drawn, interval=0.25)
    except Failure:
        panes = d.items("terminalPane")
        report = []
        for index, pane in enumerate(panes):
            background, text, pixels = pane_drawing(d, pane)
            corner = pixels[len(pixels) // 2][len(pixels[0]) // 2] if pixels and pixels[0] else None
            report.append(f"terminalPane {index} at {pane['x']},{pane['y']} {pane['width']}x{pane['height']}: "
                          f"{background:.0%} background, {text} text pixels, centre {corner}, saved to "
                          f"{d.save_grab(pixels, f'pane-{index}') if pixels and pixels[0] else '-'}")
        raise Failure("a terminal pane does not draw:\n  " + ("\n  ".join(report) or "no terminalPane on screen"))


def drop_onto(d, start, end, area):
    """Drags from `start` to `end`, then checks that the drop area `area` covering `end` held the drag just
    before the release: what lands is the drop that area takes, not a drag that went elsewhere or never began."""
    d.drag(start, end)
    with open(d.before_drop) as f:
        dump = json.load(f)
    held = [a for a in d.items(area, dump=dump) if a["props"].get("containsDrag")
            and a["x"] <= end[0] < a["x"] + a["width"] and a["y"] <= end[1] < a["y"] + a["height"]]
    expect(len(held) == 1, f"{area} under {end} did not hold the drag", d)


def band(item, which):
    """A point in a row's upper, middle or lower third (window::sidebarDropBandOf)."""
    return Desktop.centre(item, 0.5, {"upper": 0.12, "middle": 0.5, "lower": 0.88}[which])


def name_tab(d, tab, name):
    """Names a tab through the control API, and waits for the window to show it: a tab's name is what the
    sidebar and the tab strip call it."""
    d.cli("rename", tab["id"], name)
    deadline_loop(15, f"the tab named {name!r}",
                  lambda: d.find("sidebarRow", required=False, title=name, kind=TAB)
                  or d.find("tabItem", required=False, tabTitle=name), interval=0.2)


def title_shown_pane(d, title):
    """Titles the shown pane's terminal (OSC 2) by typing a printf into its shell."""
    d.type(f"printf '\\033]2;{title}\\007'\n")


def two_workspaces(d):
    """alpha (shown, one tab `a1`) and beta (one tab `b1`), both named through the control API."""
    d.cli("rename", "workspace:1", "alpha")
    name_tab(d, d.workspaces()[0]["tabs"][0], "a1")
    beta = d.cli("workspace", "new")
    d.cli("rename", beta, "beta")
    name_tab(d, workspace_named(d, "beta")["tabs"][0], "b1")
    row(d, "alpha", WORKSPACE)
    row(d, "beta", WORKSPACE)


def new_tab(d, workspace, name):
    """Opens a tab in a workspace, named `name`, and waits for the sidebar to show it."""
    d.cli("new", "--workspace", workspace, "--name", name)
    settle(d, f"the tab {name}", lambda s: name in tab_names(d, workspace))
    row(d, name, TAB)


def tab_names(d, workspace):
    return [t["name"] for t in workspace_named(d, workspace)["tabs"]]


def split_shown_tab(d, tab_name="a1"):
    """Splits the shown tab, so its panes get rows of their own and headers. Returns the two pane ids."""
    tab = next(t for w in d.workspaces() for t in w["tabs"] if t["name"] == tab_name)
    first = tab["panes"][0]["id"]
    second = d.cli("split", "--pane", first, "--right")
    settle(d, "the split", lambda s: any(len(p) == 2 for _, tabs in s for name, p in tabs if name == tab_name))
    return first, second
# }}}


# {{{ cases
def case_smoke(d):
    """The window comes up with its sidebar, and the daemon lists one workspace."""
    d.wait_item("sidebarTree")
    expect(len(d.workspaces()) == 1, "expected one workspace", d)
    expect_panes_draw(d)


def case_toggle_sidebar(d):
    """The sidebar's toggle hides it, and the toggle the band shows then brings it back."""
    d.wait_item("sidebarTree")
    d.click(Desktop.centre(d.wait_item("sidebarToggle", checked=True)))
    d.wait_gone("sidebarTree")
    d.click(Desktop.centre(d.wait_item("sidebarToggle", checked=False)))
    d.wait_item("sidebarTree")


def case_reorder_workspaces(d):
    """A workspace row dragged below another one moves after it."""
    two_workspaces(d)
    expect([w["name"] for w in d.workspaces()] == ["alpha", "beta"], "unexpected start order", d)
    drop_onto(d, Desktop.centre(row(d, "alpha", WORKSPACE)), band(row(d, "beta", WORKSPACE), "lower"),
              "sidebarRowDrop")
    settle(d, "alpha to move after beta", lambda s: [w for w, _ in s] == ["beta", "alpha"])


def case_tab_strip_to_workspace_row(d):
    """A tab dragged out of the tab strip onto another workspace's row joins that workspace as its last tab."""
    two_workspaces(d)
    new_tab(d, "alpha", "a2")
    tab = d.wait_item("tabItem", tabTitle="a2")
    drop_onto(d, Desktop.centre(tab), band(row(d, "beta", WORKSPACE), "middle"), "sidebarRowDrop")
    settle(d, "a2 to move into beta", lambda s: [t for t, _ in s[1][1]] == ["b1", "a2"]
           and [t for t, _ in s[0][1]] == ["a1"])
    expect_panes_draw(d)


def case_tab_row_to_workspace_row_then_show(d):
    """A tab row dropped on another workspace's row joins that workspace, and a click on its row there shows
    it: the window switches to that workspace and draws the tab it moved. The move took the tab out of what
    the window showed, so the window let its pane go, and the switch shows it again."""
    two_workspaces(d)
    new_tab(d, "alpha", "a2")
    drop_onto(d, Desktop.centre(row(d, "a2", TAB)), band(row(d, "beta", WORKSPACE), "middle"), "sidebarRowDrop")
    settle(d, "a2 to move into beta", lambda s: [t for t, _ in s[1][1]] == ["b1", "a2"])
    d.click(Desktop.centre(row(d, "a2", TAB)))
    deadline_loop(10, "beta to be shown", lambda: [w["name"] for w in d.workspaces()
                                                   if w.get("visibility") == "shown"] == ["beta"])
    row(d, "a2", TAB, selected=True)
    expect_panes_draw(d)


def case_pane_header_to_tab_row(d):
    """A pane's header dragged onto another tab's row joins that tab as a split."""
    two_workspaces(d)
    split_shown_tab(d)
    header = items_when(d, 2, "paneHeader")[-1]
    drop_onto(d, Desktop.centre(header, 0.3, 0.5), band(row(d, "b1", TAB), "middle"), "sidebarRowDrop")
    settle(d, "the pane to join b1", lambda s: len(dict(s[1][1])["b1"]) == 2 and len(dict(s[0][1])["a1"]) == 1)
    expect_panes_draw(d)


def case_pane_row_to_tab_row(d):
    """A pane's sidebar row dragged onto another tab's row joins that tab as a split."""
    two_workspaces(d)
    split_shown_tab(d)
    drop_onto(d, Desktop.centre(items_when(d, 2, "sidebarRow", kind=PANE)[-1]),
              band(row(d, "b1", TAB), "middle"), "sidebarRowDrop")
    settle(d, "the pane to join b1", lambda s: len(dict(s[1][1])["b1"]) == 2 and len(dict(s[0][1])["a1"]) == 1)
    expect_panes_draw(d)


def case_tab_row_to_empty_area(d):
    """A tab row dropped on the sidebar's empty area gets a workspace of its own."""
    two_workspaces(d)
    new_tab(d, "alpha", "a2")
    empty = d.wait_item("sidebarTreeEmptyDrop")
    drop_onto(d, Desktop.centre(row(d, "a2", TAB)), Desktop.centre(empty), "sidebarTreeEmptyDrop")
    settle(d, "a2 in a new workspace", lambda s: len(s) == 3 and [t for t, _ in s[2][1]] == ["a2"])
    expect_panes_draw(d)
    row(d, "a2", TAB, selected=True)


def case_workspace_row_on_terminal_refused(d):
    """A workspace row dropped on a terminal pane is refused: nothing moves, nothing opens, nothing closes."""
    two_workspaces(d)
    before = state(d)
    display = d.wait_item("terminalPane")
    d.drag(Desktop.centre(row(d, "beta", WORKSPACE)), Desktop.centre(display))
    # The OS drag ran and was over the window, so the refusal is the terminal's, not a drag that never began:
    # the pane's drop area turned it away, and the window's catch-all holds it so nothing tears off.
    with open(d.before_drop) as f:
        dump = json.load(f)
    expect(d.find("windowDropCatchAll", dump=dump)["props"].get("containsDrag"), "the drag never ran")
    expect(not any(a["props"].get("containsDrag") for a in d.items("paneDropArea", dump=dump)),
           "the terminal pane took a workspace drag")
    time.sleep(1.5)  # long enough for a wrongly taken drop to have reached the daemon
    expect(state(d) == before, "the refused drop changed something", d)
    expect(d.alive("client"), "the client ended", d)


def case_merge_workspace_into_tab(d):
    """A one-tab workspace dropped in the middle band of a tab row merges into that tab as a split."""
    two_workspaces(d)
    drop_onto(d, Desktop.centre(row(d, "beta", WORKSPACE)), band(row(d, "a1", TAB), "middle"),
              "sidebarRowDrop")
    settle(d, "beta to merge into a1", lambda s: len(s) == 1 and len(dict(s[0][1])["a1"]) == 2)
    expect_panes_draw(d)


def open_pane_menu(d):
    pane = items_when(d, 2, "sidebarRow", kind=PANE)[-1]
    d.click(Desktop.centre(pane), button=3)
    d.click(Desktop.centre(d.wait_item("", text="Close Pane…")))
    return d.wait_item("kitDialogAccept")


def case_close_pane_cancel_then_close(d):
    """A pane row's Close Pane… asks first: Cancel keeps the pane, Close ends it."""
    name_tab(d, d.workspaces()[0]["tabs"][0], "a1")
    split_shown_tab(d)
    open_pane_menu(d)
    d.click(Desktop.centre(d.wait_item("kitDialogReject")))
    d.wait_gone("kitDialogAccept")
    time.sleep(1)
    expect(len(state(d)[0][1][0][1]) == 2, "Cancel closed the pane", d)
    open_pane_menu(d)
    d.click(Desktop.centre(d.wait_item("kitDialogAccept")))
    settle(d, "the pane to close", lambda s: len(s[0][1][0][1]) == 1)


def case_collapse_expand(d):
    """A workspace's chevron folds its tabs away and unfolds them again."""
    two_workspaces(d)
    new_tab(d, "alpha", "a2")
    alpha = row(d, "alpha", WORKSPACE)
    expect(alpha["props"].get("expansion") == EXPANDED, f"alpha starts {alpha['props']}")
    chevron = deadline_loop(5, "alpha's disclosure", lambda: next(
        (c for c in d.items("sidebarRowDisclosure") if abs(Desktop.centre(c)[1] - Desktop.centre(alpha)[1]) < 4),
        None))
    d.click(Desktop.centre(chevron))
    d.wait_gone("sidebarRow", title="a2", kind=TAB)
    expect(row(d, "alpha", WORKSPACE)["props"].get("expansion") == COLLAPSED, "alpha did not fold")
    d.click(Desktop.centre(chevron))
    row(d, "a2", TAB)
    expect(row(d, "alpha", WORKSPACE)["props"].get("expansion") == EXPANDED, "alpha did not unfold")


def window_origin(d):
    window = d.geometry()["windows"][0]
    return window["x"], window["y"]


def case_drag_window_by_sidebar_top(d):
    """The sidebar's top area drags the window, as a title bar does."""
    # The sidebar's top is its own title row, or -- where the header bar carries the title -- the bar's part
    # above the sidebar. Either way its title text is an empty place of it, which the gestures fill.
    title = d.find("sidebarTitle", required=False) or d.wait_item("headerSidebarTitle")
    start = (int(title["x"] + 4), Desktop.centre(title)[1])
    before = window_origin(d)
    d.drag(start, (start[0] + 120, start[1] + 80), hold=0.1)
    deadline_loop(10, "the window to move", lambda: window_origin(d) != before)
    d.quiet()
    after = window_origin(d)
    # The window follows the pointer from where the drag set in, past the drag threshold, so it moves by a
    # little less than the pointer did -- never more, and never the other way.
    moved = (after[0] - before[0], after[1] - before[1])
    expect(90 <= moved[0] <= 122 and 50 <= moved[1] <= 82,
           f"the window moved by {moved}, not with the pointer's (120, 80)")


def case_keyboard_switch(d):
    """Ctrl+Shift+D focuses the sidebar; the arrows walk its rows and Enter shows the one reached."""
    two_workspaces(d)

    def shown():
        return [w["name"] for w in d.workspaces() if w.get("visibility") == "shown"]

    expect(shown() == ["alpha"], f"alpha should be shown, not {shown()}", d)
    d.click(Desktop.centre(d.wait_item("terminalPane")))
    d.key("ctrl+shift+d")
    deadline_loop(5, "a sidebar row to take the keyboard",
                  lambda: d.find("sidebarRow", required=False, keyboardFocused=True))
    # Down until the keyboard is on beta's row: past alpha's own rows, one at a time.
    for _ in range(6):
        d.quiet(still=0.3)
        if row(d, "beta", WORKSPACE)["props"].get("keyboardFocused"):
            break
        d.key("Down")
    expect(row(d, "beta", WORKSPACE)["props"].get("keyboardFocused"), "the arrows never reached beta's row")
    d.key("Return")
    deadline_loop(10, "beta to be shown", lambda: shown() == ["beta"])
    expect_panes_draw(d)


def case_in_process_tab_row_to_empty_area(d):
    """Without a daemon: a tab row dropped on the sidebar's empty area gets a workspace of its own. The drop is
    handled inside the OS drag's loop, which runs on the very rows the move rebuilds -- the move waits for the
    loop to return, or the window would destroy the row it is dragged from. An in-process window serves no
    control API, so its tabs are told apart by the titles their shells set (OSC 2), which its rows show as a
    daemon window's do."""
    d.click(Desktop.centre(d.wait_item("terminalPane")))
    title_shown_pane(d, "t1")
    row(d, "t1", TAB)
    d.key("ctrl+shift+t")
    items_when(d, 2, "sidebarRow", kind=TAB)
    title_shown_pane(d, "t2")
    moving = row(d, "t2", TAB)
    expect(len(items_when(d, 1, "sidebarRow", kind=WORKSPACE)) == 1, "expected one workspace")
    empty = d.wait_item("sidebarTreeEmptyDrop")
    drop_onto(d, Desktop.centre(moving), Desktop.centre(empty), "sidebarTreeEmptyDrop")
    workspaces = sorted(items_when(d, 2, "sidebarRow", kind=WORKSPACE), key=lambda item: item["y"])
    under_new = [t["props"].get("title") for t in d.items("sidebarRow", kind=TAB) if t["y"] > workspaces[1]["y"]]
    expect(under_new == ["t2"], f"the tab rows under the new workspace are {under_new}, not ['t2']")
    under_old = [t["props"].get("title") for t in d.items("sidebarRow", kind=TAB)
                 if workspaces[0]["y"] < t["y"] < workspaces[1]["y"]]
    expect(under_old == ["t1"], f"the tab rows under the first workspace are {under_old}, not ['t1']")
    expect(d.alive("client"), "the window ended")
    expect_panes_draw(d)


def case_tab_name_names_the_tab(d):
    """A tab's name, given through the control API, is what its sidebar row shows, over the title its shell
    set (OSC 2); clearing the name gives the row back that title."""
    tab = d.workspaces()[0]["tabs"][0]
    d.click(Desktop.centre(d.wait_item("terminalPane")))
    title_shown_pane(d, "shell-title")
    row(d, "shell-title", TAB)
    d.cli("rename", tab["id"], "build")
    row(d, "build", TAB)
    d.wait_gone("sidebarRow", title="shell-title", kind=TAB)
    d.cli("rename", tab["id"], "")
    row(d, "shell-title", TAB)
    d.wait_gone("sidebarRow", title="build", kind=TAB)


def case_gui_rename_names_the_tab(d):
    """A tab renamed through the window (its sidebar row's Rename…) gets the one name the CLI knows it by:
    `contour ls` shows it, `tab:<name>` addresses the tab, and `contour rename tab:<name> ""` clears it."""
    tab = d.workspaces()[0]["tabs"][0]
    expect(tab["name"] == "", f"the tab starts named {tab['name']!r}", d)
    d.click(Desktop.centre(items_when(d, 1, "sidebarRow", kind=TAB)[0]), button=3)
    d.click(Desktop.centre(d.wait_item("", text="Rename…")))
    d.wait_item("sidebarRenameField")
    d.type("deploy")
    d.key("Return")
    settle(d, "the tab to be named deploy", lambda s: [t for t, _ in s[0][1]] == ["deploy"])
    row(d, "deploy", TAB)
    listing = d.cli("ls")
    expect("deploy" in listing, f"`contour ls` does not show the name:\n{listing}", d)
    d.cli("rename", "tab:deploy", "")
    settle(d, "the name to be cleared", lambda s: [t for t, _ in s[0][1]] == [""])
    d.wait_gone("sidebarRow", title="deploy", kind=TAB)
    expect(d.workspaces()[0]["tabs"][0]["id"] == tab["id"], "the rename replaced the tab", d)


def case_ctrl_d_quits_clean(d):
    """Ctrl+D ends the window's only shell, and with it the window and the client, which exits 0. An
    AddressSanitizer build reports its leaks as the client exits, with the system libraries' left out
    (cmake/lsan-suppressions.txt): a leak of ours makes the exit status nonzero and fails the case."""
    d.wait_item("terminalPane")
    d.xdotool("windowactivate", "--sync", d.window_id())
    d.key("ctrl+d")
    client = next(proc for name, proc in d.procs if name == "client")
    deadline_loop(60, "the client to exit", lambda: client.poll() is not None, interval=0.25)
    log_text = d.read_log("client")
    # The tail only: a long report's first line may be cut off, its SUMMARY line never is.
    expect("LeakSanitizer" not in log_text and "leaked in" not in log_text,
           "the client reported leaks:\n" + log_text)
    expect(client.returncode == 0, f"the client exited {client.returncode}:\n" + log_text)


# {{{ several daemons
DAEMON = 3  # window::SidebarRowKind::Daemon
DONE, BELL = 2, 6  # window::TabBadgeKind
CONNECTED = 2  # window::DaemonLinkState::Connected
SELECTED, CONTAINS_SELECTED = 1, 2  # window::RowSelection
HOME_WORKSPACE, HOME_TAB = "Workspace 1", "Terminal"  # what the window's own daemon's first rows are called


def other_workspace(label):
    return f"{label}-ws"


def other_tab(label):
    return f"{label}-t"


def sections(d, dump=None):
    """[(daemon row title, [row, ...]), ...]: the sidebar's rows in screen order, under the daemon row heading
    each section."""
    rows = sorted(d.items("sidebarRow", dump=dump), key=lambda item: item["y"])
    found = []
    for item in rows:
        if item["props"].get("kind") == DAEMON:
            found.append((item["props"].get("title"), []))
        elif found:
            found[-1][1].append(item)
    return found


def window_on(d):
    """(daemon, title) of the row the window shows: which daemon the window is on, and what of it -- the tab, or
    the folded workspace holding it (window::RowSelection, Selected or ContainsSelected)."""
    for daemon, rows in sections(d):
        for item in rows:
            if item["props"].get("selection") in (SELECTED, CONTAINS_SELECTED):
                return daemon, item["props"].get("title")
    return None


def daemon_shown(d):
    """The daemon the window is on, by its row."""
    return (window_on(d) or (None, None))[0]


def shown_on(d, label):
    """The names of the workspaces the daemon `label` says a window shows."""
    return [w["name"] for w in d.workspaces(daemon=label) if w.get("visibility") == "shown"]


def expect_on(d, label, title, seconds=15):
    """Waits until the window shows the tab `title` of the daemon `label` -- its row, or its workspace's while
    that is folded -- and that daemon says it shows that workspace; then that every pane draws."""
    workspace = HOME_WORKSPACE if label == LABEL else other_workspace(label)
    try:
        deadline_loop(seconds, f"the window to show {title} of {label}",
                      lambda: window_on(d) in ((label, title), (label, workspace)), interval=0.1)
    except Failure as failure:
        rows = [(daemon, [(r["props"].get("title"), r["props"].get("kind"), r["props"].get("selection"))
                          for r in rows]) for daemon, rows in sections(d)]
        raise Failure(f"{failure}; it shows {window_on(d)}, the rows are {rows}") from failure
    if label != LABEL:
        deadline_loop(seconds, f"{label} to say it shows {workspace}", lambda: workspace in shown_on(d, label))
    expect_panes_draw(d)


def daemon_row(d, label, **props):
    return d.wait_item("sidebarRow", title=label, kind=DAEMON, **props)


def cancel_button(d, label):
    """The button on `label`'s daemon row while a jump into its section is under way: Cancel."""
    def found():
        heading = d.find("sidebarRow", required=False, title=label, kind=DAEMON)
        if not heading:
            return None
        return next((b for b in d.items("sidebarDaemonButton")
                     if heading["y"] <= Desktop.centre(b)[1] < heading["y"] + heading["height"]), None)
    return deadline_loop(10, f"Cancel on {label}'s row", found)


def other_pane(d, label):
    return d.workspaces(daemon=label)[0]["tabs"][0]["panes"][0]["id"]


def ring(d, label):
    """Rings the bell in the daemon `label`'s pane, out of the window's sight."""
    d.cli("send", "--pane", other_pane(d, label), "--enter", "printf '\\a'", daemon=label)


# A command as a shell with Contour's integration marks it (OSC 133): started, run, finished with status 0.
FINISHED_COMMAND = "printf '\\033]133;C\\007'; sleep 1; printf '\\033]133;D;0\\007'"


def jump_by_click(d, label):
    """Clicks `label`'s workspace row: a jump to that daemon."""
    d.click(Desktop.centre(row(d, other_workspace(label), WORKSPACE)))


def focus_terminal(d):
    if not d.find("terminalPane", required=False, activeFocus=True):
        d.click(Desktop.centre(d.wait_item("terminalPane")))
    d.wait_item("terminalPane", activeFocus=True)


def case_multi_daemon_three_listed(d):
    """The window lists its own daemon first, then the two other local daemons running, each heading a section
    with its workspace, all connected."""
    found = deadline_loop(20, "three daemon sections", lambda: len(sections(d)) == 3 and sections(d))
    expect([label for label, _ in found] == [LABEL, *OTHERS], f"the sections are {[l for l, _ in found]}")
    for label in OTHERS:
        daemon_row(d, label, linkState=CONNECTED)
        row(d, other_workspace(label), WORKSPACE)
    expect(window_on(d) == (LABEL, HOME_TAB), f"the window shows {window_on(d)}")


def case_multi_daemon_bell_on_other_row(d):
    """A bell rung on another daemon, which no window is on, shows on that daemon's row and its workspace's,
    and on no other daemon's."""
    for label in OTHERS:
        daemon_row(d, label, linkState=CONNECTED)
    ring(d, OTHERS[0])
    daemon_row(d, OTHERS[0], glyph=BELL)
    row(d, other_workspace(OTHERS[0]), WORKSPACE, glyph=BELL)
    expect(daemon_row(d, OTHERS[1])["props"].get("glyph") != BELL, "the bell shows on the other daemon too")


def case_multi_daemon_watched_command_stays_seen(d):
    """A command that finishes in the pane the window shows on another daemon is seen there: it marks neither
    its rows nor, once the window went back home, that daemon's row. The same command in a pane no window
    shows does mark its daemon's row Done -- which proves the command is one the sidebar sees finish."""
    jump_by_click(d, OTHERS[0])
    expect_on(d, OTHERS[0], other_tab(OTHERS[0]))
    focus_terminal(d)
    d.type(FINISHED_COMMAND + "\n")
    d.cli("send", "--pane", other_pane(d, OTHERS[1]), "--enter", FINISHED_COMMAND, daemon=OTHERS[1])
    daemon_row(d, OTHERS[1], glyph=DONE)
    time.sleep(1)
    for label, rows in sections(d):
        if label == OTHERS[0]:
            expect(all(r["props"].get("glyph") != DONE for r in rows), f"the watched command marked {label}'s rows")
    d.click(Desktop.centre(row(d, HOME_TAB, TAB)))
    expect_on(d, LABEL, HOME_TAB)
    time.sleep(1.5)  # long enough for a Done the daemon wrongly held to reach the row
    expect(daemon_row(d, OTHERS[0])["props"].get("glyph") != DONE, "the watched command turned unseen")
    expect(row(d, other_workspace(OTHERS[0]), WORKSPACE)["props"].get("glyph") != DONE,
           "the watched command's workspace turned unseen")


def case_multi_daemon_filter_return_jumps(d):
    """In the sidebar, typing another daemon's tab name filters the tree to it, and Return jumps there."""
    focus_terminal(d)
    d.key("ctrl+shift+d")
    deadline_loop(5, "a sidebar row to take the keyboard",
                  lambda: d.find("sidebarRow", required=False, keyboardFocused=True))
    d.type(other_tab(OTHERS[1]))
    row(d, other_tab(OTHERS[1]), TAB)
    d.key("Return")
    expect_on(d, OTHERS[1], other_tab(OTHERS[1]))


def case_multi_daemon_escape_in_terminal_during_jump(d):
    """While a jump to another daemon is under way, Escape typed in the terminal is the shell's: it reaches the
    program there and leaves the jump going, which lands once the daemon answers."""
    home_pane = d.workspaces()[0]["tabs"][0]["panes"][0]["id"]
    focus_terminal(d)
    d.type("cat -v\n")
    d.hold(OTHERS[0])
    jump_by_click(d, OTHERS[0])
    cancel_button(d, OTHERS[0])
    focus_terminal(d)
    d.key("Escape")
    d.type("esc-reached\n")
    deadline_loop(10, "the Escape to reach cat", lambda: "^[esc-reached" in d.cli("pane", "capture", "--pane",
                                                                                   home_pane))
    cancel_button(d, OTHERS[0])  # still under way
    d.release(OTHERS[0])
    expect_on(d, OTHERS[0], other_tab(OTHERS[0]))


def case_multi_daemon_second_click_wins(d):
    """A second row clicked while a jump to another daemon is under way wins: the window lands on the second
    row, and the first daemon, once it answers, is not shown."""
    d.hold(OTHERS[0])
    jump_by_click(d, OTHERS[0])
    cancel_button(d, OTHERS[0])
    jump_by_click(d, OTHERS[1])
    expect_on(d, OTHERS[1], other_tab(OTHERS[1]))
    d.release(OTHERS[0])
    time.sleep(2)  # long enough for the first jump, had it been kept, to land
    expect(daemon_shown(d) == OTHERS[1], f"the window went on to {window_on(d)}")
    expect(shown_on(d, OTHERS[0]) == [], f"{OTHERS[0]} shows {shown_on(d, OTHERS[0])}")


def case_multi_daemon_cancel_restores(d):
    """Cancel on the row of a daemon a jump is going to ends the jump: the window stays where it was, and the
    daemon, once it answers, is not shown."""
    d.hold(OTHERS[0])
    jump_by_click(d, OTHERS[0])
    d.click(Desktop.centre(cancel_button(d, OTHERS[0])))
    d.wait_gone("sidebarDaemonButton")
    d.release(OTHERS[0])
    time.sleep(2)
    expect(window_on(d) == (LABEL, HOME_TAB), f"the window shows {window_on(d)}")
    expect(shown_on(d, LABEL) != [], f"{LABEL} shows nothing")
    expect(shown_on(d, OTHERS[0]) == [], f"{OTHERS[0]} shows {shown_on(d, OTHERS[0])}")
    daemon_row(d, OTHERS[0], linkState=CONNECTED)
    expect_panes_draw(d)


def case_multi_daemon_open_in_new_window(d):
    """Open in New Window on another daemon's workspace row opens a second window on that daemon, and leaves
    the first where it was."""
    d.click(Desktop.centre(row(d, other_workspace(OTHERS[0]), WORKSPACE)), button=3)
    d.click(Desktop.centre(d.wait_item("", text="Open in New Window")))
    deadline_loop(30, "a second window", lambda: len(d.geometry()["windows"]) == 2, interval=0.25)
    deadline_loop(15, f"{OTHERS[0]} to show its workspace",
                  lambda: shown_on(d, OTHERS[0]) == [other_workspace(OTHERS[0])])
    expect(shown_on(d, LABEL) != [], f"the first window left {LABEL}")


def case_multi_daemon_jump_back_after_triage(d):
    """Next Needing Me twice walks the bells rung on two other daemons, in the tree's order; one Jump Back then
    returns to where the run started."""
    for label in OTHERS:
        ring(d, label)
        daemon_row(d, label, glyph=BELL)
    focus_terminal(d)
    d.key("ctrl+shift+j")
    expect_on(d, OTHERS[0], other_tab(OTHERS[0]))
    focus_terminal(d)
    d.key("ctrl+shift+j")
    expect_on(d, OTHERS[1], other_tab(OTHERS[1]))
    focus_terminal(d)
    d.key("ctrl+shift+alt+j")
    expect_on(d, LABEL, HOME_TAB)


def case_multi_daemon_cycle_daemon(d):
    """CycleDaemon walks the listed daemons in the tree's order, and wraps around to the window's own."""
    for label in OTHERS:
        daemon_row(d, label, linkState=CONNECTED)
    for label, title in ((OTHERS[0], other_tab(OTHERS[0])), (OTHERS[1], other_tab(OTHERS[1])), (LABEL, HOME_TAB)):
        focus_terminal(d)
        d.key("ctrl+shift+alt+Right")
        expect_on(d, label, title)


def focus_row(d, title, kind):
    """Moves the keyboard in the sidebar (focused already) onto the row `title`, by the arrows alone."""
    for _ in range(12):
        d.quiet(still=0.3)
        target = d.find("sidebarRow", required=False, title=title, kind=kind)
        focused = d.find("sidebarRow", required=False, keyboardFocused=True)
        if target and target["props"].get("keyboardFocused"):
            return
        d.key("Down" if not focused or not target or focused["y"] < target["y"] else "Up")
    raise Failure(f"the arrows never reached {title}'s row")


def focus_sidebar(d):
    focus_terminal(d)
    d.key("ctrl+shift+d")
    deadline_loop(5, "a sidebar row to take the keyboard",
                  lambda: d.find("sidebarRow", required=False, keyboardFocused=True))


def case_multi_daemon_keyboard_jump_cancel_and_back(d):
    """By the keyboard alone: Return on another daemon's workspace row jumps; Escape while it is under way
    cancels it and the window stays; Return again lands; Jump Back returns home."""
    for label in OTHERS:
        daemon_row(d, label, linkState=CONNECTED)
    focus_sidebar(d)
    focus_row(d, other_workspace(OTHERS[0]), WORKSPACE)
    d.hold(OTHERS[0])
    d.key("Return")
    cancel_button(d, OTHERS[0])
    d.key("Escape")
    d.wait_gone("sidebarDaemonButton")
    d.release(OTHERS[0])
    time.sleep(2)
    expect(window_on(d) == (LABEL, HOME_TAB), f"Escape did not keep the window home: {window_on(d)}")
    expect(shown_on(d, OTHERS[0]) == [], f"{OTHERS[0]} shows {shown_on(d, OTHERS[0])}")
    focus_sidebar(d)
    focus_row(d, other_workspace(OTHERS[0]), WORKSPACE)
    d.key("Return")
    expect_on(d, OTHERS[0], other_tab(OTHERS[0]))
    focus_terminal(d)
    d.key("ctrl+shift+alt+j")
    expect_on(d, LABEL, HOME_TAB)


def case_multi_daemon_keyboard_second_return_wins(d):
    """By the keyboard alone: Return on a second daemon's row while a jump to the first is under way wins."""
    for label in OTHERS:
        daemon_row(d, label, linkState=CONNECTED)
    focus_sidebar(d)
    focus_row(d, other_workspace(OTHERS[0]), WORKSPACE)
    d.hold(OTHERS[0])
    d.key("Return")
    cancel_button(d, OTHERS[0])
    focus_row(d, other_workspace(OTHERS[1]), WORKSPACE)
    d.key("Return")
    expect_on(d, OTHERS[1], other_tab(OTHERS[1]))
    d.release(OTHERS[0])
    time.sleep(2)
    expect(daemon_shown(d) == OTHERS[1], f"the window went on to {window_on(d)}")


def case_multi_daemon_keyboard_open_in_new_window(d):
    """By the keyboard alone: Shift+Return on another daemon's workspace row opens it in a second window."""
    daemon_row(d, OTHERS[0], linkState=CONNECTED)
    focus_sidebar(d)
    focus_row(d, other_workspace(OTHERS[0]), WORKSPACE)
    d.key("shift+Return")
    deadline_loop(30, "a second window", lambda: len(d.geometry()["windows"]) == 2, interval=0.25)
    deadline_loop(15, f"{OTHERS[0]} to show its workspace",
                  lambda: shown_on(d, OTHERS[0]) == [other_workspace(OTHERS[0])])


class XProbe:
    """One connection to the X server, held for a measurement: a click through XTest, timed as the server takes
    its release, and the colour of a few pixels read off the root window -- each a round trip of a millisecond
    or so, where an xdotool process and a whole-pane grab would cost tens of them."""

    def __init__(self, display):
        self.x11 = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
        self.xtst = ctypes.CDLL(ctypes.util.find_library("Xtst") or "libXtst.so.6")
        self.x11.XOpenDisplay.restype = ctypes.c_void_p
        self.x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x11.XDefaultRootWindow.restype = ctypes.c_ulong
        self.x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        self.x11.XGetImage.restype = ctypes.POINTER(XImage)
        self.x11.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint,
                                       ctypes.c_uint, ctypes.c_ulong, ctypes.c_int]
        self.x11.XDestroyImage.argtypes = [ctypes.POINTER(XImage)]
        self.x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        self.xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                                   ctypes.c_ulong]
        self.xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.display = self.x11.XOpenDisplay(display.encode())
        if not self.display:
            raise Failure(f"cannot open the X display {display}")
        self.root = self.x11.XDefaultRootWindow(self.display)

    def close(self):
        self.x11.XCloseDisplay(self.display)

    def click(self, point):
        """Moves the pointer to `point` and clicks there. @return When the server had taken the release."""
        self.xtst.XTestFakeMotionEvent(self.display, -1, point[0], point[1], 0)
        self.x11.XSync(self.display, 0)
        time.sleep(0.15)
        self.xtst.XTestFakeButtonEvent(self.display, 1, 1, 0)
        self.x11.XSync(self.display, 0)
        time.sleep(0.05)
        self.xtst.XTestFakeButtonEvent(self.display, 1, 0, 0)
        self.x11.XSync(self.display, 0)
        return time.monotonic()

    def colour(self, point, size=4):
        """The mean colour of the size x size pixels at `point`."""
        image = self.x11.XGetImage(self.display, self.root, point[0], point[1], size, size,
                                   ctypes.c_ulong(-1).value, 2)
        if not image:
            raise Failure(f"cannot read the screen at {point}")
        try:
            im = image.contents
            data = ctypes.string_at(im.data, im.bytes_per_line * im.height)
            pixels = [data[row * im.bytes_per_line + col * 4: row * im.bytes_per_line + col * 4 + 3]
                      for row in range(size) for col in range(size)]
            return tuple(sum(p[channel] for p in pixels) // len(pixels) for channel in (2, 1, 0))
        finally:
            self.x11.XDestroyImage(image)


# What each daemon's pane is painted with for the latency measurement (its whole screen, erased in it), so a
# landing is told from the pixels alone: the window shows the other daemon's pane once its colour is there.
LATENCY_COLOURS = {LABEL: (0x1E, 0xA0, 0x46), OTHERS[0]: (0xC8, 0x28, 0x28)}
# How many jumps the measurement times each way.
LATENCY_JUMPS = int(os.environ.get("CONTOUR_E2E_LATENCY_JUMPS", "20"))
# The goal a jump to an observed local daemon is held to (docs/superpowers/specs, the multi-daemon sidebar).
LATENCY_GOAL_MS = 100


def paint(d, label, pane):
    """Erases the pane's screen in its LATENCY_COLOURS colour, the colour staying for what is printed after."""
    r, g, b = LATENCY_COLOURS[label]
    d.cli("send", "--pane", pane, "--enter", f"printf '\\033[48;2;{r};{g};{b}m\\033[2J\\033[H'", daemon=label)


def percentile(samples, fraction):
    ordered = sorted(samples)
    return ordered[min(len(ordered) - 1, int(round(fraction * (len(ordered) - 1))))]


def case_multi_daemon_jump_latency(d):
    """Times the jump to an observed local daemon, both ways: a click on the other daemon's workspace row (and
    then on the window's own tab row) until the window draws that daemon's pane, read off the X server's
    pixels. Reports the median and 95th percentile against the 100 ms goal; fails only when a jump does not land
    within 5 s, since what a jump costs on Xvfb with software rendering in a debug build is no verdict on the
    goal. CONTOUR_E2E_LATENCY_FILE names a file the samples are appended to (direction, milliseconds)."""
    home_pane = d.workspaces()[0]["tabs"][0]["panes"][0]["id"]
    paint(d, LABEL, home_pane)
    paint(d, OTHERS[0], other_pane(d, OTHERS[0]))
    daemon_row(d, OTHERS[0], linkState=CONNECTED)
    probe = XProbe(d.display)
    samples = {"to-other": [], "to-own": []}
    try:
        pane = d.wait_item("terminalPane")
        spot = Desktop.centre(pane, 0.5, 0.7)
        deadline_loop(10, "the window's own pane painted", lambda: near(probe.colour(spot), LATENCY_COLOURS[LABEL], 12))
        for _ in range(LATENCY_JUMPS):
            for direction, label, target in (("to-other", OTHERS[0], (other_workspace(OTHERS[0]), WORKSPACE)),
                                              ("to-own", LABEL, (HOME_TAB, TAB))):
                d.quiet(still=0.3)
                clicked = probe.click(Desktop.centre(row(d, *target)))
                end = clicked + 5
                while not near(probe.colour(spot), LATENCY_COLOURS[label], 12):
                    if time.monotonic() > end:
                        raise Failure(f"a jump {direction} did not land within 5 s")
                    time.sleep(0.001)
                samples[direction].append((time.monotonic() - clicked) * 1000)
                time.sleep(0.3)
    finally:
        probe.close()
    record = os.environ.get("CONTOUR_E2E_LATENCY_FILE")
    for direction, values in samples.items():
        log(f"jump {direction}: {len(values)} jumps, median {percentile(values, 0.5):.1f} ms, "
            f"p95 {percentile(values, 0.95):.1f} ms, min {min(values):.1f} ms, max {max(values):.1f} ms "
            f"(goal {LATENCY_GOAL_MS} ms)")
        if record:
            with open(record, "a") as f:
                f.writelines(f"{direction}\t{value:.2f}\n" for value in values)
    # The pane draws in its painted colour, not the terminal's, which expect_on() looks for: the row alone.
    deadline_loop(5, "the window back on its own daemon", lambda: daemon_shown(d) == LABEL)


for _case in (case_multi_daemon_three_listed, case_multi_daemon_bell_on_other_row,
              case_multi_daemon_watched_command_stays_seen, case_multi_daemon_filter_return_jumps,
              case_multi_daemon_escape_in_terminal_during_jump, case_multi_daemon_second_click_wins,
              case_multi_daemon_cancel_restores, case_multi_daemon_open_in_new_window,
              case_multi_daemon_jump_back_after_triage, case_multi_daemon_cycle_daemon,
              case_multi_daemon_keyboard_jump_cancel_and_back, case_multi_daemon_keyboard_second_return_wins,
              case_multi_daemon_keyboard_open_in_new_window, case_multi_daemon_jump_latency):
    _case.others = OTHERS
# }}}



# The tab strip the drag starts from is on top only where the tabs are not in the sidebar.
case_tab_strip_to_workspace_row.tab_placement = "top"
case_in_process_tab_row_to_empty_area.in_process = True

CASES = {name[len("case_"):]: fn for name, fn in sorted(globals().items()) if name.startswith("case_")}
# }}}


def main(argv):
    if len(argv) >= 2 and argv[1] == "--list":
        print("\n".join(CASES))
        return 0
    if len(argv) != 3 or argv[2] not in CASES:
        print(f"usage: {argv[0]} /path/to/contour CASE   (cases: {', '.join(CASES)})", file=sys.stderr)
        return 64
    contour, case = argv[1], argv[2]
    if os.environ.get(OPT_IN) != "1":
        print(f"SKIP: set {OPT_IN}=1 to run the e2e-display workflows (they start Xvfb and a GUI client)",
              flush=True)
        return SKIP
    missing = [tool for tool in ("Xvfb", "xdotool", "xprop") if shutil.which(tool) is None]
    if missing:
        print(f"SKIP: {', '.join(missing)} not installed; the e2e-display workflows need a real X server and "
              "real input", flush=True)
        return SKIP
    desktop = Desktop(contour, tab_placement=getattr(CASES[case], "tab_placement", "sidebar"),
                      in_process=getattr(CASES[case], "in_process", False),
                      others=getattr(CASES[case], "others", ()))
    try:
        desktop.start()
        log(f"case {case}")
        CASES[case](desktop)
        log(f"case {case}: ok")
        return 0
    except Exception as error:  # noqa: BLE001 -- every failure is reported with the logs it needs
        print(f"FAIL {case}: {error}", file=sys.stderr)
        traceback.print_exc()
        for name in ("client", "daemon", "wm"):
            print(f"--- {name}.log (tail) ---\n{desktop.read_log(name)}", file=sys.stderr)
        return 1
    finally:
        desktop.stop()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
