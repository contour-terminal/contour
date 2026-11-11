# SSH test fixtures

Throwaway keys and signatures for `SshKeys_test.cpp`, made once with the system `ssh-keygen`
(OpenSSH_10.2p1, OpenSSL 3.5.9). The keys are unencrypted and exist only for these tests; the
private halves were never checked in.

```sh
printf 'contour-auth fixture message\n' > message.txt
ssh-keygen -q -t ed25519 -N '' -C 'ed25519-fixture' -f ed25519
ssh-keygen -q -t ecdsa -b 256 -N '' -C 'ecdsa-p256-fixture' -f ecdsa-p256
ssh-keygen -q -t ed25519 -N '' -C 'ed25519-other' -f ed25519-other
for k in ed25519 ecdsa-p256 ed25519-other; do
    ssh-keygen -Y sign -f $k -n contour-auth < message.txt > $k.sig
    ssh-keygen -lf $k.pub > $k.fingerprint
done
ssh-keygen -Y sign -f ed25519 -n contour-auth -O hashalg=sha256 < message.txt > ed25519-sha256.sig
```

Checked in: `message.txt`, `ed25519.pub`, `ed25519.fingerprint`, `ed25519.sig`,
`ed25519-sha256.sig` (a genuine signature the verifier must refuse, since only sha512 is accepted), `ecdsa-p256.pub`, `ecdsa-p256.fingerprint`, `ecdsa-p256.sig` and
`ed25519-other.sig` (the signature by another key, for `KeyMismatch`).

## Why there are no `sk-` fixtures

`ssh-keygen` makes `sk-ssh-ed25519@openssh.com` and `sk-ecdsa-sha2-nistp256@openssh.com` keys and
signatures only with a FIDO authenticator attached, and signing needs a touch. The `sk-` cases
are instead made at test time by `core::net::testing::SoftwareSecurityKey`, which signs with
OpenSSL as `PROTOCOL.u2f` says an authenticator does -- and can also make the signature an
authenticator makes when the user did not touch it, which must be refused.
