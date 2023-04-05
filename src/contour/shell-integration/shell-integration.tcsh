alias precmd 'echo -n "\\e[?2028l\\e[>M\\e]7;$PWD\\e\\\\";'
alias postcmd 'echo -n "\\e[?2028h";'
