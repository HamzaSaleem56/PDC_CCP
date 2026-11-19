// common.hpp
// Shared types used by master, worker, and the sequential baseline.
#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <openssl/evp.h>
#include <mpi.h>   // needed here too for MPI_Wtime(), used by the trace helpers below;
                    // harmless for sequential.cpp, which includes this header but never
                    // calls MPI_Init and so never actually uses MPI at runtime.

// ---------------------------------------------------------------------
// Hashing
// ---------------------------------------------------------------------
enum class Algo { MD5, SHA256 };

inline Algo algoFromString(const std::string& s) {
    if (s == "sha256") return Algo::SHA256;
    return Algo::MD5;
}

// Returns lowercase hex digest of (salt + plaintext)
inline std::string hashCandidate(const std::string& plaintext,
                                  const std::string& salt,
                                  Algo algo) {
    std::string input = salt + plaintext;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;

    const EVP_MD* md = (algo == Algo::MD5) ? EVP_md5() : EVP_sha256();
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, md, nullptr);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    EVP_DigestFinal_ex(ctx, digest, &digestLen);
    EVP_MD_CTX_free(ctx);

    static const char* hexChars = "0123456789abcdef";
    std::string out;
    out.resize(digestLen * 2);
    for (unsigned int i = 0; i < digestLen; i++) {
        out[2 * i]     = hexChars[(digest[i] >> 4) & 0xF];
        out[2 * i + 1] = hexChars[digest[i] & 0xF];
    }
    return out;
}

// ---------------------------------------------------------------------
// Rule-based mutation generator
//
// Instead of just hashing the raw dictionary word, every word is expanded
// into VARIANTS_PER_WORD deterministic variants (case toggle x leet-speak
// x numeric suffix). This turns a flat dictionary attack into a
// combinatorial rule-based attack while staying index-addressable, which
// is required so the master can hand out ranges of a flat [0, total)
// index space instead of literal word lists.
// ---------------------------------------------------------------------
constexpr int SUFFIX_COUNT = 101;              // 0 = no suffix, 1..100 = "00".."99"
constexpr int VARIANTS_PER_WORD = 2 /*case*/ * 2 /*leet*/ * SUFFIX_COUNT; // 404

inline std::string applyLeet(const std::string& w) {
    std::string out = w;
    for (char& c : out) {
        char lc = (char)tolower((unsigned char)c);
        switch (lc) {
            case 'a': c = '4'; break;
            case 'e': c = '3'; break;
            case 'i': c = '1'; break;
            case 'o': c = '0'; break;
            case 's': c = '5'; break;
            default: break;
        }
    }
    return out;
}

inline std::string toggleFirstCase(const std::string& w) {
    if (w.empty()) return w;
    std::string out = w;
    out[0] = isupper((unsigned char)out[0]) ? (char)tolower((unsigned char)out[0])
                                             : (char)toupper((unsigned char)out[0]);
    return out;
}

// variantIdx in [0, VARIANTS_PER_WORD)
inline std::string buildCandidate(const std::string& baseWord, long long variantIdx) {
    int suffixIdx = (int)(variantIdx % SUFFIX_COUNT);
    long long rem = variantIdx / SUFFIX_COUNT;
    int leetFlag = (int)(rem % 2);
    int caseFlag = (int)(rem / 2);

    std::string w = baseWord;
    if (caseFlag) w = toggleFirstCase(w);
    if (leetFlag) w = applyLeet(w);
    if (suffixIdx > 0) {
        int n = suffixIdx - 1; // 0..99
        char buf[4];
        snprintf(buf, sizeof(buf), "%02d", n);
        w += buf;
    }
    return w;
}

// Global flat index -> candidate string, given the dictionary.
inline std::string candidateAt(const std::vector<std::string>& dict, long long globalIdx) {
    long long wordIdx    = globalIdx / VARIANTS_PER_WORD;
    long long variantIdx = globalIdx % VARIANTS_PER_WORD;
    return buildCandidate(dict[(size_t)wordIdx], variantIdx);
}

inline long long totalCandidates(size_t dictSize) {
    return (long long)dictSize * VARIANTS_PER_WORD;
}

// ---------------------------------------------------------------------
// MPI message protocol
// ---------------------------------------------------------------------
enum Tag {
    TAG_READY           = 1, // worker -> master : "I'm idle, give me work"
    TAG_TASK            = 2, // master -> worker : Task struct
    TAG_TERMINATE       = 3, // master -> worker : empty payload
    TAG_RESULT          = 4, // worker -> master : ResultMsg struct
    TAG_ABORT_NOTICE    = 5  // master -> worker : empty payload, stop searching soon
};

struct Task {
    long long task_id;
    long long start;   // inclusive
    long long end;      // exclusive
};

constexpr int MAX_FOUND_PER_CHUNK = 8;

struct FoundEntry {
    char candidate[160];
    char matchedHash[80];
};

struct ResultMsg {
    long long task_id;
    long long processed;      // how many candidates this chunk covered
    int foundCount;            // number of valid entries in founds[] (0..MAX_FOUND_PER_CHUNK)
    FoundEntry founds[MAX_FOUND_PER_CHUNK];
};

// ---------------------------------------------------------------------
// Live message trace: makes the MPI request/response cycle between
// master and workers visible on the terminal, like a scrolling sequence
// diagram. Every line is colour-coded by rank and timestamped relative
// to a shared t0 (broadcast from rank 0 right after MPI_Init so all
// ranks agree on "time zero").
// ---------------------------------------------------------------------
#include <cstdio>
#include <string>

inline const char* traceColor(int rank) {
    if (rank == 0) return "\033[1;36m"; // bold cyan for MASTER
    static const char* palette[] = {
        "\033[1;33m", "\033[1;32m", "\033[1;35m", "\033[1;34m", "\033[1;31m", "\033[1;37m"
    };
    return palette[(rank - 1) % 6];
}

inline std::string traceLabel(int rank) {
    return rank == 0 ? std::string("MASTER") : ("WORKER-" + std::to_string(rank));
}

// t0: shared reference time (seconds, from MPI_Wtime()), same value on every rank.
inline void traceLine(double t0, int rank, const std::string& msg) {
    double t = MPI_Wtime() - t0;
    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "%7.3f", t);
    std::string label = traceLabel(rank);
    // pad label to 9 chars so arrows line up in a column
    while (label.size() < 9) label += ' ';
    printf("%s[t=%ss] %s\033[0m %s\n", traceColor(rank), tbuf, label.c_str(), msg.c_str());
    fflush(stdout);
}
