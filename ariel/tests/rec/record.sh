#!/bin/sh
# Record the PTY sessions behind the golden dumps:
#   $ sh record.sh BUILD/ariel-replay [NAME...]
# Each NAME.keys drives a program; NAME.vt holds its output, NAME.txt the
# dump ariel_test compares against. Review a new dump before committing.
set -e
replay=$1; shift
cd "$(dirname "$0")"
here=$(pwd)
[ $# -gt 0 ] || set -- bash less vim
env="env -i PATH=/opt/homebrew/bin:/usr/bin:/bin HOME=/nonexistent LANG=en_US.UTF-8 TERM=xterm-256color"
for name; do
    case $name in
    bash) cmd="$env PS1=bash% bash --norc --noprofile -i" ;;
    less) cmd="$env LESSHISTFILE=- less -R $here/input.txt" ;;
    vim)  cmd="$env vim -u NONE -i NONE -N -n $here/input.txt" ;;
    *) echo "record.sh: unknown $name" >&2; exit 1 ;;
    esac
    $replay --pty -s 80x24 -a -k $name.keys -o $name.vt -- $cmd >$name.txt
    echo "$name: $(wc -c <$name.vt) bytes"
done
