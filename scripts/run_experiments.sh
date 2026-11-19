#!/usr/bin/env bash
# Sweeps worker counts and problem sizes, logs wall-clock time for each run.
# Produces results.csv: mode,workers,dict_size,search_space,wall_seconds
set -euo pipefail
cd "$(dirname "$0")/.."

ALGO="md5"
WORKER_COUNTS=(1 2 4 8)
DICT_SIZES=(2000 10000 50000)

OUT=results.csv
echo "mode,workers,dict_size,search_space,wall_seconds" > "$OUT"

for size in "${DICT_SIZES[@]}"; do
    dict="scripts/dict_${size}.txt"
    targets="scripts/targets_${size}.txt"
    python3 scripts/gen_dictionary.py "$size" > "$dict"
    python3 scripts/gen_targets.py "$dict" 3 "$ALGO" 2> /dev/null > "$targets"
    space=$((size * 404))

    # sequential baseline
    t0=$(date +%s.%N)
    ./sequential --dict "$dict" --targets "$targets" --algo "$ALGO" > /dev/null
    t1=$(date +%s.%N)
    echo "sequential,1,$size,$space,$(echo "$t1 - $t0" | bc)" >> "$OUT"

    for w in "${WORKER_COUNTS[@]}"; do
        procs=$((w + 1)) # +1 for master rank
        t0=$(date +%s.%N)
        mpirun -np "$procs" --oversubscribe ./hash_cracker --dict "$dict" --targets "$targets" --algo "$ALGO" > /dev/null
        t1=$(date +%s.%N)
        echo "distributed,$w,$size,$space,$(echo "$t1 - $t0" | bc)" >> "$OUT"
    done
done

echo "Done. See $OUT (compute speedup = seq_time / distributed_time, efficiency = speedup / workers)."
