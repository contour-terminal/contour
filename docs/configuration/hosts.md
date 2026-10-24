# Hosts

A saved host is a remote machine whose daemon Contour can attach to by name: "Attach to Host"
lists them, and `contour client --host NAME` attaches to one. How remote attach works, and how
to set the far side up (including over Tailscale), is described in
[Remote hosts](../remote-hosts.md).

## Configuration

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
| `token_file` | The file holding the remote daemon's token. Required for `tls://`, unused for `ssh://`. |
| `tls_ca` | A CA certificate to verify the daemon's TLS certificate against (`tls://`). Without it the connection is encrypted but the daemon's certificate is not checked. |
| `remote_command` | Replaces the **whole** command run on the remote side of `ssh://` (default: `contour daemon --stdio`). Use it when `contour` is not on the remote non-interactive `PATH`. It reaches the remote shell verbatim, so quote a path with spaces inside the YAML string yourself (`"'/opt/my apps/contour' daemon --stdio"`). |
| `profile` | The profile the remote sessions are created with; empty selects the default one. |

Paths in `token_file` and `tls_ca` expand `~` and `${VAR}`, as every path in the configuration does.

## Key binding

The `AttachToHost` action opens a new window attached to the saved host its `name` names, as the
"Attach to Host" entries do; a binding without `name` is skipped.

``` yaml
input_mapping:
  - { mods: [Control, Shift], key: H, action: AttachToHost, name: home }
```

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

A daemon listening with `contour daemon --listen-tcp` always requires a token, so a `tls://` host
must name its `token_file`; a `tls://` host without one is skipped with a warning, and an ad-hoc
`tls://` URL (one not saved under `hosts:`) is refused. The port is required, and a `tls://` URL
takes no user: the token authenticates.

An entry that cannot be used — no `url`, a malformed one, a `tls://` host without `token_file` — is
reported in the log and skipped; the other hosts still load.
