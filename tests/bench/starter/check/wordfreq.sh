out=$(printf 'The cat and the dog. THE end, and a cat!\nthe\n' | python3 wordfreq.py) || exit 1
echo "$out"
[ "$(echo "$out" | sed 's/^ *//;s/ *$//')" = "4 the
2 and
2 cat" ] || [ "$(echo "$out" | sed 's/^ *//;s/ *$//')" = "4 the
2 cat
2 and" ]
