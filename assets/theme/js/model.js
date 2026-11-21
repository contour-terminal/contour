// Page model read from the rendered window shell (every workspace is in the DOM).
const attr = (n, k) => n.getAttribute(k) ?? "";

/** Reads workspaces, their tabs and the current location from `doc`. */
export function readModel(doc) {
  const strip = [...doc.querySelectorAll("#cw-strip a.ws[data-ws]")];
  const lists = new Map([...doc.querySelectorAll("#cw-tabs div.tablist[data-ws]")].map((l) => [l.dataset.ws, l]));
  let current = { workspace: null, href: "" };
  const workspaces = strip.map((a) => {
    const name = a.dataset.ws;
    const tabs = [...(lists.get(name)?.querySelectorAll("a.tab[href]") ?? [])].map((t) => {
      const href = attr(t, "href");
      if (t.hasAttribute("aria-current")) current = { workspace: name, href };
      return { title: t.textContent.trim(), href, group: t.dataset.group ?? "" };
    });
    if (a.hasAttribute("aria-current") && current.workspace === null) current = { workspace: name, href: "" };
    return { name, href: attr(a, "href"), tabs };
  });
  return { workspaces, current };
}

/** Flat page list for the palette. */
export function allPages(model) {
  return model.workspaces.flatMap((w) => w.tabs.map((t) => ({ title: t.title, href: t.href, workspace: w.name })));
}
