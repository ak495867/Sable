#ifndef SABLE_HPP
#define SABLE_HPP

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <memory>
#include <cstdint>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <functional>
#include <sqlite3.h>

namespace sable {

using Row = std::unordered_map<std::string, std::string>;

inline std::string json_escape(const std::string& str) {
    std::string out;
    out.reserve(str.size() + 16);
    for (char c : str) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

inline std::string hash_fingerprint(const std::string& sql) {
    uint64_t hash = 14695981039346656037ULL;
    for (char c : sql) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            char lower = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
            hash ^= static_cast<uint64_t>(lower);
            hash *= 1099511628211ULL;
        }
    }
    std::stringstream ss;
    ss << std::hex << hash;
    return ss.str();
}

struct Telemetry {
    int64_t query_id = 0;
    std::string fingerprint;
    std::string plan;
    int64_t rows_scanned = 0;
    int64_t rows_returned = 0;
    int64_t latency_us = 0;
    bool index_used = false;
    std::string phase = "startup";
    int64_t epoch = 0;
    std::string operation = "query";
    bool cache_hit = false;
    int64_t bytes_read = 0;
    int64_t bytes_written = 0;
    std::vector<std::string> candidate_plans;
    std::vector<std::string> sqlite_plan;
    std::string index_name;

    std::string to_json() const;
};

class WorkloadModel {
public:
    int64_t total = 0;
    int64_t point = 0;
    int64_t scan = 0;
    int64_t aggregate = 0;
    int64_t join = 0;
    int64_t write = 0;
    int window = 100;
    std::vector<int64_t> latencies;
    std::deque<std::string> recent;
    std::unordered_map<std::string, int> recent_counts;
    std::unordered_map<std::string, int64_t> fingerprints;
    std::unordered_map<std::string, int64_t> plan_counts;
    std::unordered_map<std::string, std::vector<int64_t>> plan_latency;

    explicit WorkloadModel(int win = 100);
    void observe(const std::string& fp, const std::string& plan, int64_t latency_us,
                 int64_t rows_scanned, int64_t rows_returned, const std::string& op = "query");
    std::string phase() const;
    double p50_latency() const;
    double p95_latency() const;
    std::string snapshot_json() const;
};

class CostModel {
public:
    int window = 1000;
    int version = 0;
    int dirty = 0;
    double base = 50.0;
    double per_row = 0.01;
    double index_penalty = 0.2;
    double join_multiplier = 3.0;
    double aggregate_multiplier = 1.5;

    struct Sample {
        int64_t rows_scanned;
        int64_t latency_us;
        bool index_used;
        bool join;
        bool aggregate;
    };
    std::vector<Sample> samples;

    explicit CostModel(int win = 1000);
    void observe(int64_t rows_scanned, int64_t latency_us, bool index_used, bool join, bool aggregate);
    void fit();
    double estimate(int64_t rows_scanned, bool index_used = false, bool join = false, bool aggregate = false) const;
};

class AdaptivePlanner {
public:
    CostModel* cost_model = nullptr;
    std::string last_plan = "scan";
    int fit_version = -1;
    std::unordered_map<std::string, std::tuple<std::string, std::vector<std::string>, bool>> plan_cache;

    explicit AdaptivePlanner(CostModel* cm);
    std::string select(const std::string& sql, int64_t estimated_rows,
                       const std::unordered_set<std::string>& indexes,
                       std::vector<std::string>& out_candidates, bool& cache_hit);
};

class LinUCBBandit {
public:
    std::vector<std::string> arms;
    double alpha = 0.5;
    std::unordered_map<std::string, std::vector<std::vector<double>>> A;
    std::unordered_map<std::string, std::vector<double>> b;

    explicit LinUCBBandit(const std::vector<std::string>& arm_names = {"nested_loop", "hash", "merge"}, double a = 0.5);
    std::string select(const std::vector<double>& ctx);
    void update(const std::string& arm, const std::vector<double>& ctx, double reward);
    std::string status_json() const;
};

class ShadowValidator {
public:
    struct ValidationResult {
        bool validated = false;
        double t_stat = 0.0;
        double p_value = 1.0;
        double baseline_mean = 0.0;
        double candidate_mean = 0.0;
        double speedup = 1.0;
    };

    static ValidationResult compare(const std::vector<double>& baseline_samples,
                                    const std::vector<double>& candidate_samples);
};

struct Proposal {
    std::string type;
    std::string reason;
    std::string details;
    std::string status = "proposed";
    int64_t epoch = 0;
};

class SableDB {
private:
    std::string path_;
    sqlite3* db_ = nullptr;
    int epoch_size_ = 100;
    int64_t query_id_ = 0;
    int64_t epoch_ = 0;
    std::string last_phase_ = "startup";
    std::string policy_mode_ = "heuristic";
    std::unordered_set<std::string> indexes_;
    std::vector<Proposal> proposals_;
    std::vector<Proposal> journal_;
    std::vector<Telemetry> telemetry_;
    std::unordered_map<std::string, std::string> plan_hint_cache_;
    std::unordered_map<std::string, std::tuple<std::vector<std::string>, bool, std::string>> plan_cache_;

    void init_tables();
    void persist_state(bool force = false);
    std::tuple<std::vector<std::string>, bool, std::string> inspect_sqlite_plan(const std::string& sql);
    std::string apply_plan_hint(const std::string& sql, const std::string& plan);

public:
    WorkloadModel model;
    CostModel cost_model;
    std::unique_ptr<AdaptivePlanner> planner;
    LinUCBBandit join_bandit;

    explicit SableDB(const std::string& db_path = ":memory:", int epoch_size = 100);
    ~SableDB();

    bool put(const std::string& key, const std::string& value);
    std::string get(const std::string& key);
    int bulk_put(const std::vector<std::pair<std::string, std::string>>& items);
    std::vector<Row> execute(const std::string& sql, const std::vector<std::string>& params = {});

    void record_telemetry(Telemetry t);
    void adapt();
    Proposal approve_proposal();
    bool rollback_last_adaptation();
    void enable_policy(const std::string& policy);
    int archive_cold_records(int limit = 1000);

    std::string status_json() const;
    void export_telemetry(const std::string& filepath);
    void close();
};

}

#endif
