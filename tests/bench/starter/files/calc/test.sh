#!/bin/sh
out=$(python3 calc.py 1 2 3 6)
expected="mean 3.0
median 2.5"
if [ "$out" = "$expected" ]; then
    echo PASS
else
    echo FAIL
    echo "expected:"; echo "$expected"
    echo "got:"; echo "$out"
    exit 1
fi
