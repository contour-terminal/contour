// In-site navigation: fetch the next page and swap the window's panes. No DOM access at import time.
const SWAP = ["cw-strip", "cw-tabs", "cw-tabsel", "cw-page", "cw-aside", "cw-loc", "cw-title"];
const ASSET = /\.(?!html?$)[a-z0-9]{1,5}$/i;

/** Drops the hash. */
export const scrollKey = (url) => url.split("#")[0];

/**
 * A tabbed set (pymdownx.tabbed) shows one panel at a time; a link into a hidden one checks that panel's radio first.
 * Nested sets are opened from the innermost outward.
 * @returns {boolean} whether a radio was newly checked.
 */
export function revealOsPanel(doc, target) {
  let opened = false;
  // Walk outward: a nested set needs its own radio checked and every enclosing one too.
  for (let block = target?.closest?.(".tabbed-block"); block; block = block.parentElement?.closest?.(".tabbed-block")) {
    const set = block.closest?.(".tabbed-set");
    if (!set) continue;
    const at = [...block.parentElement.children].indexOf(block);
    const radio = [...set.querySelectorAll(":scope > input")][at];
    if (radio && !radio.checked) { radio.checked = true; opened = true; }
  }
  return opened;
}

/**
 * Scrolls to the element a `#hash` names, first revealing it when it sits in a hidden tab panel.
 * @param {string} hash With or without the leading `#`; percent-encoding is undone, malformed escapes kept.
 * @returns {boolean} whether the hash named an element.
 */
export function scrollToHash(doc, hash) {
  let id = hash.replace(/^#/, "");
  try { id = decodeURIComponent(id); } catch { /* keep the raw id */ }
  const target = id ? doc.getElementById(id) : null;
  if (!target) return false;
  revealOsPanel(doc, target);
  target.scrollIntoView();
  return true;
}

/** Decides who handles a link: "router", "hash" (same page anchor) or "browser". */
export function classifyLink(link, current, siteRoot) {
  if (link.target && link.target !== "_self") return "browser";
  if (link.download || link.ctrlKey || link.metaKey || link.shiftKey || link.altKey || (link.button ?? 0) !== 0)
    return "browser";
  let u, cur;
  try { u = new URL(link.href, current); cur = new URL(current); } catch { return "browser"; }
  if (!/^https?:$/.test(u.protocol) || !u.href.startsWith(siteRoot)) return "browser";
  if (u.hash && u.origin + u.pathname + u.search === cur.origin + cur.pathname + cur.search) return "hash";
  return ASSET.test(u.pathname.split("/").pop()) ? "browser" : "router";
}

/**
 * @param {object} o `doc`, `siteRoot` (absolute), and the injectable
 *   `fetchPage(url) -> {ok, text()}`, `parse(text) -> Document`, `history`, `location`, `onNavigated(url)`.
 *   Returns `navigate(url, mode)`, `follow(href)` (the click handler's choice of mode, with the fallback to a full
 *   load) and `start()`.
 */
export function createRouter(o) {
  const { doc, siteRoot, onNavigated = () => {} } = o;
  const history = o.history ?? globalThis.history;
  const location = o.location ?? globalThis.location;
  const fetchPage = o.fetchPage ?? ((url) => fetch(url, { headers: { Accept: "text/html" } }));
  const parse = o.parse ?? ((t) => new DOMParser().parseFromString(t, "text/html"));
  let ticket = 0;
  let shown = null; // page currently displayed, without hash
  const scrolls = new Map(); // scrollKey -> #page.scrollTop of the last visit

  async function navigate(url, mode) {
    const mine = ++ticket;
    const fail = () => { if (mine === ticket) (mode === "pop" ? location.reload() : location.assign(url)); };
    let next;
    try {
      const res = await fetchPage(url);
      if (!res.ok) return fail();
      next = parse(await res.text());
      if (res.redirected && res.url) url = res.url;
    } catch { return fail(); }
    if (mine !== ticket) return;
    if (!next?.getElementById("cw-page")) return fail(); // MkDocs redirect stub
    shown ??= scrollKey(location.href);
    const out = doc.getElementById("cw-page")?.scrollTop ?? 0;
    scrolls.set(shown, out);
    const key = scrollKey(new URL(url, location.href).href);
    const stored = scrolls.get(key) ?? history.state?.scroll ?? 0;
    if (mode !== "pop") history.replaceState({ ...history.state, scroll: out }, "");
    for (const id of SWAP) {
      const n = next.getElementById(id);
      if (n) doc.getElementById(id)?.replaceWith(n);
    }
    if (next.title) doc.title = next.title;
    if (mode === "push") history.pushState({ scroll: 0 }, "", url);
    else if (mode === "replace") history.replaceState({ scroll: 0 }, "", url);
    const page = doc.getElementById("cw-page");
    const found = scrollToHash(doc, new URL(url, location.href).hash);
    if (mode === "pop") page.scrollTop = stored;
    else if (!found) page.scrollTop = 0;
    page.focus?.({ preventScroll: true });
    shown = key;
    onNavigated(url);
  }

  /** Same page: replace the entry; else push. A failed navigation falls back to a full load. */
  function follow(href) {
    const same = scrollKey(href) === scrollKey(location.href);
    return navigate(href, same ? "replace" : "push").catch(() => location.assign(href));
  }

  function start() {
    history.scrollRestoration = "manual";
    shown = scrollKey(location.href);
    doc.addEventListener("click", (e) => {
      const a = e.target?.closest?.("a[href]");
      if (!a || e.defaultPrevented || typeof a.href !== "string") return;
      const link = { href: a.href, target: a.target, download: a.hasAttribute("download"), button: e.button,
        ctrlKey: e.ctrlKey, metaKey: e.metaKey, shiftKey: e.shiftKey, altKey: e.altKey };
      if (classifyLink(link, location.href, siteRoot) !== "router") return;
      e.preventDefault();
      follow(a.href);
    });
    doc.defaultView?.addEventListener("popstate", () => {
      if (scrollKey(location.href) !== shown) navigate(location.href, "pop").catch(() => location.reload());
      else ticket++; // drop any in-flight click response
    });
  }

  return { navigate, follow, start };
}
