#! /usr/bin/env python3
"""The sidebar's workflows, end to end, on a real X server: the contour_e2e_display_* tests (label e2e-display).

Each case starts its own Xvfb, a minimal window manager (minimal-wm.py, for the window moves and the focus a
frameless window asks of one), a private daemon and `contour client` on it, then drives the window with real X
pointer and keyboard events through xdotool -- so a drag runs QDrag's real drag loop, which no offscreen test
can -- and asserts what happened through the control API (`contour api call workspace.list`). What the window
draws is asserted off the X server's own pixels, in the one place the control API cannot see: that every terminal
pane a case ends showing draws its terminal (expect_panes_draw), told by colours only the terminal uses.

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

    def __init__(self, contour, tab_placement="sidebar", in_process=False):
        self.contour = contour
        # No daemon: the window's sessions live in the GUI process, which serves no control API, so a case
        # reads everything it asserts from the geometry dump.
        self.in_process = in_process
        self.procs = []  # (name, Popen), in start order
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

    # {{{ environment
    def env(self, gui=False):
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
        # The control verbs reach the private daemon through it. An in-process window is given none to find.
        if not self.in_process:
            env["CONTOUR_DAEMON"] = os.path.join(self.root, "run", "contour", LABEL)
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

    def start(self):
        self.start_bus()
        self.start_xvfb()
        self.spawn("wm", [sys.executable, "-I", os.path.join(HERE, "minimal-wm.py")], self.env(gui=True))
        deadline_loop(15, "the window manager", lambda: self.wm_ready())
        debug = ["--log", os.environ["CONTOUR_E2E_DEBUG"]] if os.environ.get("CONTOUR_E2E_DEBUG") else []
        if self.in_process:
            self.spawn("client", [self.contour, "terminal", *debug], self.env(gui=True))
        else:
            self.spawn("daemon", [self.contour, "daemon", "--label", LABEL], self.env())
            deadline_loop(30, "the daemon", lambda: self.cli_ok("ls"))
            self.spawn("client", [self.contour, "client", "--label", LABEL, *debug], self.env(gui=True))
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
    def run(self, argv, gui=False, check=True, timeout=30):
        result = subprocess.run(argv, env=self.env(gui=gui), capture_output=True, text=True, timeout=timeout,
                                stdin=subprocess.DEVNULL)
        if check and result.returncode != 0:
            raise Failure(f"{' '.join(argv)} exited {result.returncode}: {result.stderr.strip()}")
        return result

    def cli(self, *args, check=True):
        return self.run([self.contour, *args], check=check).stdout.strip()

    def cli_ok(self, *args):
        if not self.alive("daemon"):
            raise Failure("the daemon exited:\n" + self.read_log("daemon"))
        try:
            return self.run([self.contour, *args], check=False).returncode == 0
        except subprocess.TimeoutExpired:
            return False

    def api(self, method, request=None):
        out = self.run([self.contour, "api", "call", method, json.dumps(request or {})]).stdout
        return json.loads(out)

    def workspaces(self):
        return self.api("workspace.list")["workspaces"]

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
    """[(workspace label, [(tab label, [pane id, ...]), ...]), ...] in order."""
    return [(w["label"], [(t["label"], [p["id"] for p in t["panes"]]) for t in w["tabs"]])
            for w in d.workspaces()]


def expect(condition, message, d=None):
    if not condition:
        detail = json.dumps(state(d), indent=1) if d else ""
        raise Failure(f"{message}\nstate: {detail}")


def settle(d, what, predicate, seconds=15):
    """Waits for the daemon's state to satisfy predicate(state)."""
    return deadline_loop(seconds, what, lambda: predicate(state(d)), interval=0.2)


def workspace_named(d, label):
    return next(w for w in d.workspaces() if w["label"] == label)


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
    """Labels a tab through the control API, and waits for the window to show it: a tab's label names it in
    the sidebar and the tab strip."""
    d.cli("label", tab["id"], name)
    deadline_loop(15, f"the tab labelled {name!r}",
                  lambda: d.find("sidebarRow", required=False, title=name, kind=TAB)
                  or d.find("tabItem", required=False, tabTitle=name), interval=0.2)


def title_shown_pane(d, title):
    """Titles the shown pane's terminal (OSC 2) by typing a printf into its shell."""
    d.type(f"printf '\\033]2;{title}\\007'\n")


def two_workspaces(d):
    """alpha (shown, one tab `a1`) and beta (one tab `b1`), both labelled through the control API."""
    d.cli("workspace", "rename", "1", "alpha")
    name_tab(d, d.workspaces()[0]["tabs"][0], "a1")
    beta = d.cli("workspace", "new")
    d.cli("workspace", "rename", beta, "beta")
    name_tab(d, workspace_named(d, "beta")["tabs"][0], "b1")
    row(d, "alpha", WORKSPACE)
    row(d, "beta", WORKSPACE)


def new_tab(d, workspace, label):
    """Opens a tab in a workspace, labelled `label`, and waits for the sidebar to show it."""
    d.cli("new", "--workspace", workspace, "--name", label)
    settle(d, f"the tab {label}", lambda s: label in tab_labels(d, workspace))
    row(d, label, TAB)


def tab_labels(d, workspace):
    return [t["label"] for t in workspace_named(d, workspace)["tabs"]]


def split_shown_tab(d, label="a1"):
    """Splits the shown tab, so its panes get rows of their own and headers. Returns the two pane ids."""
    tab = next(t for w in d.workspaces() for t in w["tabs"] if t["label"] == label)
    first = tab["panes"][0]["id"]
    second = d.cli("split", "--pane", first, "--right")
    settle(d, "the split", lambda s: any(len(p) == 2 for _, tabs in s for name, p in tabs if name == label))
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
    expect([w["label"] for w in d.workspaces()] == ["alpha", "beta"], "unexpected start order", d)
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
    deadline_loop(10, "beta to be shown", lambda: [w["label"] for w in d.workspaces()
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
        return [w["label"] for w in d.workspaces() if w.get("visibility") == "shown"]

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


def case_tab_label_names_the_tab(d):
    """A tab's control-API label is what its sidebar row shows, over the title its shell set (OSC 2); clearing
    the label gives the row back that title."""
    tab = d.workspaces()[0]["tabs"][0]
    d.click(Desktop.centre(d.wait_item("terminalPane")))
    title_shown_pane(d, "shell-title")
    row(d, "shell-title", TAB)
    d.cli("label", tab["id"], "build")
    row(d, "build", TAB)
    d.wait_gone("sidebarRow", title="shell-title", kind=TAB)
    d.cli("label", tab["id"], "")
    row(d, "shell-title", TAB)
    d.wait_gone("sidebarRow", title="build", kind=TAB)


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
                      in_process=getattr(CASES[case], "in_process", False))
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
