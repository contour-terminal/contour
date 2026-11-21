// Command palette: pure entry building/ranking, plus a DOM controller built on demand.
const OPTION_ID = "cw-palette-opt-";
const KIND_ORDER = { page: 0, action: 1, heading: 2, text: 3 };

const hayOf = (...parts) => parts.join(" ").toLowerCase();

/** One row per page: `{label, kind: "page", workspace, href, hay}`. */
export function pageEntries(pages) {
  return pages.map((p) => ({ label: p.title, kind: "page", workspace: p.workspace, href: p.href, hay: hayOf(p.title, p.workspace ?? "") }));
}

/** One row per action: `{label, kind: "action", action, hay}`. */
export function actionEntries(actions) {
  return actions.map((a) => ({ label: a.label, kind: "action", action: a.action, hay: hayOf(a.label) }));
}

/** Heading and text rows from an MkDocs search index: `{label, kind: "heading" | "text", href, hay}`. */
export function indexEntries(searchIndex) {
  const docs = searchIndex?.docs ?? [];
  const titles = new Map();
  for (const d of docs) if (!d.location.includes("#")) titles.set(d.location, d.title);
  const entries = [];
  for (const d of docs) {
    const hash = d.location.indexOf("#");
    if (hash < 0) {
      if (d.text) entries.push({ label: d.title, kind: "text", href: d.location, hay: hayOf(d.title, d.text) });
    } else {
      const page = titles.get(d.location.slice(0, hash)) ?? "";
      const label = page ? `${page} › ${d.title}` : d.title;
      entries.push({ label, kind: "heading", href: d.location, hay: hayOf(label) });
    }
  }
  return entries;
}

/** Build palette entries from pages, an optional MkDocs search index, and actions. */
export const buildEntries = (pages, searchIndex, actions) =>
  [...pageEntries(pages), ...actionEntries(actions), ...indexEntries(searchIndex)];

/** Filter and order entries for a query; words match literally, case-insensitively. */
export function rankEntries(entries, query, limit = 40) {
  const words = query.toLowerCase().split(/\s+/).filter(Boolean);
  const wordStart = (s, w) => s.startsWith(w) || s.includes(` ${w}`);
  const scored = [];
  for (const [i, e] of entries.entries()) {
    const kind = KIND_ORDER[e.kind] ?? 9;
    if (words.length === 0 && kind > KIND_ORDER.action) continue;
    if (!words.every((w) => e.hay.includes(w))) continue;
    const label = e.label.toLowerCase();
    const key = words.length === 0
      ? [kind === KIND_ORDER.action ? -1 : kind, 0, 0, 0] // nothing typed: the actions lead, then the pages
      : [kind, label.startsWith(words[0]) ? 0 : 1, words.every((w) => wordStart(label, w)) ? 0 : 1, e.label.length];
    scored.push({ e, i, key });
  }
  const cmp = (a, b) => {
    for (const [n, k] of a.key.entries()) if (k !== b.key[n]) return k - b.key[n];
    return a.i - b.i;
  };
  scored.sort(cmp);
  // The limit trims pages, never the actions an empty query leads with.
  const actions = words.length === 0 ? scored.filter((s) => s.e.kind === "action").length : 0;
  return scored.slice(0, Math.max(limit, actions)).map((s) => s.e);
}

/** Escape text for safe interpolation into HTML. */
export function escapeHtml(s) {
  const map = { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" };
  return String(s).replace(/[&<>"']/g, (c) => map[c]);
}

/**
 * Wire the palette overlay; all DOM access happens here.
 * Keyboard contract: handled keys call stopPropagation, and main.js must not resolve page keys
 * while the palette input has focus, except Ctrl+K, which may call open() again; open() is a
 * no-op when already open, so the typed query survives. `onClose` runs whenever an open palette closes, however it was closed.
 */
export function createPalette({ overlay, input, list, notice, onPick, onClose = () => {} }) {
  let entries = [];
  let shown = [];
  let sel = 0;
  const isOpen = () => overlay.classList.contains("on");
  const render = () => {
    shown = rankEntries(entries, input.value);
    sel = Math.min(sel, Math.max(shown.length - 1, 0));
    list.innerHTML = shown
      .map((e, i) => `<div class="it${i === sel ? " on" : ""}" role="option" id="${OPTION_ID}${i}" aria-selected="${i === sel}" data-i="${i}"><span${e.kind === "action" ? ' class="act"' : ""}>${escapeHtml(e.label)}</span><small>${escapeHtml(e.workspace ?? e.kind)}</small></div>`)
      .join("");
    list.querySelector(".on")?.scrollIntoView?.({ block: "nearest" });
    input.setAttribute?.("aria-expanded", String(shown.length > 0));
    if (shown.length > 0) input.setAttribute?.("aria-activedescendant", `${OPTION_ID}${sel}`);
    else input.removeAttribute?.("aria-activedescendant");
  };
  let prevFocus = null;
  const close = () => {
    if (!isOpen()) return;
    overlay.classList.remove("on");
    input.setAttribute?.("aria-expanded", "false");
    input.removeAttribute?.("aria-activedescendant");
    prevFocus?.focus?.();
    prevFocus = null;
    onClose();
  };
  const open = () => {
    if (isOpen()) return;
    prevFocus = overlay.ownerDocument?.activeElement ?? null;
    overlay.classList.add("on");
    input.value = "";
    sel = 0;
    render();
    input.focus();
  };
  const move = (d) => {
    if (shown.length === 0) return;
    sel = (sel + d + shown.length) % shown.length;
    render();
  };
  const pick = (i) => {
    const e = shown[i];
    if (!e) return;
    close();
    onPick(e);
  };
  input.addEventListener("input", () => { sel = 0; render(); });
  input.addEventListener("keydown", (ev) => {
    const step = { ArrowDown: 1, ArrowUp: -1 }[ev.key] ?? (ev.ctrlKey ? { n: 1, p: -1 }[ev.key.toLowerCase()] : undefined);
    if (step !== undefined) move(step);
    else if (ev.key === "Enter") { if (!ev.isComposing) pick(sel); }
    else if (ev.key === "Escape") close();
    else return;
    ev.preventDefault();
    ev.stopPropagation();
  });
  list.addEventListener("click", (ev) => {
    const row = ev.target.closest?.(".it");
    if (row) pick(Number(row.dataset.i));
  });
  overlay.addEventListener("click", (ev) => { if (ev.target === overlay) close(); });
  return {
    open,
    close,
    isOpen,
    setEntries(e) { entries = e; if (isOpen()) render(); },
    setNotice(text) { notice.textContent = text; },
  };
}
