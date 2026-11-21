// Key table and resolver. Pure: no DOM access at import time.
/**
 * `hint`, where present, puts the row in the status line: `{order, text, keys?, sep?, fallback?}`. `keys` overrides the
 * chord's own keys (joined by `sep`); `fallback` marks the hint shown only while single-key shortcuts are off.
 */
const row = (chord, action, label, single = true, hint = null) => ({ chord, action, label, single, hint });

export const KEY_TABLE = [
  row("1", "workspace-1", "workspace 1", true, { order: 3, text: "workspace", keys: ["1", "3"], sep: "–" }),
  row("2", "workspace-2", "workspace 2"),
  row("3", "workspace-3", "workspace 3"),
  row("g t", "tab-next", "next tab", true, { order: 2, text: "next tab" }),
  row("g T", "tab-prev", "previous tab"),
  row("j", "scroll-down", "scroll down"),
  row("k", "scroll-up", "scroll up"),
  row("d", "half-down", "half page down"),
  row("u", "half-up", "half page up"),
  row("g g", "top", "top"),
  row("G", "bottom", "bottom"),
  row(":", "palette", "command palette", true, { order: 1, text: "palette" }),
  row("/", "palette", "command palette"),
  row("Ctrl+k", "palette", "command palette", false, { order: 0, text: "palette", fallback: true }),
  row("?", "help", "this help", true, { order: 0, text: "keys" }),
  row("t", "theme", "toggle theme"),
  row("m", "maximize", "maximize"),
  row("Escape", "close", "close overlay", false),
];

/** The status line's hints, in display order: `{keys, sep, text, fallback}`. */
export function statusHints(table = KEY_TABLE) {
  return table
    .filter((r) => r.hint)
    .map((r) => ({ keys: r.hint.keys ?? r.chord.split(/[ +]/), sep: r.hint.sep ?? "", text: r.hint.text, fallback: !!r.hint.fallback, order: r.hint.order }))
    .sort((a, b) => Number(b.fallback) - Number(a.fallback) || a.order - b.order);
}

const BY_CHORD = new Map(KEY_TABLE.map((r) => [r.chord, r]));
const NONE = (pending = "") => ({ action: null, pending });

/** Keys that only change a modifier state; they never end or start a chord. */
const MODIFIER_KEYS = new Set(["Shift", "Control", "Alt", "Meta", "CapsLock", "AltGraph"]);

/** How long a pending chord prefix waits for its follow-up key, in milliseconds. */
export const PENDING_TIMEOUT_MS = 1000;

/** Resolve a key event against the pending prefix. */
export function resolveKey(pending, ev, opts) {
  const { key } = ev;
  if (MODIFIER_KEYS.has(key)) return NONE(pending);
  if (ev.altKey) return NONE();
  if (ev.ctrlKey || ev.metaKey) return key.toLowerCase() === "k" ? { action: "palette", pending: "" } : NONE();
  if (key === "Escape") return { action: "close", pending: "" };
  if (!opts.singleKeys) return NONE();
  if (pending) {
    const hit = BY_CHORD.get(`${pending} ${key}`);
    return hit ? { action: hit.action, pending: "" } : NONE();
  }
  if (key === "g") return NONE("g");
  const hit = BY_CHORD.get(key);
  return hit && hit.single ? { action: hit.action, pending: "" } : NONE();
}

const NON_TEXT = new Set(["checkbox", "radio", "button", "submit", "reset"]);

/** True when the target takes typed text, so shortcuts must stay out of the way. */
export function isTypingTarget(t) {
  if (!t) return false;
  if (t.isContentEditable) return true;
  if (t.tagName === "INPUT") return !NON_TEXT.has(t.type);
  return t.tagName === "TEXTAREA" || t.tagName === "SELECT";
}
