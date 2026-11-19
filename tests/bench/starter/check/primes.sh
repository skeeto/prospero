cc -O2 -o primes primes.c || exit 1
out=$(./primes 56 59 64 2 1 18446744073709551557 18446744073709551615 18446744073709551616)
echo "$out"
echo "$out" | grep -qx '56: composite' || exit 1
echo "$out" | grep -qx '59: prime' || exit 1
echo "$out" | grep -qx '64: composite' || exit 1
echo "$out" | grep -qx '2: prime' || exit 1
echo "$out" | grep -qx '18446744073709551557: prime' || exit 1
echo "$out" | grep -qx '18446744073709551615: composite' || exit 1
echo "$out" | grep -q '^18446744073709551616: .*overflow' || exit 1
