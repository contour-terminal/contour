# Synthetic session logs for `contour agent stats`

Written by hand in the shapes Claude Code 2.1.330 writes, so that no recorded conversation is carried:

- `basic.jsonl` -- a finished session: a response written as two lines that repeat its usage, tool calls and
  a failed result, a typed answer and an inserted (`isMeta`) prompt, an API error, a compaction, a pull
  request link, a turn's duration and the `cost-state` written on exit.
- `running.jsonl` -- `basic.jsonl` without its `cost-state`: a session still running.
- `empty.jsonl` -- one line binding it to the run, and nothing else.
- `broken.jsonl` -- a line that is not JSON among two that are.
