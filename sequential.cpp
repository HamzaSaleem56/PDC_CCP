// sequential.cpp
// Single-threaded, single-process baseline. Used to compute speedup and
// efficiency against the MPI+OpenMP version: speedup = T_seq / T_parallel,
// efficiency = speedup / num_worker_processes.
#include <iostream>
#include <fstream>
#include <vector>
#include <unordered_set>
#include <chrono>
#include "common.hpp"

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

int main(int argc, char** argv) {
    std::string dictPath, targetsPath, algoStr = "md5", salt = "";
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        auto next = [&](){ return std::string(argv[++i]); };
        if (s == "--dict") dictPath = next();
        else if (s == "--targets") targetsPath = next();
        else if (s == "--algo") algoStr = next();
        else if (s == "--salt") salt = next();
    }

    std::vector<std::string> dict = loadLines(dictPath);
    auto targetLines = loadLines(targetsPath);
    std::unordered_set<std::string> targets(targetLines.begin(), targetLines.end());
    Algo algo = algoFromString(algoStr);

    long long total = totalCandidates(dict.size());
    std::cout << "[SEQUENTIAL] search space: " << total << " candidates\n";

    auto t0 = std::chrono::steady_clock::now();
    size_t foundCount = 0;
    for (long long i = 0; i < total && foundCount < targets.size(); i++) {
        std::string cand = candidateAt(dict, i);
        std::string h = hashCandidate(cand, salt, algo);
        if (targets.count(h)) {
            std::cout << "[SEQUENTIAL] MATCH hash=" << h << " candidate=" << cand << "\n";
            foundCount++;
        }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::cout << "===== RUN SUMMARY (sequential) =====\n";
    std::cout << "targets found : " << foundCount << " / " << targets.size() << "\n";
    std::cout << "wall time     : " << secs << " s\n";
    std::cout << "=====================================\n";
    return 0;
}
