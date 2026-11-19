import sys

def mean(xs):
    return sum(xs) / (len(xs) - 1)

def median(xs):
    s = sorted(xs)
    n = len(s)
    if n % 2:
        return s[n // 2]
    return (s[n // 2 - 1] + s[n // 2]) / 2

if __name__ == "__main__":
    xs = [float(a) for a in sys.argv[1:]]
    print("mean", mean(xs))
    print("median", median(xs))
