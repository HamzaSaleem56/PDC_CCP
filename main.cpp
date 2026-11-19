// main.cpp
// Distributed + parallel password/hash cracker.
//   rank 0        -> master: partitions work, adaptive load balancing,
//                    timeout-based reassignment of stalled workers
//   rank 1..N-1   -> worker: OpenMP-parallel hashing over an assigned
//                    index range, reports back to master
//
// Usage:
//   mpirun -np <P> ./hash_cracker --dict dict.txt --targets targets.txt
//                                 --algo md5|sha256 [--salt SALT]
//                                 [--time-slice 0.5] [--timeout-factor 4.0]
//                                 [--no-trace] [--no-calibrate]
//                                 [--stall-rank R --stall-seconds S]
//
// targets.txt: one hex hash per line (all cracked, or search space
// exhausted, ends the run).
//
// By default this prints a live, colour-coded trace of every MPI message
// between master (rank 0) and workers (rank 1..N-1) -- READY/TASK/
// RESULT/TERMINATE -- like a scrolling sequence diagram, plus a periodic
// metrics dashboard and a final summary with speedup/efficiency.
// Use --no-trace for clean, low-overhead timing runs (e.g. inside
// scripts/run_experiments.sh).
#include <mpi.h>
#include <omp.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <chrono>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <thread>
#include "common.hpp"

struct Args {
    std::string dictPath;
    std::string targetsPath;
    std::string algoStr = "md5";
    std::string salt = "";
    double timeSlice = 0.5;       // seconds of work per chunk (adaptive sizing target)
    double timeoutFactor = 4.0;   // reassign a worker's chunk if it's this many x overdue
    int stallRank = -1;            // DEMO ONLY: this rank artificially stalls, to show
                                    // the master's timeout/reassignment logic live
    double stallSeconds = 10.0;
    bool trace = true;              // live master<->worker message trace on stdout
    bool calibrate = true;          // quick single-core calibration for speedup estimate
};

static Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        auto next = [&](){ return std::string(argv[++i]); };
        if (s == "--dict") a.dictPath = next();
        else if (s == "--targets") a.targetsPath = next();
        else if (s == "--algo") a.algoStr = next();
        else if (s == "--salt") a.salt = next();
        else if (s == "--time-slice") a.timeSlice = std::stod(next());
        else if (s == "--timeout-factor") a.timeoutFactor = std::stod(next());
        else if (s == "--stall-rank") a.stallRank = std::stoi(next());
        else if (s == "--stall-seconds") a.stallSeconds = std::stod(next());
        else if (s == "--no-trace") a.trace = false;
        else if (s == "--no-calibrate") a.calibrate = false;
    }
    return a;
}

static std::vector<std::string> loadLines(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

// Comma-formats a count for readability in trace/metrics output, e.g. 1234567 -> "1,234,567"
static std::string fmtNum(long long n) {
    std::string s = std::to_string(n);
    int insertPos = (int)s.size() - 3;
    while (insertPos > 0) { s.insert(insertPos, ","); insertPos -= 3; }
    return s;
}

// Quick, single-threaded (no OpenMP, no MPI) calibration: hashes a small sample
// of the real search space to estimate one CPU core's candidates/sec rate.
// This is what lets the master print an automatic, self-contained speedup
// estimate without requiring you to run the ./sequential binary separately
// (though that full run, via run_experiments.sh, remains the rigorous
// ground-truth measurement for your report).
static double calibrateSingleCoreRate(const std::vector<std::string>& dict, long long total,
                                       Algo algo, const std::string& salt) {
    long long sampleSize = std::min<long long>(total, 50000);
    auto start = std::chrono::steady_clock::now();
    size_t sink = 0;
    for (long long i = 0; i < sampleSize; i++) {
        std::string cand = candidateAt(dict, i);
        std::string h = hashCandidate(cand, salt, algo);
        sink += h.size();
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    (void)sink;
    return sampleSize / std::max(1e-6, secs);
}

// ======================================================================
// MASTER
// ======================================================================
static void runMaster(const Args& args, const std::vector<std::string>& dict,
                       std::unordered_set<std::string> targets, Algo algo,
                       int worldSize, double mpiT0) {
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();

    long long total = totalCandidates(dict.size());

    double singleCoreRate = 0.0;
    if (args.calibrate) {
        singleCoreRate = calibrateSingleCoreRate(dict, total, algo, args.salt);
        std::cout << "[MASTER] calibration: one CPU core hashes ~" << fmtNum((long long)singleCoreRate)
                  << " candidates/sec (used for the speedup estimate below)\n";
    }
    long long nextIndex = 0;
    long long taskCounter = 0;
    bool exhausted = false;

    const int numWorkers = worldSize - 1;

    // Per-worker adaptive throughput estimate (candidates/sec), seeded from
    // the ACTUAL search-space size rather than a fixed constant. A fixed
    // guess (e.g. 2.0e5 cand/s at the default 0.5s time-slice -> a flat
    // 100,000-candidate first chunk) breaks down whenever the whole search
    // space is small: the first worker to ask for work can end up claiming
    // the entire remaining space in one chunk, leaving every other worker
    // immediately told TERMINATE with nothing to do. Dividing by
    // (numWorkers * 4) instead of numWorkers deliberately leaves room for
    // a few adaptive rounds -- the first round hands out roughly a quarter
    // of a fair per-worker share each, so later chunks can still resize
    // based on real observed throughput instead of the whole job finishing
    // in a single, unbalanced round.
    long long targetInitialChunk = std::max<long long>(1000, total / (numWorkers * 4));
    double initialThroughputGuess = targetInitialChunk / std::max(args.timeSlice, 0.01);

    std::unordered_map<int, double> throughput;
    for (int r = 1; r < worldSize; r++) throughput[r] = initialThroughputGuess;

    // outstanding[task_id] = {worker, start, end, assignedAt, expectedSecs}
    struct Outstanding { int worker; long long start, end; Clock::time_point assignedAt; double expectedSecs; };
    std::unordered_map<long long, Outstanding> outstanding;

    // requeue list of ranges that timed out or were orphaned, remembering
    // which rank and task they were taken from (so we can log who picks
    // them up later)
    struct RequeuedChunk { long long start, end; int origWorker; long long origTaskId; };
    std::vector<RequeuedChunk> requeue;

    std::unordered_map<std::string, std::string> foundMatches; // hash -> candidate
    std::unordered_map<int, long long> processedByWorker;       // worker -> candidates it actually finished
    long long totalProcessed = 0;
    int terminatedWorkers = 0;
    int reassignedChunks = 0;

    auto allTargetsFound = [&](){ return foundMatches.size() >= targets.size(); };

    auto lastDashboard = Clock::now();

    auto printDashboard = [&](){
        double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
        double aggRate = elapsed > 0 ? totalProcessed / elapsed : 0.0;
        double pct = total > 0 ? (100.0 * totalProcessed / total) : 0.0;
        printf("\033[2m[METRICS] t=%.1fs | processed=%s/%s (%.1f%%) | aggregate throughput=%s cand/s | "
               "busy workers=%d/%d | reassigned=%d\033[0m\n",
               elapsed, fmtNum(totalProcessed).c_str(), fmtNum(total).c_str(), pct,
               fmtNum((long long)aggRate).c_str(), (int)outstanding.size(), numWorkers, reassignedChunks);
    };

    while (terminatedWorkers < numWorkers) {
        MPI_Status status;
        int flag = 0;
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);

        if (flag) {
            if (status.MPI_TAG == TAG_READY) {
                int worker = status.MPI_SOURCE;
                int dummy;
                MPI_Recv(&dummy, 1, MPI_INT, worker, TAG_READY, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                bool stop = exhausted && requeue.empty() && outstanding.empty();
                stop = stop || allTargetsFound();

                if (stop) {
                    MPI_Send(nullptr, 0, MPI_BYTE, worker, TAG_TERMINATE, MPI_COMM_WORLD);
                    if (args.trace) {
                        char buf[96];
                        snprintf(buf, sizeof(buf), "\u2500\u2500TERMINATE\u2500\u2500\u2500\u2500\u2500\u25b6 WORKER-%d (no more work)", worker);
                        traceLine(mpiT0, 0, buf);
                    }
                    terminatedWorkers++;
                    continue;
                }

                // Decide chunk range: prefer requeued work first (keeps coverage correct)
                long long start, end;
                bool isReassignment = false;
                int origWorker = -1;
                long long origTaskId = -1;
                if (!requeue.empty()) {
                    auto chunk = requeue.back(); requeue.pop_back();
                    start = chunk.start; end = chunk.end;
                    isReassignment = true;
                    origWorker = chunk.origWorker;
                    origTaskId = chunk.origTaskId;
                } else if (nextIndex < total) {
                    double est = throughput[worker];
                    long long chunkSize = std::max<long long>(1000, (long long)(est * args.timeSlice));
                    start = nextIndex;
                    end = std::min(total, start + chunkSize);
                    nextIndex = end;
                    if (nextIndex >= total) exhausted = true;
                } else {
                    // nothing left and nothing outstanding yet to requeue -> tell worker to wait by
                    // immediately terminating it; it can't help further.
                    MPI_Send(nullptr, 0, MPI_BYTE, worker, TAG_TERMINATE, MPI_COMM_WORLD);
                    if (args.trace) {
                        char buf[96];
                        snprintf(buf, sizeof(buf), "\u2500\u2500TERMINATE\u2500\u2500\u2500\u2500\u2500\u25b6 WORKER-%d (nothing left)", worker);
                        traceLine(mpiT0, 0, buf);
                    }
                    terminatedWorkers++;
                    continue;
                }

                Task task{ taskCounter++, start, end };
                double expected = (end - start) / std::max(1.0, throughput[worker]);
                outstanding[task.task_id] = { worker, start, end, Clock::now(), expected };
                MPI_Send(&task, sizeof(Task), MPI_BYTE, worker, TAG_TASK, MPI_COMM_WORLD);

                if (args.trace) {
                    char buf[160];
                    snprintf(buf, sizeof(buf), "\u2500\u2500TASK id=%lld range=[%s,%s) size=%s\u2500\u2500\u25b6 WORKER-%d",
                             task.task_id, fmtNum(start).c_str(), fmtNum(end).c_str(),
                             fmtNum(end - start).c_str(), worker);
                    traceLine(mpiT0, 0, buf);
                }

                if (isReassignment) {
                    std::cout << "\033[1;91m[MASTER] reassigned chunk (orig task " << origTaskId
                              << ", stalled on rank " << origWorker << ") -> now given to rank "
                              << worker << " as new task " << task.task_id << "\033[0m\n";
                }

            } else if (status.MPI_TAG == TAG_RESULT) {
                ResultMsg res;
                MPI_Recv(&res, sizeof(ResultMsg), MPI_BYTE, status.MPI_SOURCE, TAG_RESULT,
                         MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                auto it = outstanding.find(res.task_id);
                if (it != outstanding.end()) {
                    int worker = it->second.worker;
                    double elapsed = std::chrono::duration<double>(Clock::now() - it->second.assignedAt).count();
                    long long chunkLen = it->second.end - it->second.start;
                    if (elapsed > 0 && chunkLen > 0) {
                        double observed = chunkLen / elapsed;
                        // exponential moving average -> smooths noisy single-chunk timing
                        throughput[worker] = 0.5 * throughput[worker] + 0.5 * observed;
                    }
                    totalProcessed += res.processed;
                    processedByWorker[worker] += res.processed;
                    outstanding.erase(it);

                    if (args.trace) {
                        char buf[160];
                        snprintf(buf, sizeof(buf), "\u2500\u2500RESULT id=%lld processed=%s found=%d\u2500\u2500\u25b6 MASTER",
                                 res.task_id, fmtNum(res.processed).c_str(), res.foundCount);
                        traceLine(mpiT0, worker, buf);
                    }

                    for (int k = 0; k < res.foundCount; k++) {
                        foundMatches[res.founds[k].matchedHash] = res.founds[k].candidate;
                        std::cout << "\033[1;92m[MASTER] MATCH  hash=" << res.founds[k].matchedHash
                                  << "  candidate=" << res.founds[k].candidate
                                  << "  (found by rank " << status.MPI_SOURCE << ")\033[0m\n";
                    }
                } else {
                    // Result for a chunk we already reassigned/timed out — duplicate, ignore
                    // other than recording any genuine finds it still reports.
                    for (int k = 0; k < res.foundCount; k++)
                        foundMatches[res.founds[k].matchedHash] = res.founds[k].candidate;
                }
            }
        }

        // --- timeout sweep: reassign chunks from workers that are badly overdue ---
        auto now = Clock::now();
        for (auto it = outstanding.begin(); it != outstanding.end(); ) {
            double elapsed = std::chrono::duration<double>(now - it->second.assignedAt).count();
            double limit = std::max(1.0, it->second.expectedSecs) * args.timeoutFactor;
            if (elapsed > limit) {
                std::cout << "[MASTER] rank " << it->second.worker
                          << " overdue on task " << it->first
                          << " (" << elapsed << "s > " << limit << "s) -> reassigning chunk\n";
                requeue.push_back({ it->second.start, it->second.end, it->second.worker, it->first });
                // Penalize this worker's throughput estimate so it gets smaller chunks next time
                throughput[it->second.worker] *= 0.5;
                reassignedChunks++;
                it = outstanding.erase(it);
            } else {
                ++it;
            }
        }

        if (std::chrono::duration<double>(now - lastDashboard).count() > 0.5) {
            printDashboard();
            lastDashboard = now;
        }
    }

    double wallSecs = std::chrono::duration<double>(Clock::now() - t0).count();

    std::cout << "\n\033[1m===== RUN SUMMARY (distributed) =====\033[0m\n";
    std::cout << "workers                     : " << numWorkers << "\n";
    std::cout << "search space                : " << fmtNum(total) << " candidates\n";
    std::cout << "processed                   : " << fmtNum(totalProcessed) << "\n";
    std::cout << "targets found                : " << foundMatches.size() << " / " << targets.size() << "\n";
    std::cout << "reassigned (stalled) chunks  : " << reassignedChunks << "\n";
    std::cout << "wall time (T_parallel)       : " << wallSecs << " s\n";
    std::cout << "aggregate throughput         : " << fmtNum((long long)(totalProcessed / std::max(1e-6, wallSecs))) << " cand/s\n";

    std::cout << "\n--- per-worker contribution (shows how well load was balanced) ---\n";
    for (auto& kv : processedByWorker) {
        double share = totalProcessed > 0 ? 100.0 * kv.second / totalProcessed : 0.0;
        printf("%s  WORKER-%-2d : %12s candidates  (%.1f%% of all work done, final est. rate %s cand/s)%s\n",
               traceColor(kv.first), kv.first, fmtNum(kv.second).c_str(), share,
               fmtNum((long long)throughput[kv.first]).c_str(), "\033[0m");
    }

    if (args.calibrate && singleCoreRate > 0) {
        double seqEstimate = total / singleCoreRate;
        double speedup = seqEstimate / std::max(1e-6, wallSecs);
        double efficiency = numWorkers > 0 ? speedup / numWorkers : 0.0;
        std::cout << "\n--- parallel/distributed computing metrics ---\n";
        std::cout << "est. sequential time (T_seq)  : " << seqEstimate << " s"
                  << "  (extrapolated from single-core calibration, not a full run)\n";
        std::cout << "speedup  (T_seq / T_parallel)  : " << speedup << "x\n";
        std::cout << "efficiency (speedup / workers)  : " << (efficiency * 100.0) << "%\n";
        std::cout << "  note: for the rigorous ground-truth number, run ./sequential on the\n";
        std::cout << "  same dict/targets and compare its wall time directly (see README / run_experiments.sh).\n";
    }

    std::cout << "\n--- matches ---\n";
    for (auto& kv : foundMatches)
        std::cout << "  " << kv.first << "  ->  " << kv.second << "\n";
    std::cout << "\033[1m======================================\033[0m\n";
}

// ======================================================================
// WORKER
// ======================================================================
static void runWorker(const Args& args, const std::vector<std::string>& dict,
                       const std::unordered_set<std::string>& targets, Algo algo,
                       int rank, double mpiT0) {
    while (true) {
        if (args.trace) traceLine(mpiT0, rank, "\u2500\u2500READY\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u25b6 MASTER   (requesting work)");
        MPI_Send(nullptr, 0, MPI_INT, 0, TAG_READY, MPI_COMM_WORLD);

        MPI_Status status;
        MPI_Probe(0, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

        if (status.MPI_TAG == TAG_TERMINATE) {
            MPI_Recv(nullptr, 0, MPI_BYTE, 0, TAG_TERMINATE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            break;
        }

        Task task;
        MPI_Recv(&task, sizeof(Task), MPI_BYTE, 0, TAG_TASK, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        // DEMO HOOK: artificially stall this one rank on its first task so you can
        // show the master detecting an overdue chunk and reassigning it live,
        // e.g. --stall-rank 2 --stall-seconds 8
        if (rank == args.stallRank) {
            std::cout << "[WORKER " << rank << "] (demo) artificially stalling for "
                      << args.stallSeconds << "s on task " << task.task_id << "\n";
            std::this_thread::sleep_for(std::chrono::duration<double>(args.stallSeconds));
        }

        // Intentionally NOT early-exiting on the first match within a chunk:
        // with multiple target hashes, more than one target can legitimately
        // fall inside the same chunk, and stopping early would silently miss
        // the others. All candidates in [start, end) are scanned; matches
        // are collected under a critical section (rare event, negligible
        // contention cost).
        std::vector<std::pair<std::string,std::string>> localFinds; // (candidate, hash)

        #pragma omp parallel for schedule(dynamic, 256)
        for (long long i = task.start; i < task.end; i++) {
            std::string cand = candidateAt(dict, i);
            std::string h = hashCandidate(cand, args.salt, algo);
            if (targets.count(h)) {
                #pragma omp critical
                { localFinds.push_back({cand, h}); }
            }
        }

        ResultMsg res{};
        res.task_id = task.task_id;
        res.processed = task.end - task.start;
        res.foundCount = std::min((int)localFinds.size(), MAX_FOUND_PER_CHUNK);
        for (int k = 0; k < res.foundCount; k++) {
            strncpy(res.founds[k].candidate, localFinds[k].first.c_str(), sizeof(res.founds[k].candidate) - 1);
            strncpy(res.founds[k].matchedHash, localFinds[k].second.c_str(), sizeof(res.founds[k].matchedHash) - 1);
        }
        MPI_Send(&res, sizeof(ResultMsg), MPI_BYTE, 0, TAG_RESULT, MPI_COMM_WORLD);
    }
}

// ======================================================================
int main(int argc, char** argv) {
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided);

    int rank, worldSize;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &worldSize);

    Args args = parseArgs(argc, argv);
    Algo algo = algoFromString(args.algoStr);

    if (worldSize < 2) {
        if (rank == 0) std::cerr << "Need at least 2 MPI ranks (1 master + >=1 worker)\n";
        MPI_Finalize();
        return 1;
    }

    // Shared "time zero" so every rank's trace timestamps line up on one timeline.
    double mpiT0 = MPI_Wtime();
    MPI_Bcast(&mpiT0, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);

    std::vector<std::string> dict = loadLines(args.dictPath);
    std::vector<std::string> targetLines = loadLines(args.targetsPath);
    std::unordered_set<std::string> targets(targetLines.begin(), targetLines.end());

    if (dict.empty() || targets.empty()) {
        if (rank == 0) std::cerr << "Dictionary or targets file empty/missing\n";
        MPI_Finalize();
        return 1;
    }

    if (rank == 0) {
        std::cout << "Dictionary words: " << dict.size()
                  << "  | variants/word: " << VARIANTS_PER_WORD
                  << "  | total search space: " << totalCandidates(dict.size()) << "\n";
        runMaster(args, dict, targets, algo, worldSize, mpiT0);
    } else {
        runWorker(args, dict, targets, algo, rank, mpiT0);
    }

    MPI_Finalize();
    return 0;
}
