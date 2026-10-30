# Remote hosts

A Contour window can show the sessions of a daemon running on **another machine**: your home PC from
the office, a build server, a workstation under your desk. The sessions keep running there; the window
here is a client of that daemon, exactly as it is of a local one (see
[Persistent sessions](persistent-sessions.md)).

There are two ways to reach a remote daemon:

| Transport | What it needs | Authentication |
| --- | --- | --- |
| **ssh** (`ssh://`) | An OpenSSH client here, Contour on the remote machine. Nothing listens on the network besides sshd. | ssh's own: keys or an agent. |
| **TLS** (`tls://`) | A daemon on the remote machine started with `--listen-tcp`. | A token, over TLS. |

If you can already `ssh` to the machine, use ssh: it needs no open port, no token and no certificate.

## Saved hosts

Hosts you attach to often are saved in the configuration under `hosts:`, by name
([reference](configuration/hosts.md)):

``` yaml
hosts:
  home:
    url: "ssh://me@homepc"
  studio:
    url: "tls://studio.tailnet.ts.net:7297"
    token_file: "~/.config/contour/studio.token"
    tls_ca: "~/.config/contour/studio-ca.pem"
```

Hosts can also be added in **Settings › Hosts**, without editing the file: each one is written to
`hosts/<name>.yml` beside `contour.yml`, with the same keys as an entry of `hosts:`. Its page shows the SSH
or TLS fields its address calls for, says under a field what would keep the host from loading, and saves
nothing until it is fixed; **Attach** opens a window attached to it. Hosts written in `contour.yml` are
listed there too, read-only, with **Duplicate to Edit**.

Then attach in any of these ways:

- **"Attach to Host"** in the command palette lists the saved hosts; pick one.
- The same list is in the terminal's **context menu** and the **title-bar menu**.
- From a shell:

  ```sh
  contour client --host home                     # a saved host, by name
  contour client --host ssh://me@homepc:2222     # an ssh URL that is not saved
  ```

  An ad-hoc `tls://` URL is refused: a TLS host needs its `token_file`, so save it first.

A window attached to a remote host says so in its chrome, so it is never mistaken for a local one: its
title reads `home — <session title>`, and the tab strip starts with the host's name.

## Choosing where a window lives: the Connect picker

The **Connect picker** is one question before a window opens: where do its sessions live? It lists

- **This computer**: *Persistent sessions*, the local daemon, with its state (`running · 3 sessions`, or
  `not running · starts on connect`), and *This window only*, with no daemon: its shells end when the window
  closes (`daemon.mode: in_process`);
- **Saved hosts**: each host of [`hosts:`](configuration/hosts.md), tagged SSH or TLS;
- and, while you type an `ssh://` or `tls://` address in the field, a leading **Connect to ADDRESS** row for
  it. The address is not saved.

Open it from **File › New Window to…** (<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>K</kbd>, <kbd>⇧⌘K</kbd> on
macOS; the `NewWindowTo` action), from `contour connect` with no target, from the launchers below, or at
every plain start, with **Ask every time Contour starts** (`connect_picker_on_start`, also in Settings ›
General). A launch with command-line arguments never shows it.

The field has the focus. Type to filter by name or address; <kbd>↑</kbd> and <kbd>↓</kbd> move, <kbd>Return</kbd>
connects and <kbd>Esc</kbd> cancels, which quits when no other window is open. Every row shows a key,
<kbd>Alt</kbd>+<kbd>1</kbd> to <kbd>9</kbd> (<kbd>⌘1</kbd> to <kbd>⌘9</kbd> on macOS), that connects to it at
once; plain digits stay typable, for addresses and ports. Filtering renumbers the rows, so `star` then
<kbd>Alt</kbd>+<kbd>1</kbd> reaches a host named `starfive`. <kbd>Ctrl</kbd>+<kbd>L</kbd> (<kbd>⌘L</kbd>)
puts the caret back in the field. The selection starts on the target you used last, which Contour remembers
in `gui-state.yml`, not in `contour.yml`.

A host that cannot be reached stays in the picker: the reason is shown under its row, in the words Attach to
Host uses (an unknown host key, an authentication failure, Contour missing on the host, an unreachable
host), with **Retry** and **Edit Host…**, which opens Settings › Hosts on it. **Manage Hosts…** opens
Settings › Hosts.

### `contour connect`

``` sh
contour connect                     # the picker
contour connect daemon              # this computer's daemon; daemon:LABEL for another one
contour connect local               # this window only, no daemon
contour connect starfive            # a saved host, by name
contour connect ssh://me@pi4.lan    # an ssh:// or tls:// address
contour connect --list [--json]     # the targets the picker would show
```

A target opens the window as `contour client` does (`--label`, `--host`), or `contour terminal --daemon-mode
in_process` for `local`; no target shows the picker. `--config FILE` names the configuration that holds the
hosts. `--list` prints each target, as `contour connect` reads it, with its detail, or a JSON array with
`--json`. An ad-hoc `tls://` address is refused, as for `contour client --host`: a TLS host needs its token
file, so save it first.

### From the launchers

- **Linux**: the desktop entry has a **Connect…** action (`contour connect`) beside *Open a New Window*, and
  `org.contourterminal.Contour.Connect.desktop` is a launcher of its own, "Contour — Connect…", for menus that
  show no actions.
- **macOS**: the Dock menu has **New Window to…** and each saved host.
- **Windows**: the jump list has the task **Connect…** and one per saved host, and the installer adds a
  **Contour (Connect…)** Start menu shortcut.

## Over SSH

Contour runs, for every attach and every reconnect:

```sh
ssh -T -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=3 \
    [-p PORT] -- [USER@]HOST contour daemon --stdio
```

On the remote side, `contour daemon --stdio` reaches your daemon there — starting it if none runs, the
same way a local `contour client` does — and relays the session between it and ssh.

### Requirements

- **An OpenSSH client** on this machine (`ssh` on `PATH`). On Windows, the built-in OpenSSH client
  ("OpenSSH Client" optional feature) works.
- **Contour on the remote machine**, findable by a *non-interactive* login — which often has a shorter
  `PATH` than your interactive shell. Check with:

  ```sh
  ssh me@homepc contour version
  ```

  If that says `command not found`, give the full path in the saved host:

  ``` yaml
  hosts:
    home:
      url: "ssh://me@homepc"
      remote_command: "/opt/contour/bin/contour daemon --stdio"
  ```

- **Key or agent authentication.** Contour never answers a prompt: there is no terminal to type a
  password, a key passphrase, a second factor or a host-key confirmation into. Load your key into the
  agent (`ssh-add`) or use a key without a passphrase.
- **The host key accepted once.** The first connection to a new host asks whether to trust its key.
  Run this once in a terminal and answer `yes`, then attach from Contour:

  ```sh
  ssh me@homepc
  ```

Your `~/.ssh/config` applies as usual: host aliases, `User`, `Port`, `ProxyJump` and `IdentityFile` all
work, so `url: "ssh://homepc"` can rely on what that file says about `homepc`.

### When it does not connect

Contour reads what ssh reports and says what to do. The messages, and the fix for each:

| Message (abridged) | Cause | Fix |
| --- | --- | --- |
| `ssh could not log in to ssh://me@homepc without a prompt … Run this once in a terminal, then attach again: ssh me@homepc` | The host key is not known yet, or no key/agent identity was accepted. | Run the command shown in a terminal. Accept the host key; if it asks for a password, set up a key (`ssh-copy-id me@homepc`) or load yours into the agent (`ssh-add`). |
| `The host key of ssh://me@homepc has changed …` | The remote machine's key differs from the one in `~/.ssh/known_hosts`: a reinstalled machine, or someone in between. | If you expected it, remove the old key with `ssh-keygen -R homepc` and run `ssh me@homepc` once. If you did not, do not connect. |
| `ssh could not reach ssh://me@homepc: …` | The name does not resolve, nothing answers on the port, or there is no route. ssh's own words follow. | Check the name, the port and the network (VPN, Tailscale). Contour retries this on its own while a window waits. |
| `Contour was not found on ssh://me@homepc by a non-interactive ssh login …` | `contour` is not installed there, or not on the non-interactive `PATH`. | Install it, or set `remote_command:` to its full path (see above). |
| `Contour on ssh://me@homepc is too old for ssh attach …` | The remote Contour has no `contour daemon --stdio`. | Upgrade Contour on the remote machine. |
| `ssh://me@homepc: daemon did not start within 5 seconds.` (or another reason) | The remote relay ran, but could not reach or start the daemon there. | Run `ssh me@homepc contour daemon --stdio` in a terminal and read what it says; check the daemon's log on that machine. |
| `ssh://me@homepc printed more than 64 KiB before Contour answered …` | A login script on the remote machine prints output on non-interactive logins. | Make `.bashrc` / profile scripts quiet when not interactive (e.g. wrap output in `[[ $- == *i* ]] && …`). A short banner is skipped; only a long one fails. |
| `ssh to ssh://me@homepc ended before Contour answered (exit code N): …` | ssh ended for a reason Contour does not recognize. | Run `ssh me@homepc contour daemon --stdio` in a terminal to see the whole output. Contour retries this one too. |
| `ssh://me@homepc did not answer within 30 seconds` | The connection hangs (a firewall that drops packets, a stuck login). | As for "could not reach". |
| `ssh was not found on PATH; install the OpenSSH client` | No ssh client on this machine. | Install OpenSSH. |

A connection that drops mid-session — the laptop sleeps, the Wi-Fi changes — is reconnected with a
fresh ssh, while the window shows that it is reconnecting. A failure that retrying cannot fix (a refused
login, a host key, a missing Contour) ends the attempt with its message instead of repeating it.

## Over TLS

The daemon on the remote machine listens on a TCP port itself; every connection is TLS-encrypted and
must present a token.

On the remote machine:

```sh
umask 077 && openssl rand -base64 32 > ~/.config/contour/daemon.token
contour daemon --background --listen-tcp 192.0.2.10:7297 --token-file ~/.config/contour/daemon.token
```

Copy the token file to the machine you sit at (for example with `scp`), keeping it private
(`chmod 600`), and save the host:

``` yaml
hosts:
  studio:
    url: "tls://studio.example.net:7297"
    token_file: "~/.config/contour/studio.token"
```

### Unverified TLS, and a pinned CA

Without `tls_ca`, the daemon presents a certificate it generated itself and **the client does not check
it**. The connection is encrypted, which stops anyone merely listening, but **an active
man-in-the-middle can pose as the daemon and take the token**, and with it your sessions. Use this only
on a network you trust, such as a tailnet (below).

To have the client verify the daemon, give the daemon a certificate issued by a CA of your own, for the
name you connect to, and pin that CA on the client:

```sh
# Once, anywhere: a CA of your own.
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 3650 \
    -subj "/CN=My Contour CA" -keyout ca.key -out ca.pem

# For the remote machine: a certificate for the name the client connects to.
openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
    -subj "/CN=studio.example.net" -keyout daemon.key -out daemon.csr
printf 'subjectAltName=DNS:studio.example.net\n' > daemon.ext
openssl x509 -req -in daemon.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 825 \
    -extfile daemon.ext -out daemon.pem
```

Start the daemon with `--tls-cert daemon.pem --tls-key daemon.key`, copy `ca.pem` (never `ca.key`) to
the client, and add `tls_ca: "~/.config/contour/studio-ca.pem"` to the saved host. The certificate must
name the host in the `url`: one merely signed by the CA, for another name, is refused.

Contour's control commands — the `contour` verbs that change a daemon's state from a script — need
`tls_ca`: they refuse a TLS host whose certificate is not verified.

## Reaching a home PC from the office with Tailscale

[Tailscale](https://tailscale.com) puts your machines on a private network (a *tailnet*) wherever they
are, without opening ports on your router. Contour does not bundle Tailscale or talk to it; it simply
connects to an address on the tailnet like any other.

1. **Install Tailscale on both machines** from <https://tailscale.com/download> and log in to the same
   tailnet on each.
2. **Find the home PC's tailnet address** — on the home PC:

   ```sh
   tailscale ip -4          # e.g. 100.101.102.103
   ```

   With MagicDNS on (the default), the machine is also reachable as
   `<machine>.<tailnet>.ts.net`, e.g. `homepc.tail1234.ts.net`; the admin console shows the exact name.

3. **The simplest way is ssh over the tailnet.** If the home PC runs sshd, nothing else is needed:

   ``` yaml
   hosts:
     home:
       url: "ssh://me@homepc.tail1234.ts.net"
   ```

4. **Or listen with TLS on the tailnet address only** — on the home PC:

   ```sh
   umask 077 && openssl rand -base64 32 > ~/.config/contour/daemon.token
   contour daemon --background --listen-tcp 100.101.102.103:7297 \
       --token-file ~/.config/contour/daemon.token
   ```

   Binding to the `100.x.y.z` address, not to `0.0.0.0`, keeps the port off every other network the PC
   is on. `contour daemon-service install` does not record `--listen-tcp`, so start this listener
   yourself (or from your login scripts). Then, at the office, with the token file copied over:

   ``` yaml
   hosts:
     home:
       url: "tls://homepc.tail1234.ts.net:7297"
       token_file: "~/.config/contour/home.token"
   ```

   Tailscale already encrypts and authenticates the traffic between your machines, which is what makes
   unverified TLS a reasonable choice here. For the control commands, or for defense in depth, add
   `tls_ca` as [above](#unverified-tls-and-a-pinned-ca), with the certificate issued for
   `homepc.tail1234.ts.net`.

5. **Attach**: "Attach to Host" → `home`, or `contour client --host home`.

## Foreign paths

The sessions of a remote host run on that machine, so the directories and files they report — the
current directory a shell announces (OSC 7), a command block's working directory, a link to a file —
are **that machine's** paths. A window attached to a remote host shows them, but never opens them here:
it does not open such a file, start a program in such a directory, or offer "open folder" for it, since
on this machine the path names nothing, or something else.
