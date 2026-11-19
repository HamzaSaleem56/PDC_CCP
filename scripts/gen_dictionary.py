#!/usr/bin/env python3
"""Generate a synthetic wordlist for benchmarking.

Self-generated and license-free, per the project's "self-generated test
hashes and authorized datasets" scope -- no real leaked password lists
are used or needed.

Usage: python3 gen_dictionary.py <num_words> > dictionary.txt
"""
import random
import sys

SYLLABLES = ["ba", "ca", "da", "el", "fo", "gi", "ho", "ja", "ka", "li",
             "mo", "na", "pa", "qui", "ro", "sa", "ta", "vi", "wo", "zy",
             "bra", "cro", "dru", "fla", "gri", "plo", "stra", "tri"]

def make_word(rng):
    n = rng.randint(2, 4)
    return "".join(rng.choice(SYLLABLES) for _ in range(n))

def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 42
    rng = random.Random(seed)
    words = set()
    while len(words) < n:
        words.add(make_word(rng))
    for w in words:
        print(w)

if __name__ == "__main__":
    main()
