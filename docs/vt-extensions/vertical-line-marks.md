# Vertical Line Markers

Suppose you type a lot in the terminal, and I bet you do. Some commands may have inconveniently long
output and you need a way to conveniently scroll the terminal viewport up to the top of that
command. This is what this feature is there for. You can easily walk up/down your markers
like you'd walk up code folds or markers in VIM or other editors.

## Set a mark

A mark is set by the `OSC 133 ; A` shell-integration sequence, which says "a new prompt starts on
this line":

```sh
printf "\033]133;A\033\\"
```

!!! note

    Contour used to offer a dedicated `SETMARK` sequence (`CSI > M`) for this. It was deprecated in
    favour of `OSC 133 ; A` and **removed in 0.7.1** — `OSC 133 ; A` does the same thing and, unlike
    a Contour-only extension, is understood by other terminals too.

You will usually not write this by hand: Contour's bundled
[shell integration](osc-133-shell-integration.md) already emits it (along with the rest of OSC 133),
which is what also drives "copy last command output" and output folding. Install it with:

```sh
contour generate integration shell zsh to ~/.config/contour/shell-integration.zsh
```

## Example key bindings in Contour

```yaml
input_mapping:
    - { mods: [Alt, Shift], key: 'k', mode: '~Alt', action: ScrollMarkUp }
    - { mods: [Alt, Shift], key: 'j', mode: '~Alt', action: ScrollMarkDown }
```

If you would rather integrate the mark into your prompt by hand than install the shell integration,
the sections below show how.

## Integration into ZSH

zsh is way too configurable to give a fully generic answer here, but to show how you can integrate vertical line markers when using [powerlevel9k](https://github.com/Powerlevel9k/powerlevel9k), this is what your `~/.zshrc` config could contain:

```sh
prompt_setmark() {
    echo -ne "%{\033]133;A\033\\\\%}"
}
POWERLEVEL9K_LEFT_PROMPT_ELEMENTS=(setmark user dir vcs)
```

## Integration into Bash

Bash is usually highly customized to your needs, but the bottom line would be as suggested below. You can create your custom `prompt_setmark` function that contains `\\[` and `\\]` as enclosing markers for the escape sequence to tell your shell that they do not change the current cursor position, and then use this function in our `PS1` environment variable or invoked inside your function assigned to `PROMPT_COMMAND`.

```sh
prompt_setmark() {
    echo -ne "\\[\033]133;A\033\\\\\\]"
}

# extending existing PS1
export PS1="`prompt_setmark`${PS1}"
```
