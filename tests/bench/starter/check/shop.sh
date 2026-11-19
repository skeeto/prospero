! grep -rn calc_total --include='*.py' . || exit 1
grep -q 'def compute_total' pricing.py || exit 1
out=$(python3 main.py) || exit 1
[ "$out" = "cart total: 27.0
direct total: 1.08" ]
