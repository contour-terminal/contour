# Orchestrator: topics, automatic boards, agent skills and messages

Date: 2026-11-22 · Branch: `feature/agent-orchestrator`

## Goal

Make the orchestrator the one place that runs and supervises agents across a fleet. On another machine
the only thing you can reach is that machine's daemon (`contour client`), and so its orchestrator tab. From
there you should be able to:

1. start an agent on **anything**: a free-form topic as well as a pull request or an issue;
2. read **what every agent says it is doing**: each run keeps a status board, shown *rendered* in its
   details;
3. trust that **the agent has the skills its prompt names**: the orchestrator checks for the Claude Code
   plugins its tasks need and installs any that are missing;
4. **read the transcript and talk to the agent**, with no terminal on that machine.

These four share one change underneath: a run's subject becomes *an item or a topic*. Everything else
builds on the existing machinery: runs, the dispatch form, `file-read` boards, `agent-transcript`, and
declared commands and panes.

## 1. Topics

**Model.** `ItemRef` gains `TopicRef { std::string slug; }`. A topic run also keeps the text you typed in
the new `Run::topic` field (empty for items). The slug is derived from the text by `Naming`: lower-case
ASCII `[a-z0-9-]`, runs of anything else folded to one `-`, at most 40 characters, `topic` when nothing
survives. Only the slug reaches a branch name or a path (`agent/topic-<slug>-<stamp>`). The text reaches
the prompt and the title, and nothing else. The text is the user's own, typed into the dashboard, so the
anti-goal about *untrusted* text reaching a prompt does not apply. It is still sanitised wherever it is
drawn.

**Config.** `applies_to` accepts `topic`. A topic task's prompt may use `{topic}`, which is replaced by the
text. When no task applies to topics, a built-in one stands in: `{ id: topic, label: "topic", applies_to:
topic, prompt: "{topic}" }`, with the first configured model. Every configuration therefore gets topics with
no change.

**Dashboard.** `n` opens the topic form. Its fields:
- the repository (`←` `→`);
- the topic text, a free-text field with the cursor in it;
- the task (topic tasks only) and the model.

Below them the form shows what the dispatch form shows today: the prompt, the worktree and the command line.
`Enter` dispatches. A topic run is listed under AGENTS, and later Finished, by its text. `d` on a finished
topic run opens the form again with the same text. A topic has no web page: the details show the text in
place of the URL.

**Persistence.** The persisted runs and `agent-runs.jsonl` gain an item of kind `topic`, carrying `slug` and
`topic`. This is an additive change to `contract/agent-runs/`; the contract's README lists the new kind.

## 2. Status boards: automatic, and rendered in the details

**Asked for by every run.** The pane template gains Claude Code's `--append-system-prompt`, through a new
argument `board` of kind `text`:

```
pane agent       @worktrees = claude --model {model} --session-id {session} --append-system-prompt {board} {prompt}
pane agent-plain @worktrees = claude --model {model} --session-id {session} {prompt}
```

A config key `status_board` sets it:
- **omitted:** a default instruction. It tells the agent to keep `STATUS.md` at the worktree's root with the
  `contour-workflows:status-board` skill, current at each state change, and never committed;
- **a string:** your own instruction;
- **`off`:** the run uses `agent-plain`.

This makes the board automatic. The README's "a task's prompt does" becomes "every run's system prompt
does, unless `status_board: off`". A system prompt is an instruction that holds for the whole session,
which a hook request never was. The orchestrator still only *reads* the file.

**Rendered in the details.** The selected agent's details panel shows its board below the run's facts,
rendered as Markdown: headings, emphasis, lists, task boxes, tables, code spans and blocks. It is drawn
inert, as the board screen draws it today, and read every five seconds while the run is selected, as the `b`
screen reads it. The `b` screen renders the same way.

core-cpp's `MarkdownRenderer` writes escape sequences to a `TerminalOutput`, not to a `Canvas`. So
`view/BoardMarkdown.{hpp,cpp}` turns the Markdown into styled canvas lines (core-cpp's `MarkdownInline`,
where it fits). It covers only what boards use, under golden tests.

## 3. The agent's skills

**Config.** A `plugins` key lists the plugins the tasks need, each with the marketplace that provides it:

```yaml
plugins:
  - { id: contour-workflows@contour-terminal, marketplace: contour-terminal/claude-marketplace }
```

The default is that one entry. `plugins: []` turns the check off.

**Check.** A new declared command, `claude-plugins` (`claude plugin list --json`, short timeout), runs at
start and on `R`. A plugin that is not listed, or is listed as not enabled, puts a line among the problems:
"contour-workflows@contour-terminal is not installed · `I` installs it". The dispatch form then asks for a
second `Enter`, as it does today while the status hooks are missing.

**Install.** `I` asks for confirmation, then runs two declared commands:
- `claude-marketplace-add`: `claude plugin marketplace add {source}`;
- `claude-plugin-install`: `claude plugin install {plugin}`.

It then checks again. Both commands run the same `claude` the panes run, through the `programs:` map from
the previous change. The user starts each install from the dashboard, and the installs land in their own
user scope; the orchestrator never edits Claude Code's settings files.

## 4. Transcript and messages

**Transcript.** It already exists (`t`). Two changes:
- while the run's agent is running, the transcript follows: shown on its last page, it is read again every
  five seconds;
- the details panel shows the last three entries of the transcript under the board, read with the same
  command.

**Messages.** `m` opens a message line wherever a run is selected: on its row, its board or its
transcript. It works while the run's pane is open. `Enter` sends the text to the agent; `Esc` drops it.

The text travels through a new daemon-world import:

```wit
/// Types text the user wrote into a pane the extension owns, as a bracketed paste followed by Enter.
pane-send: func(pane: string, text: string) -> result<_, host-error>;
```

- **Host side.** The host accepts it only for a pane the extension owns and opened from a template whose
  manifest declares `pane-input <template>`. Text must be UTF-8. It is refused if it holds a control
  character other than newline or tab, and capped at 16 KiB. It is delivered the way the control API's
  `pane-send` delivers text, so Claude Code receives it as one message.
- **Consent.** The consent dialog lists the new capability: "may type text you write into its agent panes".
- **Guest side.** The orchestrator calls `pane-send` only from the `m` line, with exactly what was typed. It
  never generates input and never reads a screen. The README's anti-goal changes from "the extension typing
  into a pane" to "the extension typing anything into a pane that you did not write and send".

When the agent is waiting on a permission prompt, a message typed there is what Claude Code receives at that
prompt. The README says so; quick replies are left out (YAGNI).

## Errors

| Situation | What the user sees |
| --- | --- |
| Topic text empty | The form does not dispatch; the field says "type a topic". |
| `claude plugin list` fails or its output does not parse | A problem line with the exit status; dispatch is not blocked by it. |
| Install fails | The problem line carries the command's last stderr line and the command to run by hand. |
| `pane-send` refused (`not-owned`, `gone`, `bad-argument`, `not-supported-yet` on an older host) | The message line keeps the text and shows why, in the FailureWords. |
| Board Markdown the renderer does not know | Drawn as plain text, never dropped. |

## Testing

- **Core:** `Naming` slugs (table test); config parsing of `applies_to: topic`, `status_board` and `plugins`;
  engine dispatch of a topic run, through to the pane's argv for each `status_board` value; plugin-list
  parsing and the install flow; persistence round-trips of topic runs.
- **View:** golden screens for the topic form, the details panel with a rendered board and the transcript
  tail, the message line, and the plugin problem line, each at 120×40 and 80×24. `BoardMarkdown` gets table
  tests.
- **Host:** the `pane-send` policy (`DeclaredPanes`): owned only, declared only, text checks. The manifest
  grammar corpus gets `pane-input`, valid and invalid. The WIT ABI checks (`DaemonWorld_test`,
  `WorldCheck_test`). `OwnedPanes`/`SessionHost` delivery through a fake session.
- **Contract:** `contract/manifest-v2/valid/orchestrator.manifest` with its consent text;
  `contract/agent-runs/` with a topic run.

## Phases

Each phase is its own commit (or a few), builds clean, and passes `orchestrator_test`, `vtextension_test`
and the affected suites.

1. **Topics.** Core model, naming, config, topic form, persistence, README.
2. **Boards.** The `board` argument and the two templates, `status_board`, and the rendered board in the
   details and the `b` screen.
3. **Skills.** `plugins`, the check, and the install.
4. **Messages.** `pane-send` through WIT, vtextension, vthost and the manifest grammar; regenerated guest
   bindings; the `m` line; the transcript follow and tail.

## Out of scope

- Quick replies to permission prompts.
- Supervising several machines' agents from one dashboard: each machine's daemon serves its own orchestrator,
  and you attach to each.
- Publishing boards as artifacts: the skill may, but the orchestrator reads only `STATUS.md`.
