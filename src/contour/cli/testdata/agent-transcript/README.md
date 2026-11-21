# Claude Code session logs

The logs `contour agent transcript` is tested against.

- `<version>/` holds a log **recorded** from Claude Code `<version>` (its `VERSION` file has `claude
  --version`'s line), in a scratch directory with a scratch `HOME`: a two-file git repository whose agent
  wrote a file, ran `mkdir sub && cd sub && ls -a ..` and replied. Its entries' `cwd` follow the agent's
  `cd`, which is why a log is bound to the directory it was written *at or beneath*. The log was scrubbed by
  `scripts/scrub-session-log.py` (an allowlist of keys; UUIDs and tool-use ids numbered, the scratch root `/home/user/proj`),
  so it holds the structure and the scratch task's own words only. `page-1.txt` is the verb's text output.
- `synthetic/` holds logs written by hand from the recorded structure, one case each; see its README.

The `fixture-privacy` lint gate (`scripts/check-fixture-privacy.py`) checks every file here.
