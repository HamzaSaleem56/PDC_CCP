#!/usr/bin/env bash
# One-command demo for the distributed hash cracker.
#
#   ./demo.sh            # 4 workers, quick estimated speedup
#   ./demo.sh 2          # 2 workers
#   ./demo.sh 4 baseline # 4 workers, real measured speedup (slower)
#
# Builds the project if needed, generates a small sample dictionary and some
# target hashes, then runs the MPI + OpenMP cracker on them. Meant for a fresh
# machine (e.g. a GitHub Codespace) where you just want to see it work.
set -euo pipefail
cd "$(dirname "$0")"

WORKERS="${1:-4}"
MODE="${2:-}"

# 1. Build if the binary isn't there yet.
if [ ! -x ./hash_cracker ]; then
    echo ">> Building the project..."
    make
fi

# 2. Make a small sample dictionary + target hashes to crack.
DICT="scripts/demo-dict.txt"
TARGETS="scripts/demo-targets.txt"
echo ">> Generating a sample dictionary (5,000 words) and 3 target hashes..."
python3 scripts/gen_dictionary.py 5000 > "$DICT"
python3 scripts/gen_targets.py "$DICT" 3 md5 2>/dev/null > "$TARGETS"

# 3. Run it. One master rank + N worker ranks.
NP=$(( WORKERS + 1 ))
FLAGS="--oversubscribe"                      # allow more ranks than CPU cores
# mpirun refuses to run as root unless told to; harmless to add when needed.
if [ "$(id -u)" = "0" ]; then
    FLAGS="$FLAGS --allow-run-as-root"
fi

EXTRA=""
if [ "$MODE" = "baseline" ]; then
    EXTRA="--baseline"
    echo ">> Running with $WORKERS workers + 1 master, real sequential baseline..."
else
    echo ">> Running with $WORKERS workers + 1 master..."
fi

echo
mpirun $FLAGS -np "$NP" ./hash_cracker \
    --dict "$DICT" --targets "$TARGETS" --algo md5 $EXTRA
