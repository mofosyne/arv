# bash completion for arv. It asks arv itself what fits (arv __complete), so it follows the
# commands of the arv installed, and never needs changing when arv does; where arv offers nothing
# (a folder, a disc, an image), bash completes file names as usual.
#   installed by make install; in a checkout: source src/arv/completion/arv.bash
_arv() {
    local IFS=$'\n'
    COMPREPLY=($("${COMP_WORDS[0]}" __complete "${COMP_WORDS[@]:1:COMP_CWORD}" 2>/dev/null))
}
complete -o default -o bashdefault -F _arv arv
