# Hosts

A saved host is a remote machine whose daemon Contour can attach to by name: "Attach to Host"
lists them, and `contour client --host NAME` attaches to one. How remote attach works, and how
to set the far side up (including over Tailscale), is described in
[Remote hosts](../remote-hosts.md).

## Configuration

A profile's `ssh:` block instead opens a remote login shell through Contour's built-in SSH client; it
does not require Contour on the server. Contour chooses the first nonempty value from the profile's
`environment.TERM`, the top-level `ssh_default_term`, and the Contour process's `TERM` environment
variable. If none is set, Windows uses `xterm-256color`. Other platforms send an empty terminal type.
Choose a terminal type whose terminfo entry is installed on the server.

In Settings, edit **Default SSH terminal type** under **General → Advanced** and **Environment
variables** under the profile's **Shell** group. Settings can edit profiles it manages. For a
profile defined in `contour.yml`, edit the file or duplicate the profile in Settings.

```yaml
ssh_default_term: xterm-256color
profiles:
  remote-shell:
    ssh:
      host: homepc
      user: me
    environment:
      TERM: contour
      COLORTERM: truecolor
```

The profile's `environment:` values go to the shell the profile opens. A local shell receives them
directly. For built-in SSH, `TERM` travels in the PTY request. Other entries, such as `COLORTERM`,
are sent as SSH environment requests and may require server permission (for example, OpenSSH's
`AcceptEnv`). Review values in existing profiles before using them with a remote host.

### Connect timeout

`ssh.connect_timeout` limits, in seconds, how long a built-in SSH connection may take to come up: from
the TCP connect through the handshake, authentication and the terminfo installation below to the
started shell. Time spent waiting for you (confirming a new host key, typing a key's passphrase or a
password) does not count. It defaults to `30`; `0` means no limit, and more than `86400` (24 hours)
is taken as `86400`. A connection that runs out of time ends with an error naming the step it was in,
the host and `ssh.connect_timeout`. Once the shell runs, no limit applies. Resolving the host name is
left to the system resolver and its own timeouts.

Authenticating through an SSH agent does not count toward `ssh.connect_timeout` either: an agent may
ask you to approve each use of a key (1Password, Secretive, `ssh-add -c`, a FIDO key waiting for a
touch, gpg-agent asking for its PIN), and Contour cannot tell that wait apart from the host's. The
agent step has its own allowance of 2 minutes (or `connect_timeout`, if that is longer), for all of
the agent's keys together; when it runs out, the connection ends with an error saying the agent did
not finish in time. On Linux, macOS and other Unix systems this also cuts off an agent that never
answers, or an approval left pending. On Windows it does not: there the allowance takes effect only
once the agent has answered, so an agent that never replies, or an approval left pending, is not cut
off. With `connect_timeout: 0` the agent step has no limit either.

```yaml
profiles:
  remote-shell:
    ssh:
      host: homepc
      connect_timeout: 10
```

### Terminfo installation

When a built-in SSH profile connects, Contour first runs a one-off command on a second SSH channel
that compiles Contour's terminfo entry with the remote `tic` and installs it into the remote user's
`~/.terminfo` (`c/contour`, or `63/contour` where the remote's `tic` uses hex-named directories, as
on macOS). The entry stays there; every later connect reinstalls it, which also updates it after a
Contour upgrade. When the install succeeds, the shell starts with `TERM=contour`.

If the remote has no `tic` (for example Alpine without ncurses, or BusyBox), the home directory is
not writable, or anything else fails, the connection proceeds with the terminal type chosen above;
the details go to the SSH log only. The step is bounded by 3 seconds, and never by more than what
is left of `ssh.connect_timeout`. When it times out and the connection is still usable, one line is
shown in the terminal and the shell starts. When the install
leaves the connection unable to open the shell's channel (the host stopped answering while the install
channel was being opened, or while a request was still being sent), the connection ends with an error
that names `ssh.install_terminfo: false`. A host whose install failed, timed out or ended the
connection is skipped for the rest of that Contour run, so connecting again works.

A profile `environment.TERM` set to anything other than `contour` skips the install. To turn it off
for a profile, set `install_terminfo: false` (default `true`); the error message names it as
`ssh.install_terminfo: false`:

```yaml
profiles:
  remote-shell:
    ssh:
      host: homepc
      install_terminfo: false
```

Two kinds of hosts need care:

- Hosts that allow only one session per connection, or that drop the connection after a command, can
  fail to open the shell after the attempt. The error message then says to set
  `ssh.install_terminfo: false`.
- Hosts with a forced command (`ForceCommand`, or `command=` in `authorized_keys`) run that command
  once more on the first connect of a Contour run instead of the install; the install then fails and
  the host is skipped from then on. If that forced command prompts (for example for MFA), turn the
  option off for that profile.

The installed file is a regular terminfo entry; removing `~/.terminfo/*/contour` on the remote undoes it.

### Saved hosts

Hosts are configured under the top-level `hosts` entry, a map of host name to its settings.
Listings are sorted by name.

``` yaml
hosts:
  home:
    url: "ssh://me@homepc"
    remote_command: "/opt/contour/bin/contour daemon --stdio"
    profile: "remote"
  studio:
    url: "tls://studio.tailnet.ts.net:7297"
    token_file: "~/.config/contour/studio.token"
    tls_ca: "~/.config/contour/studio-ca.pem"
```

| Key | Meaning |
| --- | --- |
| `url` | Where the host is: `ssh://[user@]host[:port]` or `tls://host:port`. Required. |
| `token_file` | The file holding the remote daemon's token (`tls://`). Not needed when an SSH key signs in. |
| `tls_ca` | A CA certificate to verify the daemon's TLS certificate against (`tls://`). Without it the daemon is pinned on first use, as ssh does for a host: its certificate's fingerprint is kept in `known_daemons`, and a daemon that presents another one is refused. |
| `identity_file` | The SSH private key that signs in to a `tls://` daemon, instead of the keys of your SSH agent. |
| `remote_command` | Replaces the **whole** command run on the remote side of `ssh://` (default: `contour daemon --stdio`). Use it when `contour` is not on the remote non-interactive `PATH`. It reaches the remote shell verbatim, so quote a path with spaces inside the YAML string yourself (`"'/opt/my apps/contour' daemon --stdio"`). The workspace sidebar, which watches a listed host in the background and never starts its daemon, runs `contour daemon --stdio --no-start` by default, and your `remote_command` followed by ` --no-start`: a custom command must therefore end in `contour daemon --stdio`, or otherwise take the relay's flags after it (a wrapper script passes on `"$@"`). |
| `profile` | The profile the remote sessions are created with; empty selects the default one. |
| `show_in_sidebar` | Lists the host's daemon in the workspace sidebar, connected or not (default: `false`). Contour then logs in to the host in the background, without a window asking, whenever the sidebar looks again: in batch mode, so it never asks for a password or an unknown host key itself, and it never starts the daemon there. Your SSH agent may still ask (a pinentry, 1Password, a FIDO key's touch) when a key it holds is used. "Hide from Sidebar" and "Show in Sidebar" override it until you change this switch in Settings. |

Paths in `token_file`, `tls_ca` and `identity_file` expand `~` and `${VAR}`, as every path in the configuration does.

## Key binding

The `AttachToHost` action opens a new window attached to the saved host its `name` names, as the
"Attach to Host" entries do; a binding without `name` is skipped.

``` yaml
input_mapping:
  - { mods: [Control, Shift], key: H, action: AttachToHost, name: home }
```

A host is attached like a launch against its daemon: one window, which shows the first workspace of the daemon
no window shows (else a new one) and switches between the daemon's other workspaces in place. `contour client --host
NAME --workspace W` shows the workspace `W` first. A window that is already open can move to a saved host
with the `SwitchToDaemon` and `OpenDaemon` actions, which take `host: NAME`; see
[Key Mapping](key-mapping.md#switching-daemons-and-workspaces).

## `ssh://`

``` yaml
hosts:
  home:
    url: "ssh://me@homepc:2222"
```

Contour runs `ssh <target> contour daemon --stdio`; ssh does the authentication, with keys or an
agent only (no password or host-key prompt is answered from the GUI). The user and the port are
optional; ssh's own configuration (`~/.ssh/config`) applies as usual. A host or user that begins
with `-` is refused.

## `tls://`

``` yaml
hosts:
  studio:
    url: "tls://studio.tailnet.ts.net:7297"
    token_file: "~/.config/contour/studio.token"
```

A `tls://` host needs nothing beyond its `url`: it signs in with the keys of your SSH agent, or with its
`identity_file` (best loaded into the agent with `ssh-add` when it has a passphrase, as the daemon waits only 20
seconds for the sign-in), when the daemon authorizes keys, and with its `token_file` otherwise. The first time a
daemon without a `tls_ca` is met, a dialog (a question on the terminal, for the control commands) shows its
fingerprint to compare with what `contour daemon status` prints on that machine, and asks whether to trust
it; where nobody can be asked, it is refused unless the fingerprint is given (`--daemon-fingerprint`). A trusted
daemon is pinned, and one that later presents another certificate is refused, never asked about. After a daemon was reinstalled,
`contour daemon forget-pin HOST:PORT` forgets the old pin. The port is required, and a `tls://` URL
takes no user.

An entry that cannot be used — no `url`, or a malformed one — is
reported in the log and skipped; the other hosts still load.
