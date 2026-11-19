#!/usr/bin/env python3
"""Pick N random points inside the cracker's own search space (dictionary
word + rule-based mutation) and hash them, so the generated target hashes
are guaranteed to be reachable by the cracker -- avoids wasting an
experiment on an unreachable hash.

Mirrors common.hpp's buildCandidate() exactly. Keep in sync if you change
the mutation rules on the C++ side.

Usage: python3 gen_targets.py dictionary.txt <num_targets> <md5|sha256> [salt] > targets.txt
"""
import hashlib
import random
import sys

SUFFIX_COUNT = 101
VARIANTS_PER_WORD = 2 * 2 * SUFFIX_COUNT

LEET = {"a": "4", "e": "3", "i": "1", "o": "0", "s": "5"}

def apply_leet(w):
    return "".join(LEET.get(c.lower(), c) for c in w)

def toggle_first_case(w):
    if not w:
        return w
    return (w[0].lower() if w[0].isupper() else w[0].upper()) + w[1:]

def build_candidate(word, variant_idx):
    suffix_idx = variant_idx % SUFFIX_COUNT
    rem = variant_idx // SUFFIX_COUNT
    leet_flag = rem % 2
    case_flag = rem // 2
    w = word
    if case_flag:
        w = toggle_first_case(w)
    if leet_flag:
        w = apply_leet(w)
    if suffix_idx > 0:
        w += f"{suffix_idx - 1:02d}"
    return w

def main():
    dict_path, n, algo = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    salt = sys.argv[4] if len(sys.argv) > 4 else ""
    with open(dict_path) as f:
        words = [w.strip() for w in f if w.strip()]

    rng = random.Random(7)
    hasher = hashlib.md5 if algo == "md5" else hashlib.sha256

    chosen = set()
    print(f"# plaintexts used (for your report, not fed to the cracker): ", file=sys.stderr)
    while len(chosen) < n:
        word = rng.choice(words)
        variant = rng.randrange(VARIANTS_PER_WORD)
        cand = build_candidate(word, variant)
        if cand in chosen:
            continue
        chosen.add(cand)
        h = hasher((salt + cand).encode()).hexdigest()
        print(f"{cand} -> {h}", file=sys.stderr)
        print(h)

if __name__ == "__main__":
    main()
