// Window preferences and chrome state. Pure: no DOM access at import time.
export const PREF_KEYS = {
  theme: "contour-site.theme",
  maximized: "contour-site.maximized",
  singleKeys: "contour-site.single-keys",
};

/** Storage wrapper that never throws (blocked storage, private mode). */
export function safeStorage(backing) {
  return {
    get(k) {
      try { return backing?.getItem(k) ?? null; } catch { return null; }
    },
    set(k, v) {
      try { backing?.setItem(k, v); } catch { /* storage unavailable */ }
    },
  };
}

export function createPrefs(store) {
  return {
    theme() { const v = store.get(PREF_KEYS.theme); return v === "light" || v === "dark" ? v : null; },
    setTheme(t) { store.set(PREF_KEYS.theme, t); },
    setMaximized(b) { store.set(PREF_KEYS.maximized, b ? "1" : "0"); },
    singleKeys: () => store.get(PREF_KEYS.singleKeys) !== "0",
    setSingleKeys(b) { store.set(PREF_KEYS.singleKeys, b ? "1" : "0"); },
  };
}

export function createWindow(doc, prefs) {
  const html = doc.documentElement;
  const win = () => doc.getElementById("cw-window");
  const launcher = () => doc.getElementById("cw-launcher");
  const current = () =>
    html.getAttribute("data-theme") ??
    prefs.theme() ?? (doc.defaultView?.matchMedia?.("(prefers-color-scheme: dark)").matches ? "dark" : "light");
  return {
    toggleTheme() {
      const next = current() === "dark" ? "light" : "dark";
      html.setAttribute("data-theme", next);
      prefs.setTheme(next);
    },
    toggleMaximize() {
      prefs.setMaximized(html.toggleAttribute("data-max"));
    },
    // A closed window is inert, so nothing in it can take focus; focus follows to the launcher and back.
    close() {
      win()?.classList.add("closed");
      win()?.setAttribute("inert", "");
      launcher()?.classList.add("on");
      launcher()?.focus?.();
    },
    open() {
      win()?.classList.remove("closed");
      win()?.removeAttribute("inert");
      launcher()?.classList.remove("on");
      doc.getElementById("cw-page")?.focus?.({ preventScroll: true });
    },
  };
}
