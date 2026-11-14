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
| `remote_command` | Replaces the **whole** command run on the remote side of `ssh://` (default: `contour daemon --stdio`). Use it when `contour` is not on the remote non-interactive `PATH`. It reaches the remote shell verbatim, so quote a path with spaces inside the YAML string yourself (`"'/opt/my apps/contour' daemon --stdio"`). |
| `profile` | The profile the remote sessions are created with; empty selects the default one. |

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
