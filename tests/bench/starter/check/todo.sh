rm -f todo.json
echo '[{"text": "old"}]' > todo.json
python3 todo.py add milk || exit 1
python3 todo.py add eggs || exit 1
python3 todo.py done 2 || exit 1
out=$(python3 todo.py list) || exit 1
echo "$out"
[ "$out" = "1. [ ] old
2. [x] milk
3. [ ] eggs" ]
