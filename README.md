# Distributed Password/Hash Cracker — End-to-End Walkthrough

MPI (distributed, master-worker) + OpenMP (shared-memory parallel) hash
cracker, with adaptive load balancing, timeout-based fault handling, and
a rule-based (not just raw-dictionary) attack. Built and smoke-tested on
Ubuntu/WSL2-equivalent with OpenMPI 4.1 + OpenSSL 3.0.

**Ethics/scope**: only self-generated dictionaries and self-generated
target hashes are used (`scripts/gen_dictionary.py`, `scripts/gen_targets.py`).
No real leaked password lists, no third-party hashes.

## 1. Architecture

```
                    ┌─────────────────────┐
                    │   MASTER (rank 0)    │
                    │  - owns the flat     │
                    │    index space       │
                    │    [0, total)        │
                    │  - adaptive chunk    │
                    │    sizing per worker │
                    │  - timeout sweep /   │
                    │    reassignment      │
                    │  - stop condition    │
                    └─────┬────────┬───────┘
                MPI_Send/Recv    MPI_Send/Recv
                          │              │
              ┌───────────┘              └───────────┐
              ▼                                        ▼
     ┌─────────────────┐                      ┌─────────────────┐
     │ WORKER rank 1    │                      │ WORKER rank N    │
     │ OpenMP threads   │        ...           │ OpenMP threads   │
     │ hash a range of  │                      │ hash a range of  │
     │ candidates       │                      │ candidates       │
     └─────────────────┘                      └─────────────────┘
```

The search space is **not** a literal word list handed around — it's a
flat index space `[0, dict_size * 404)`. Index `i` deterministically maps
to one candidate string via `candidateAt()` in `common.hpp`: word
`i / 404`, mutation variant `i % 404` (case toggle × leet-speak ×
2-digit numeric suffix, 2×2×101 = 404 combinations per word). This is
what lets the master hand out cheap integer ranges instead of shipping
strings, and is what makes "dictionary attack" into "rule-based attack"
(a materially bigger, non-trivial search space).

### Message protocol (see `common.hpp`)
| Tag | Direction | Payload | Meaning |
|---|---|---|---|
| `TAG_READY` | worker→master | none | "I'm idle, give me work" |
| `TAG_TASK` | master→worker | `Task{task_id,start,end}` | here's your index range |
| `TAG_TERMINATE` | master→worker | none | no more work, exit |
| `TAG_RESULT` | worker→master | `ResultMsg{task_id,processed,foundCount,founds[]}` | chunk done, 0+ matches |

### Adaptive load balancing (pull-based, not static)
Workers never get pre-assigned a fixed share up front. Each idle worker
*requests* a chunk; the master sizes that chunk as
`throughput_estimate(worker) × time_slice` (default time-slice: 0.5s of
work), and updates `throughput_estimate` after every completed chunk
with an exponential moving average of observed candidates/sec. Fast
workers converge to bigger chunks, slow ones to smaller chunks — this
is the direct fix for heterogeneous worker speed (which you will
genuinely have on WSL2/a shared VM host, since ranks compete for the
same physical cores).

### Timeout-based fault handling
Standard MPI (without the ULFM extension, which isn't in stock
OpenMPI/MPICH) aborts the *entire* job if a rank actually crashes — so
"restart a dead process" isn't a realistic demo without extra
tooling. What **is** realistic and still a genuine CCP-level design
problem: a worker that's alive but *stalled* (overloaded, stuck,
pathologically slow). The master tracks `expected_time` per outstanding
chunk and reassigns it to someone else if it's more than
`--timeout-factor` × overdue, and down-weights that worker's throughput
estimate so it gets smaller chunks next time. This was tested directly
(see §4) — artificially stalling one rank, the master detects it,
reassigns the chunk, and the run still completes correctly.

Because the orphaned worker is still alive and will eventually reply,
results carry a `task_id`; the master drops results for task_ids it has
already reassigned/completed, so a late duplicate reply can't corrupt
the found-matches set.

### Multi-target, salted hashing
`targets.txt` can hold multiple hashes; the cracker doesn't stop at the
first match, it keeps going until **all** targets are found or the
space is exhausted. `--salt` is prepended to every candidate before
hashing (`hash(salt + candidate)`), and `--algo md5|sha256` selects the
digest (via OpenSSL EVP, so swapping in SHA-1/SHA-512/etc. later is a
one-line change).

## 1b. Watching It Happen: the live message trace

By default (`--trace` is on unless you pass `--no-trace`) every MPI message
between master and workers is printed live, colour-coded by rank, as it's
sent — so the request/response cycle is visible on the terminal instead of
happening silently "under the hood":

```
[t=  0.000s] WORKER-1  ──READY────────────────▶ MASTER   (requesting work)
[t=  0.036s] MASTER    ──TASK id=0 range=[0,10,000) size=10,000──▶ WORKER-1
[t=  0.054s] WORKER-1  ──RESULT id=0 processed=10,000 found=1──▶ MASTER
[MASTER] MATCH  hash=4ee7...  candidate=quicroel36  (found by rank 1)
[t=  0.087s] MASTER    ──TERMINATE─────▶ WORKER-1 (no more work)
```

Every 0.5s of wall time, a dim `[METRICS]` line also prints a live
dashboard: elapsed time, candidates processed so far (and % of the total
search space), aggregate throughput across all workers combined, and how
many workers are currently busy — so you can watch progress and load in
real time, not just at the end.

**Note on line ordering**: master and workers are separate OS processes
each writing to the same terminal through `mpirun`. Each line's timestamp
is accurate (all ranks share one `MPI_Wtime()` reference, broadcast at
startup), but because small MPI messages can return from `MPI_Send`
before the receiver has actually processed them (the "eager" protocol),
and because each process's own stdout buffer flushes independently, you
may occasionally see a line appear slightly out of chronological order in
the merged output (e.g. a worker's next `READY` printed a moment before
the master finishes logging its *previous* `RESULT`). This is normal,
harmless interleaving of concurrent processes' output — not a bug in the
coordination logic itself — and is itself a small, honest teaching point
about distributed systems: "when it was logged" and "when it truly
happened" are not the same thing once you have multiple independent
clocks and buffers in play.

Use `--no-trace` to suppress all of this for clean, low-overhead timing
runs (already used by `scripts/run_experiments.sh`, so your measured
times aren't inflated by console I/O).

## 1c. Metrics in the Final Summary

At the end of a run you now get, in addition to the original summary:
- **Per-worker contribution table**: how many candidates each worker
  actually finished, as a % of the total — direct visual evidence of
  whether the adaptive load balancer did its job (ranks should end up
  with roughly even shares, or logically-explained uneven shares if one
  stalled).
- **Speedup and efficiency**, computed automatically: the master runs a
  short single-core calibration at startup (hashes ~50,000 candidates
  with no MPI/OpenMP involved, to estimate one core's raw candidates/sec),
  extrapolates an estimated sequential time from that, and reports
  `speedup = T_seq_estimate / T_parallel` and `efficiency = speedup / workers`.
  This is clearly labelled as an *estimate* — for the rigorous,
  ground-truth number for your report, run `./sequential` on the exact
  same dictionary/targets and compare its actually-measured wall time
  (see `run_experiments.sh`, which does this automatically). Disable the
  quick estimate with `--no-calibrate` if you only want the real thing.

## 2. Build

```bash
sudo apt-get update
sudo apt-get install -y openmpi-bin libopenmpi-dev libssl-dev build-essential
cd distributed_hash_cracker
make            # builds ./hash_cracker and ./sequential
```

## 3. Run

```bash
# 1. generate a synthetic dictionary and some reachable target hashes
python3 scripts/gen_dictionary.py 20000 > dict.txt
python3 scripts/gen_targets.py dict.txt 3 md5 > targets.txt   # plaintexts printed to stderr for your report

# 2. sequential baseline
./sequential --dict dict.txt --targets targets.txt --algo md5

# 3. distributed + parallel run (4 worker ranks + 1 master)
mpirun -np 5 ./hash_cracker --dict dict.txt --targets targets.txt --algo md5
```

Useful flags: `--salt STRING`, `--time-slice 0.5` (seconds of work per
adaptive chunk), `--timeout-factor 4.0` (reassignment sensitivity),
`--no-trace` (suppress the live message trace + dashboard), `--no-calibrate`
(skip the automatic speedup estimate).

### Demoing fault tolerance live
```bash
mpirun -np 5 ./hash_cracker --dict dict.txt --targets targets.txt --algo md5 \
    --stall-rank 2 --stall-seconds 8 --timeout-factor 1.5
```
Rank 2 will deliberately sleep on its first chunk; watch the master log
detect the overdue chunk and reassign it to a different worker while
the run still finishes correctly.

### On the real WSL2/VM environment
Nothing here is sandbox-specific — `mpirun -np N` and OpenMP thread
count are controlled by however many cores you give WSL2/the VM
(`.wslconfig` → `processors=N` for WSL2). More real cores than this dev
sandbox had is where you should actually see distributed beat
sequential; in a core-starved environment the MPI/OpenMP coordination
overhead can dominate, which is itself a useful, reportable result for
your efficiency analysis.

## 4. What was actually verified (not just compiled)
- Builds cleanly with `-Wall`, no warnings.
- Single-target MD5 dictionary attack: sequential and distributed agree.
- **Multi-target bug found and fixed during testing**: an early-exit
  optimization inside a chunk was silently skipping other targets that
  happened to fall in the same chunk. Fixed by collecting *all* matches
  per chunk instead of stopping at the first (see `main.cpp`,
  `localFinds`). Worth mentioning in your report as a real debugging
  story, not just a feature list.
- Multi-target MD5, and SHA-256 + salt, each independently verified
  against Python's `hashlib` for the same plaintext/salt/algo.
- Timeout → reassignment path triggered on purpose and confirmed in the
  master's own log output (`[MASTER] rank 2 overdue ... -> reassigning
  chunk`), with the run still completing with correct results.

## 5. Experiments for your report
```bash
bash scripts/run_experiments.sh
```
Sweeps worker counts {1,2,4,8} × dictionary sizes {2000,10000,50000},
writes `results.csv` with wall-clock time per (mode, workers, size).
From that, compute per the course's expected metrics:
- **Speedup(p)** = T_sequential / T_distributed(p workers)
- **Efficiency(p)** = Speedup(p) / p
- **Strong scaling**: fixed problem size, vary `p` — speedup vs. p curve
- **Weak scaling**: scale dictionary size with `p` — time should stay ~flat
- Compare observed speedup against the **Amdahl's Law** ceiling using
  the measured serial fraction (task dispatch + result aggregation +
  dictionary load), and against **Gustafson's Law** for the weak-scaling
  case.

## 6. CCP attribute mapping (IEA WP1–WP7)
| Attribute | How this project satisfies it |
|---|---|
| WP1 (mandatory) | First-principles parallel/distributed computing theory: MPI message passing, OpenMP shared-memory parallelism, Amdahl/Gustafson analysis, cryptographic hashing |
| WP2 | Explicit trade-off: chunk size vs. communication overhead; throughput-estimate smoothing vs. responsiveness to real slowdowns |
| WP3 | No single obvious algorithm: adaptive pull-based balancing (vs. naive static chunking) and idempotent task reassignment under uncertainty both require original design, not a textbook recipe |
| WP4 | Rule-based mutation search space (combinatorial, not a flat list) + multi-target + salted hashing are beyond the routine "hash one word, compare" exercise |
| WP7 | Interdependent sub-problems: partitioning ↔ throughput estimation ↔ timeout detection ↔ termination condition all have to stay consistent with each other |

That's WP1 + three more (WP2, WP3, WP4, WP7) — comfortably clears the
"WP1 + at least two of WP2–WP7" bar.
