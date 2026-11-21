// Wires the modules to the page: keys, palette, router, window chrome and page enhancements.
import { KEY_TABLE, statusHints, resolveKey, PENDING_TIMEOUT_MS, isTypingTarget } from "./keys.js";
import { safeStorage, createPrefs, createWindow } from "./window.js";
import { pageEntries, actionEntries, indexEntries, createPalette, escapeHtml } from "./palette.js";
import { readModel, allPages } from "./model.js";
import { createRouter, scrollKey, scrollToHash } from "./router.js";

const doc = document;
const $ = (id) => doc.getElementById(id);
const LAST_KEY = "contour-site.last";
const TYPED_KEY = "contour-site.typed";
const ABOUT = "contour --about";
const INDEX_PATH = "search/search_index.json";
const NOTICE_NO_INDEX = "full-text search unavailable";

// Reading the property itself throws a SecurityError when site data is blocked.
const store = (name) => { try { return globalThis[name]; } catch { return null; } };
const prefs = createPrefs(safeStorage(store("localStorage")));
const session = safeStorage(store("sessionStorage"));
const win = createWindow(doc, prefs);
const siteRoot = new URL(doc.querySelector('meta[name="site-root"]')?.content ?? ".", location.href).href;
const reducedMotion = () => globalThis.matchMedia?.("(prefers-reduced-motion: reduce)").matches ?? false;
const resolve = (href) => new URL(href, location.href).href;

// ---- last tab per workspace, for the visit
const last = (() => { try { return JSON.parse(session.get(LAST_KEY) ?? "{}") ?? {}; } catch { return {}; } })();
const tabsOf = (model, name) => (model.workspaces.find((w) => w.name === name)?.tabs ?? []).map((t) => resolve(t.href));
const lastTab = (model, name) => (tabsOf(model, name).includes(last[name]) ? last[name] : null);
function remember() {
  const m = readModel(doc);
  if (m.current.workspace && m.current.href) {
    last[m.current.workspace] = resolve(m.current.href);
    session.set(LAST_KEY, JSON.stringify(last));
  }
}

// ---- navigation
const router = createRouter({ doc, siteRoot, onNavigated: () => { remember(); fadeIn(); enhance(); } });
function go(href) {
  const u = new URL(href, location.href);
  if (scrollKey(u.href) === scrollKey(location.href) && !u.hash) return;
  router.follow(u.href);
}
function goWorkspace(index) {
  const model = readModel(doc);
  const w = model.workspaces[index];
  if (w) go(lastTab(model, w.name) ?? w.href);
}
function stepTab(d) {
  const m = readModel(doc);
  const tabs = tabsOf(m, m.current.workspace);
  if (tabs.length === 0) return;
  const at = tabs.indexOf(resolve(m.current.href));
  go(tabs[at < 0 ? (d > 0 ? 0 : tabs.length - 1) : (at + d + tabs.length) % tabs.length]);
}

// ---- palette
const actions = () => [
  { label: "theme: toggle light/dark", action: "theme" },
  { label: "window: toggle maximize", action: "maximize" },
  { label: "help: show keys", action: "help" },
  { label: `keys: single-key shortcuts \u2014 ${prefs.singleKeys() ? "on" : "off"}`, action: "single-keys" },
];
const pagesNow = () => allPages(readModel(doc)).map((p) => ({ ...p, href: resolve(p.href) }));
const palette = createPalette({
  overlay: $("cw-palette"), input: $("cw-palette-input"), list: $("cw-palette-list"), notice: $("cw-palette-notice"),
  onPick(e) {
    if (e.kind === "action") run(e.action);
    else go(e.kind === "page" ? e.href : new URL(e.href, siteRoot).href);
  },
  onClose: syncMode,
});
// The index rows are built once, when the index arrives; pages and actions follow the page and the preferences.
let indexRows = [];
let indexState = "idle";
const refreshEntries = () => palette.setEntries([...pageEntries(pagesNow()), ...actionEntries(actions()), ...indexRows]);
async function loadIndex() {
  indexState = "loading";
  try {
    const res = await fetch(new URL(INDEX_PATH, siteRoot));
    if (!res.ok) throw new Error(String(res.status));
    indexRows = indexEntries(await res.json());
    indexState = "done";
    refreshEntries();
  } catch {
    palette.setNotice(NOTICE_NO_INDEX);
    indexState = "failed";
  }
}
function openPalette() {
  if (palette.isOpen()) return;
  refreshEntries();
  show("palette");
  if (indexState === "idle") loadIndex();
}

// The pending chord prefix (`g` of `g t`).
let pending = "";
let pendingTimer = 0;
/** Sets the pending prefix; a non-empty one expires after PENDING_TIMEOUT_MS without a follow-up key. */
function setPending(value) {
  clearTimeout(pendingTimer);
  pending = value;
  if (value) pendingTimer = setTimeout(clearPending, PENDING_TIMEOUT_MS);
}
const clearPending = () => setPending("");

// ---- overlays: at most one is shown; the mode text follows
const mode = $("cw-mode");
const help = $("cw-help");
function syncMode() {
  const text = palette.isOpen() ? "COMMAND" : help.classList.contains("on") ? "HELP" : "NORMAL";
  mode.textContent = text;
  mode.classList.toggle("cmd", text !== "NORMAL");
}
/** Shows "palette", "help" or, with null, neither. */
function show(name) {
  clearPending();
  if (name !== "palette") palette.close();
  help.classList.toggle("on", name === "help");
  if (name === "palette") palette.open();
  syncMode();
}
// ---- help and status-line hints, both read from the key table
const kbd = (k) => `<kbd>${escapeHtml(k)}</kbd>`;
(() => {
  const byAction = new Map();
  for (const r of KEY_TABLE) {
    const g = byAction.get(r.action) ?? { chords: [], label: r.label };
    g.chords.push(r.chord);
    byAction.set(r.action, g);
  }
  $("cw-help-list").innerHTML = [...byAction.values()]
    .map((g) => `<div><span><kbd>${g.chords.map(escapeHtml).join("</kbd> <kbd>")}</kbd></span><span>${escapeHtml(g.label)}</span></div>`)
    .join("");
  $("cw-hints").innerHTML = statusHints()
    .map((h) => `<span class="${h.fallback ? "hint-off" : "hint"}">${h.keys.map(kbd).join(h.sep)} ${escapeHtml(h.text)}</span>`)
    .join("");
})();
help.addEventListener("click", () => show(null));

// ---- actions: one table from name to behaviour, shared by keys and the palette
const scroller = (f) => () => { const p = $("cw-page"); p?.scrollBy({ top: f(p), behavior: "auto" }); };
const BIG = 1e7;
// The WCAG 2.1.4 escape hatch must show its state: a status flash, and the single-key hints go away.
const flash = $("cw-flash");
let flashTimer = 0;
function syncKeyHints() { doc.documentElement.toggleAttribute("data-nokeys", !prefs.singleKeys()); }
function toggleSingleKeys() {
  prefs.setSingleKeys(!prefs.singleKeys());
  syncKeyHints();
  flash.textContent = `single-key shortcuts ${prefs.singleKeys() ? "on" : "off"}`;
  clearTimeout(flashTimer);
  flashTimer = setTimeout(() => { flash.textContent = ""; }, 2500);
}
syncKeyHints();
const ACTION_TABLE = {
  "workspace-1": () => goWorkspace(0),
  "workspace-2": () => goWorkspace(1),
  "workspace-3": () => goWorkspace(2),
  "tab-next": () => stepTab(1),
  "tab-prev": () => stepTab(-1),
  "scroll-down": scroller(() => 70),
  "scroll-up": scroller(() => -70),
  "half-down": scroller((p) => p.clientHeight / 2),
  "half-up": scroller((p) => -p.clientHeight / 2),
  top: scroller(() => -BIG),
  bottom: scroller(() => BIG),
  palette: openPalette,
  help: () => show(help.classList.contains("on") ? null : "help"),
  theme: () => win.toggleTheme(),
  maximize: () => win.toggleMaximize(),
  "single-keys": toggleSingleKeys,
  close: () => { show(null); $("cw-aside")?.classList.remove("open"); syncOutlineToggle(); },
};
function run(name) { ACTION_TABLE[name]?.(); }

// ---- keys
const OVERLAY_ACTIONS = new Set(["help", "close", "palette"]); // these manage the overlays themselves
doc.addEventListener("keydown", (ev) => {
  if (ev.defaultPrevented || $("cw-window").classList.contains("closed")) return;
  const inPalette = doc.activeElement === $("cw-palette-input");
  const singleKeys = prefs.singleKeys() && !inPalette && !isTypingTarget(doc.activeElement);
  const r = resolveKey(pending, ev, { singleKeys });
  if (r.pending !== pending) setPending(r.pending);
  if (!r.action) return;
  if (inPalette && r.action !== "palette") return; // the palette owns its own keys
  ev.preventDefault();
  if (!OVERLAY_ACTIONS.has(r.action)) show(null);
  run(r.action);
});

// A lone `g` is also dropped when focus moves elsewhere.
doc.defaultView.addEventListener("blur", clearPending);
doc.addEventListener("focusin", clearPending);

// ---- window chrome
const closeWindow = () => { run("close"); win.close(); };
$("cw-wc-close").addEventListener("click", closeWindow);
$("cw-wc-min").addEventListener("click", closeWindow);
$("cw-wc-max").addEventListener("click", () => win.toggleMaximize());
$("cw-launcher").addEventListener("click", () => win.open());
$("cw-titlebar").addEventListener("dblclick", (e) => { if (!e.target.closest("button")) win.toggleMaximize(); });
$("cw-theme").addEventListener("click", () => win.toggleTheme());

const outlineToggle = $("cw-outline-toggle");
function syncOutlineToggle() { outlineToggle.setAttribute("aria-expanded", String($("cw-aside")?.classList.contains("open") ?? false)); }
outlineToggle.addEventListener("click", () => { $("cw-aside")?.classList.toggle("open"); syncOutlineToggle(); });

// ---- clicks that need the model: search button, outline links, strip -> last tab
doc.addEventListener("click", (e) => {
  const t = e.target;
  if (t.closest?.("#cw-search")) { openPalette(); return; }
  if (t.closest?.("#cw-aside a")) { $("cw-aside").classList.remove("open"); syncOutlineToggle(); }
  const ws = t.closest?.("#cw-strip a.ws");
  if (ws && !e.defaultPrevented && e.button === 0 && !(e.ctrlKey || e.metaKey || e.shiftKey || e.altKey)) {
    const m = readModel(doc);
    const to = lastTab(m, ws.dataset.ws);
    if (to) { e.preventDefault(); go(to); }
  }
});

// ---- clock
const clock = $("cw-clock");
const MINUTE = 60000;
function tick() {
  const now = new Date();
  clock.textContent = now.toTimeString().slice(0, 5);
  setTimeout(tick, MINUTE - (now.getSeconds() * 1000 + now.getMilliseconds()));
}
tick();

// ---- page enhancements, re-run after every navigation
// A short fade for the swapped page; the reduced-motion rule in the stylesheet switches it off.
function fadeIn() {
  const page = $("cw-page");
  if (!page) return;
  page.classList.remove("reveal");
  void page.offsetWidth;
  page.classList.add("reveal");
}

let typedOnce = session.get(TYPED_KEY) === "1";
function typeHome() {
  const typed = $("cw-typed");
  if (!typed) return;
  const finish = () => { typed.textContent = ABOUT; $("cw-typed-cursor")?.remove(); };
  if (typedOnce || reducedMotion()) { finish(); return; }
  typedOnce = true;
  session.set(TYPED_KEY, "1");
  typed.textContent = "";
  let i = 0;
  const step = () => {
    if (!typed.isConnected) return;
    typed.textContent = ABOUT.slice(0, ++i);
    if (i < ABOUT.length) setTimeout(step, 45 + Math.random() * 50);
    else setTimeout(() => $("cw-typed-cursor")?.remove(), 900);
  };
  setTimeout(step, 350);
}

function addCopyButtons(page) {
  for (const pre of page.querySelectorAll("pre")) {
    if (pre.querySelector(":scope > button")) continue;
    const b = doc.createElement("button");
    b.type = "button";
    b.textContent = "copy";
    b.setAttribute("aria-label", "Copy code");
    b.addEventListener("click", async () => {
      const clone = pre.cloneNode(true);
      clone.querySelector(":scope > button")?.remove();
      const text = clone.textContent.replace(/\n$/, "");
      try { await navigator.clipboard.writeText(text); b.textContent = "copied"; } catch { b.textContent = "failed"; }
      setTimeout(() => { b.textContent = "copy"; }, 1200);
    });
    pre.appendChild(b);
  }
}

// Outline links and their headings are resolved once per page; scrolling only schedules one update per frame.
function spy(page) {
  const links = [...doc.querySelectorAll("#cw-aside a[data-head]")];
  if (links.length === 0) return;
  const heads = links.map((a) => doc.getElementById(a.dataset.head));
  let queued = false;
  const update = () => {
    queued = false;
    const top = page.getBoundingClientRect().top;
    const atEnd = page.scrollTop + page.clientHeight >= page.scrollHeight - 2 && page.scrollTop > 0;
    let on = atEnd ? links.at(-1) : links[0];
    if (!atEnd) {
      for (const [i, a] of links.entries()) if (heads[i] && heads[i].getBoundingClientRect().top - top < 120) on = a;
    }
    for (const a of links) a.classList.toggle("on", a === on);
  };
  page.addEventListener("scroll", () => { if (!queued) { queued = true; requestAnimationFrame(update); } }, { passive: true });
  update();
}

// Keyboard scrolling needs the page focused; a deep link keeps the browser's own focus point.
let firstEnhance = true;
function focusPage(page) {
  const wanted = location.hash.length > 1;
  if (firstEnhance && !wanted && (!doc.activeElement || doc.activeElement === doc.body)) page.focus({ preventScroll: true });
  firstEnhance = false;
}

// A hash into a hidden install panel: show the panel, then scroll (the browser could not).
globalThis.addEventListener("hashchange", () => scrollToHash(doc, location.hash));

function enhance() {
  const page = $("cw-page");
  if (!page) return;
  focusPage(page);
  addCopyButtons(page);
  typeHome();
  spy(page);
  syncOutlineToggle();
}

remember();
scrollToHash(doc, location.hash); // the router handles the hash of every later navigation
enhance();
router.start();
