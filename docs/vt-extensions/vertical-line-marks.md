# Vertical Line Markers

Suppose you type a lot in the terminal, and I bet you do. Some commands may have inconveniently long
output and you need a way to conveniently scroll the terminal viewport up to the top of that
command. This is what this feature is there for. You can easily walk up/down your markers
like you'd walk up code folds or markers in VIM or other editors.

## Set a mark:

```sh
echo -ne "\033[>M"
```

## Example key bindings in Contour:

```yaml
input_mapping:
    - { mods: [Alt, Shift], key: 'k', mode: '~Alt', action: ScrollMarkUp }
    - { mods: [Alt, Shift], key: 'j', mode: '~Alt', action: ScrollMarkDown }
```

It is recommended to integrate the marker into your command prompt, such as `$PS1` in bash or sh to
have automatic markers set.

## Integration into ZSH:

zsh is way too configurable to give a fully generic answer here, but to show how you can integrate vertical line markers when using [powerlevel9k](https://github.com/Powerlevel9k/powerlevel9k), this is what your `~/.zshrc` config could contain:

```sh
prompt_setmark() {
    echo -ne "%{\033[>M%}"
}
POWERLEVEL9K_LEFT_PROMPT_ELEMENTS=(setmark user dir vcs)
```

## Integration into Bash

Bash is usually highly customized to your needs, but the bottom line would be as suggested below. You can create your custom `prompt_setmark` function that contains `\\[` and `\\]` as enclosing markers for the escape sequence to tell your shell that they do not change the current cursor position, and then use this function in our `PS1` environment variable or invoked inside your function assigned to `PROMPT_COMMAND`.

```sh
prompt_setmark() {
    echo -ne "\\[\033[>M\\]"
}

# extending existing PS1
export PS1="`prompt_setmark`${PS1}"
```

