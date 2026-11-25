#include "sable.hpp"
#include <fstream>
#include <iostream>
#include <cmath>

namespace sable {

std::string Telemetry::to_json() const {
    std::ostringstream ss;
    ss << "{"
       << "\"query_id\":" << query_id << ","
       << "\"fingerprint\":\"" << json_escape(fingerprint) << "\","
       << "\"plan\":\"" << json_escape(plan) << "\","
       << "\"rows_scanned\":" << rows_scanned << ","
       << "\"rows_returned\":" << rows_returned << ","
       << "\"latency_us\":" << latency_us << ","
       << "\"index_used\":" << (index_used ? "true" : "false") << ","
       << "\"phase\":\"" << json_escape(phase) << "\","
       << "\"epoch\":" << epoch << ","
       << "\"operation\":\"" << json_escape(operation) << "\","
       << "\"cache_hit\":" << (cache_hit ? "true" : "false") << ","
       << "\"bytes_read\":" << bytes_read << ","
       << "\"bytes_written\":" << bytes_written << ","
       << "\"candidate_plans\":[";
    for (size_t i = 0; i < candidate_plans.size(); ++i) {
        ss << "\"" << json_escape(candidate_plans[i]) << "\"";
        if (i + 1 < candidate_plans.size()) ss << ",";
    }
    ss << "],\"sqlite_plan\":[";
    for (size_t i = 0; i < sqlite_plan.size(); ++i) {
        ss << "\"" << json_escape(sqlite_plan[i]) << "\"";
        if (i + 1 < sqlite_plan.size()) ss << ",";
    }
    ss << "],\"index_name\":\"" << json_escape(index_name) << "\"}";
    return ss.str();
}

WorkloadModel::WorkloadModel(int win) : window(win) {}

void WorkloadModel::observe(const std::string& fp, const std::string& plan, int64_t latency_us,
                            int64_t rows_scanned, int64_t rows_returned, const std::string& op) {
    total++;
    latencies.push_back(latency_us);
    if (latencies.size() > 12000) {
        latencies.erase(latencies.begin(), latencies.begin() + 2000);
    }
    if (static_cast<int>(recent.size()) >= window) {
        std::string old = recent.front();
        recent.pop_front();
        if (--recent_counts[old] <= 0) {
            recent_counts.erase(old);
        }
    }
    recent.push_back(plan);
    recent_counts[plan]++;
    fingerprints[fp]++;
    plan_counts[plan]++;
    auto& pl = plan_latency[plan];
    pl.push_back(latency_us);
    if (pl.size() > 1200) {
        pl.erase(pl.begin(), pl.begin() + 200);
    }
    if (plan == "index_lookup" || plan == "point_lookup") point++;
    else if (plan == "scan") scan++;
    else if (plan == "aggregate") aggregate++;
    else if (plan == "join") join++;
    if (op == "insert" || op == "update" || op == "delete") write++;
}

std::string WorkloadModel::phase() const {
    if (recent_counts.empty()) return "startup";
    std::string dominant = "point_lookup";
    int max_cnt = -1;
    for (const auto& kv : recent_counts) {
        if (kv.second > max_cnt) {
            max_cnt = kv.second;
            dominant = kv.first;
        }
    }
    if (dominant == "point_lookup" || dominant == "index_lookup") return "point_lookups";
    if (dominant == "scan") return "scans";
    if (dominant == "aggregate") return "analytics";
    if (dominant == "join") return "joins";
    return (write > total / 2) ? "writes" : "mixed";
}

double WorkloadModel::p50_latency() const {
    if (latencies.empty()) return 0.0;
    std::vector<int64_t> s = latencies;
    std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
    return static_cast<double>(s[s.size() / 2]);
}

double WorkloadModel::p95_latency() const {
    if (latencies.empty()) return 0.0;
    std::vector<int64_t> s = latencies;
    size_t idx = static_cast<size_t>(s.size() * 0.95);
    if (idx >= s.size()) idx = s.size() - 1;
    std::nth_element(s.begin(), s.begin() + idx, s.end());
    return static_cast<double>(s[idx]);
}

std::string WorkloadModel::snapshot_json() const {
    double n = std::max<int64_t>(total, 1);
    double rn = std::max<size_t>(recent.size(), 1);
    double pt_ratio = 0.0;
    auto it_pt = recent_counts.find("point_lookup");
    if (it_pt != recent_counts.end()) pt_ratio += it_pt->second;
    auto it_idx = recent_counts.find("index_lookup");
    if (it_idx != recent_counts.end()) pt_ratio += it_idx->second;
    pt_ratio /= rn;

    double sc_ratio = 0.0;
    auto it_sc = recent_counts.find("scan");
    if (it_sc != recent_counts.end()) sc_ratio = it_sc->second / rn;

    double wr_ratio = 0.0;
    auto it_wr = recent_counts.find("write");
    if (it_wr != recent_counts.end()) wr_ratio = it_wr->second / rn;

    std::ostringstream ss;
    ss << "{"
       << "\"queries\":" << total << ","
       << "\"point_lookup_ratio\":" << (point / n) << ","
       << "\"scan_ratio\":" << (scan / n) << ","
       << "\"aggregate_ratio\":" << (aggregate / n) << ","
       << "\"join_ratio\":" << (join / n) << ","
       << "\"write_ratio\":" << (write / n) << ","
       << "\"recent_point_lookup_ratio\":" << pt_ratio << ","
       << "\"recent_scan_ratio\":" << sc_ratio << ","
       << "\"recent_write_ratio\":" << wr_ratio << ","
       << "\"phase\":\"" << phase() << "\","
       << "\"p50_latency_us\":" << p50_latency() << ","
       << "\"p95_latency_us\":" << p95_latency()
       << "}";
    return ss.str();
}

CostModel::CostModel(int win) : window(win) {}

void CostModel::observe(int64_t rows_scanned, int64_t latency_us, bool index_used, bool join, bool aggregate) {
    samples.push_back({rows_scanned, latency_us, index_used, join, aggregate});
    if (samples.size() > static_cast<size_t>(window + 200)) {
        samples.erase(samples.begin(), samples.begin() + 200);
    }
    dirty++;
    if (dirty >= 100 || samples.size() <= 20) {
        fit();
        dirty = 0;
    }
}

void CostModel::fit() {
    if (samples.size() < 3) return;
    double n = static_cast<double>(samples.size());
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    double idx_sum = 0.0, non_idx_sum = 0.0, join_sum = 0.0;
    int idx_cnt = 0, non_idx_cnt = 0, join_cnt = 0;

    for (const auto& s : samples) {
        double rx = static_cast<double>(s.rows_scanned);
        double ly = static_cast<double>(s.latency_us);
        sx += rx;
        sy += ly;
        sxx += rx * rx;
        sxy += rx * ly;
        if (s.index_used) {
            idx_sum += ly;
            idx_cnt++;
        } else if (!s.join) {
            non_idx_sum += ly;
            non_idx_cnt++;
        }
        if (s.join) {
            join_sum += ly;
            join_cnt++;
        }
    }
    double denom = n * sxx - sx * sx;
    if (denom != 0.0) {
        double slope = (n * sxy - sx * sy) / denom;
        per_row = std::max(0.001, std::min(slope, 1.0));
    }
    base = std::max(1.0, sy / n - per_row * sx / n);
    if (idx_cnt > 0 && non_idx_cnt > 0) {
        double non_idx_mean = non_idx_sum / non_idx_cnt;
        if (non_idx_mean > 0.0) {
            index_penalty = std::max(0.01, std::min(0.9, (idx_sum / idx_cnt) / non_idx_mean));
        }
    }
    if (join_cnt > 0 && non_idx_cnt > 0) {
        double non_idx_mean = non_idx_sum / non_idx_cnt;
        if (non_idx_mean > 0.0) {
            join_multiplier = std::max(1.1, std::min(10.0, (join_sum / join_cnt) / non_idx_mean));
        }
    }
    version++;
}

double CostModel::estimate(int64_t rows_scanned, bool index_used, bool join, bool aggregate) const {
    double cost = base + per_row * static_cast<double>(rows_scanned);
    if (index_used) cost *= index_penalty;
    if (join) cost *= join_multiplier;
    if (aggregate) cost *= aggregate_multiplier;
    return std::max(1.0, cost);
}

AdaptivePlanner::AdaptivePlanner(CostModel* cm) : cost_model(cm) {}

std::string AdaptivePlanner::select(const std::string& sql, int64_t estimated_rows,
                                    const std::unordered_set<std::string>& indexes,
                                    std::vector<std::string>& out_candidates, bool& cache_hit) {
    if (cost_model && cost_model->version != fit_version) {
        plan_cache.clear();
        fit_version = cost_model->version;
    }
    std::string key = sql + ":" + (indexes.count("idx_sable_key") ? "1" : "0");
    auto it = plan_cache.find(key);
    if (it != plan_cache.end()) {
        cache_hit = true;
        last_plan = std::get<0>(it->second);
        out_candidates = std::get<1>(it->second);
        return last_plan;
    }
    cache_hit = false;
    std::string low = sql;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return std::tolower(c); });

    std::vector<std::string> plans;
    if (low.find(" join ") != std::string::npos) {
        plans = {"join", "scan"};
    } else if (low.find("count(") != std::string::npos || low.find("sum(") != std::string::npos ||
               low.find("avg(") != std::string::npos || low.find("group by") != std::string::npos) {
        plans = {"aggregate", "scan"};
    } else if (low.find(" where ") != std::string::npos && low.find("=") != std::string::npos) {
        if (indexes.count("idx_sable_key")) {
            plans = {"index_lookup", "point_lookup", "scan"};
        } else {
            plans = {"point_lookup", "scan"};
        }
    } else {
        plans = {"scan"};
    }
    out_candidates = plans;
    double best_cost = 1e18;
    std::string best_plan = plans[0];
    for (const auto& p : plans) {
        double c = 0.0;
        if (p == "scan") c = cost_model ? cost_model->estimate(estimated_rows) : 100.0;
        else if (p == "point_lookup") c = cost_model ? cost_model->estimate(estimated_rows, false) * 0.5 : 50.0;
        else if (p == "index_lookup") c = cost_model ? cost_model->estimate(estimated_rows, true) : 20.0;
        else if (p == "join") c = cost_model ? cost_model->estimate(estimated_rows, false, true) : 300.0;
        else if (p == "aggregate") c = cost_model ? cost_model->estimate(estimated_rows, false, false, true) : 150.0;
        if (c < best_cost) {
            best_cost = c;
            best_plan = p;
        }
    }
    last_plan = best_plan;
    if (plan_cache.size() > 2048) plan_cache.clear();
    plan_cache[key] = {last_plan, out_candidates, true};
    return last_plan;
}

LinUCBBandit::LinUCBBandit(const std::vector<std::string>& arm_names, double a)
    : arms(arm_names), alpha(a) {
    for (const auto& arm : arms) {
        A[arm] = {
            {1.0, 0.0, 0.0},
            {0.0, 1.0, 0.0},
            {0.0, 0.0, 1.0}
        };
        b[arm] = {0.0, 0.0, 0.0};
    }
}

static std::vector<std::vector<double>> invert3x3(const std::vector<std::vector<double>>& m) {
    double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
               - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
               + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::abs(det) < 1e-12) det = 1e-12;
    double inv = 1.0 / det;
    std::vector<std::vector<double>> res(3, std::vector<double>(3, 0.0));
    res[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv;
    res[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv;
    res[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv;
    res[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv;
    res[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv;
    res[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv;
    res[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv;
    res[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv;
    res[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv;
    return res;
}

std::string LinUCBBandit::select(const std::vector<double>& ctx) {
    std::string best_arm = arms[0];
    double max_ucb = -1e18;
    for (const auto& arm : arms) {
        auto invA = invert3x3(A[arm]);
        const auto& bar = b[arm];
        std::vector<double> theta(3, 0.0);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                theta[i] += invA[i][j] * bar[j];
            }
        }
        double est = 0.0;
        for (int i = 0; i < 3; ++i) est += ctx[i] * theta[i];
        std::vector<double> v(3, 0.0);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                v[i] += invA[i][j] * ctx[j];
            }
        }
        double quad = 0.0;
        for (int i = 0; i < 3; ++i) quad += ctx[i] * v[i];
        double ucb = est + alpha * std::sqrt(std::max(0.0, quad));
        if (ucb > max_ucb) {
            max_ucb = ucb;
            best_arm = arm;
        }
    }
    return best_arm;
}

void LinUCBBandit::update(const std::string& arm, const std::vector<double>& ctx, double reward) {
    auto& mat = A[arm];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            mat[i][j] += ctx[i] * ctx[j];
        }
    }
    auto& vec = b[arm];
    for (int i = 0; i < 3; ++i) {
        vec[i] += reward * ctx[i];
    }
}

std::string LinUCBBandit::status_json() const {
    std::ostringstream ss;
    ss << "{\"arms\":[";
    for (size_t i = 0; i < arms.size(); ++i) {
        ss << "\"" << arms[i] << "\"";
        if (i + 1 < arms.size()) ss << ",";
    }
    ss << "],\"alpha\":" << alpha << "}";
    return ss.str();
}

ShadowValidator::ValidationResult ShadowValidator::compare(
    const std::vector<double>& baseline, const std::vector<double>& candidate) {
    ValidationResult res;
    if (baseline.empty() || candidate.empty()) return res;
    double b_sum = 0.0;
    for (double v : baseline) b_sum += v;
    res.baseline_mean = b_sum / baseline.size();

    double c_sum = 0.0;
    for (double v : candidate) c_sum += v;
    res.candidate_mean = c_sum / candidate.size();

    double b_var = 0.0;
    for (double v : baseline) b_var += (v - res.baseline_mean) * (v - res.baseline_mean);
    b_var = baseline.size() > 1 ? b_var / (baseline.size() - 1) : 0.0;

    double c_var = 0.0;
    for (double v : candidate) c_var += (v - res.candidate_mean) * (v - res.candidate_mean);
    c_var = candidate.size() > 1 ? c_var / (candidate.size() - 1) : 0.0;

    double se = std::sqrt(b_var / baseline.size() + c_var / candidate.size());
    if (se > 1e-9) {
        res.t_stat = (res.baseline_mean - res.candidate_mean) / se;
        res.p_value = 0.5 * std::erfc(res.t_stat / std::sqrt(2.0));
    } else {
        res.t_stat = 0.0;
        res.p_value = (res.candidate_mean < res.baseline_mean) ? 0.0 : 1.0;
    }
    res.speedup = res.candidate_mean > 0.0 ? res.baseline_mean / res.candidate_mean : 1.0;
    res.validated = (res.t_stat > 1.645 && res.p_value < 0.05);
    return res;
}

SableDB::SableDB(const std::string& db_path, int epoch_size)
    : path_(db_path), epoch_size_(epoch_size) {
    planner = std::make_unique<AdaptivePlanner>(&cost_model);
    int rc = sqlite3_open(path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        if (db_) sqlite3_close(db_);
        throw std::runtime_error("cannot open sqlite database");
    }
    init_tables();
}

SableDB::~SableDB() {
    close();
}

void SableDB::init_tables() {
    char* err = nullptr;
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, &err);
    sqlite3_exec(db_, "PRAGMA temp_store=MEMORY;", nullptr, nullptr, &err);
    sqlite3_exec(db_, "PRAGMA cache_size=-64000;", nullptr, nullptr, &err);
    sqlite3_exec(db_, "PRAGMA busy_timeout=10000;", nullptr, nullptr, &err);
    sqlite3_exec(db_,
        "CREATE TABLE IF NOT EXISTS sable_records (id INTEGER PRIMARY KEY, key TEXT UNIQUE, value TEXT, created_at REAL);"
        "CREATE TABLE IF NOT EXISTS _sable_state (state_key TEXT PRIMARY KEY, state_json TEXT NOT NULL, updated_at REAL NOT NULL);",
        nullptr, nullptr, &err);
    if (err) sqlite3_free(err);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT 1 FROM sqlite_master WHERE type='index' AND name='idx_sable_key';", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            indexes_.insert("idx_sable_key");
        }
        sqlite3_finalize(stmt);
    }
}

void SableDB::close() {
    if (db_) {
        persist_state(true);
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void SableDB::persist_state(bool force) {
    if (!db_ || sqlite3_get_autocommit(db_) == 0) return;
    if (!force && (model.total % epoch_size_ != 0)) return;

    std::string payload = status_json();
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO _sable_state(state_key, state_json, updated_at) VALUES('adaptive', ?, ?) "
                      "ON CONFLICT(state_key) DO UPDATE SET state_json=excluded.state_json, updated_at=excluded.updated_at;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, payload.c_str(), -1, SQLITE_TRANSIENT);
        double now = static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        sqlite3_bind_double(stmt, 2, now);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::tuple<std::vector<std::string>, bool, std::string> SableDB::inspect_sqlite_plan(const std::string& sql) {
    std::string key = sql + ":" + (indexes_.count("idx_sable_key") ? "1" : "0");
    auto it = plan_cache_.find(key);
    if (it != plan_cache_.end()) return it->second;

    std::vector<std::string> lines;
    sqlite3_stmt* stmt = nullptr;
    std::string explain = "EXPLAIN QUERY PLAN " + sql;
    if (sqlite3_prepare_v2(db_, explain.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* detail = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            if (detail) lines.push_back(detail);
        }
        sqlite3_finalize(stmt);
    }
    bool is_idx = false;
    std::string idx_name;
    for (const auto& l : lines) {
        std::string up = l;
        std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return std::toupper(c); });
        if (up.find("USING INDEX") != std::string::npos || up.find("USING INTEGER PRIMARY KEY") != std::string::npos) {
            is_idx = true;
            if (up.find("IDX_SABLE_KEY") != std::string::npos) {
                idx_name = "idx_sable_key";
            }
        }
    }
    auto entry = std::make_tuple(lines, is_idx, idx_name);
    if (plan_cache_.size() > 2048) plan_cache_.clear();
    plan_cache_[key] = entry;
    return entry;
}

std::string SableDB::apply_plan_hint(const std::string& sql, const std::string& plan) {
    std::string key = sql + ":" + plan + ":" + (indexes_.count("idx_sable_key") ? "1" : "0");
    auto it = plan_hint_cache_.find(key);
    if (it != plan_hint_cache_.end()) return it->second;

    std::string low = sql;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return std::tolower(c); });
    std::string res = sql;
    if (low.find("select") == 0 && low.find("sable_records") != std::string::npos &&
        low.find("indexed by") == std::string::npos && low.find("not indexed") == std::string::npos) {
        size_t pos = low.find("from sable_records");
        if (pos != std::string::npos) {
            if (plan == "index_lookup" && indexes_.count("idx_sable_key") && low.find("where") != std::string::npos && low.find("key") != std::string::npos) {
                res = sql.substr(0, pos) + "FROM sable_records INDEXED BY idx_sable_key" + sql.substr(pos + 18);
            } else if (plan == "scan" && indexes_.count("idx_sable_key") && low.find("where") != std::string::npos) {
                res = sql.substr(0, pos) + "FROM sable_records NOT INDEXED" + sql.substr(pos + 18);
            }
        }
    }
    if (plan_hint_cache_.size() > 2048) plan_hint_cache_.clear();
    plan_hint_cache_[key] = res;
    return res;
}

bool SableDB::put(const std::string& key, const std::string& value) {
    execute("INSERT INTO sable_records(key, value, created_at) VALUES(?, ?, ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
            {key, value, std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count() / 1000000.0)});
    return true;
}

std::string SableDB::get(const std::string& key) {
    auto rows = execute("SELECT key, value, created_at FROM sable_records WHERE key = ?;", {key});
    if (!rows.empty()) {
        auto it = rows[0].find("value");
        if (it != rows[0].end()) return it->second;
    }
    return "";
}

int SableDB::bulk_put(const std::vector<std::pair<std::string, std::string>>& items) {
    auto start = std::chrono::steady_clock::now();
    sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO sable_records(key, value, created_at) VALUES(?, ?, ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    double now = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() / 1000000.0);
    for (const auto& item : items) {
        sqlite3_bind_text(stmt, 1, item.first.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, item.second.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, now);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    query_id_++;
    Telemetry t;
    t.query_id = query_id_;
    t.fingerprint = "bulk_put";
    t.plan = "write";
    t.rows_scanned = items.size();
    t.rows_returned = items.size();
    t.latency_us = std::max<int64_t>(1, elapsed);
    t.operation = "insert";
    t.bytes_written = items.size() * 64;
    record_telemetry(t);
    return static_cast<int>(items.size());
}

std::vector<Row> SableDB::execute(const std::string& sql, const std::vector<std::string>& params) {
    auto start = std::chrono::steady_clock::now();
    std::string op = "query";
    size_t first_space = sql.find_first_of(" \t\n\r");
    if (first_space != std::string::npos) {
        op = sql.substr(0, first_space);
        std::transform(op.begin(), op.end(), op.begin(), [](unsigned char c) { return std::tolower(c); });
    }

    std::string plan = "scan";
    std::vector<std::string> candidates = {"scan"};
    bool cache_hit = false;
    if (op == "insert" || op == "update" || op == "delete" || op == "replace") {
        plan = "write";
        candidates = {"write"};
    } else if (planner) {
        plan = planner->select(sql, 0, indexes_, candidates, cache_hit);
    }

    std::string hinted_sql = apply_plan_hint(sql, plan);
    auto [sq_plan, index_used, index_name] = inspect_sqlite_plan(hinted_sql);

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, hinted_sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        hinted_sql = sql;
        rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        std::tie(sq_plan, index_used, index_name) = inspect_sqlite_plan(sql);
    }
    for (size_t i = 0; i < params.size(); ++i) {
        sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<Row> rows;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        Row row;
        int cols = sqlite3_column_count(stmt);
        for (int c = 0; c < cols; ++c) {
            const char* name = sqlite3_column_name(stmt, c);
            const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, c));
            row[name ? name : ""] = val ? val : "";
        }
        rows.push_back(std::move(row));
    }
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    query_id_++;
    int64_t scanned = std::max<int64_t>(changes > 0 ? changes : rows.size(), rows.size());

    Telemetry t;
    t.query_id = query_id_;
    t.fingerprint = hash_fingerprint(sql);
    t.plan = plan;
    t.rows_scanned = scanned;
    t.rows_returned = rows.size();
    t.latency_us = std::max<int64_t>(1, elapsed);
    t.index_used = index_used;
    t.phase = last_phase_;
    t.epoch = epoch_;
    t.operation = op;
    t.cache_hit = cache_hit;
    t.bytes_read = rows.size() * 64;
    t.candidate_plans = candidates;
    t.sqlite_plan = sq_plan;
    t.index_name = index_name;

    record_telemetry(t);
    cost_model.observe(scanned, t.latency_us, index_used, plan == "join", plan == "aggregate");
    persist_state();
    return rows;
}

void SableDB::record_telemetry(Telemetry t) {
    telemetry_.push_back(t);
    if (telemetry_.size() > 10000) {
        telemetry_.erase(telemetry_.begin(), telemetry_.begin() + 5000);
    }
    model.observe(t.fingerprint, t.plan, t.latency_us, t.rows_scanned, t.rows_returned, t.operation);
    std::string phase = model.phase();
    if (phase != last_phase_ && model.total > epoch_size_) {
        Proposal p;
        p.type = "phase_change";
        p.reason = "observed phase transition from " + last_phase_ + " to " + phase;
        p.details = "{\"from\":\"" + last_phase_ + "\",\"to\":\"" + phase + "\"}";
        p.status = "observed";
        p.epoch = epoch_;
        proposals_.push_back(p);
    }
    last_phase_ = phase;
    if (model.total % epoch_size_ == 0) {
        adapt();
    }
}

void SableDB::adapt() {
    epoch_++;
    auto s = model;
    double rn = std::max<size_t>(s.recent.size(), 1);
    double pt_ratio = 0.0;
    auto it_pt = s.recent_counts.find("point_lookup");
    if (it_pt != s.recent_counts.end()) pt_ratio += it_pt->second;
    auto it_idx = s.recent_counts.find("index_lookup");
    if (it_idx != s.recent_counts.end()) pt_ratio += it_idx->second;
    pt_ratio /= rn;

    double wr_ratio = 0.0;
    auto it_wr = s.recent_counts.find("write");
    if (it_wr != s.recent_counts.end()) wr_ratio = it_wr->second / rn;

    if (pt_ratio > 0.60 && !indexes_.count("idx_sable_key")) {
        bool exists = false;
        for (const auto& p : proposals_) {
            if (p.type == "create_index" && p.status == "proposed") exists = true;
        }
        if (!exists) {
            proposals_.push_back({"create_index", "point lookup ratio exceeded 60%", "{\"index\":\"idx_sable_key\"}", "proposed", epoch_});
        }
    }
    if ((wr_ratio > 0.50 || (static_cast<double>(s.write) / std::max<int64_t>(s.total, 1)) > 0.50) &&
        indexes_.count("idx_sable_key") && pt_ratio < 0.25) {
        bool exists = false;
        for (const auto& p : proposals_) {
            if (p.type == "drop_index" && p.status == "proposed") exists = true;
        }
        if (!exists) {
            proposals_.push_back({"drop_index", "write workload exceeded 50%", "{\"index\":\"idx_sable_key\"}", "proposed", epoch_});
        }
    }
}

Proposal SableDB::approve_proposal() {
    for (auto& p : proposals_) {
        if (p.status == "proposed") {
            p.status = "committed";
            journal_.push_back(p);
            if (p.type == "create_index") {
                sqlite3_exec(db_, "CREATE INDEX IF NOT EXISTS idx_sable_key ON sable_records(key);", nullptr, nullptr, nullptr);
                indexes_.insert("idx_sable_key");
            } else if (p.type == "drop_index") {
                sqlite3_exec(db_, "DROP INDEX IF EXISTS idx_sable_key;", nullptr, nullptr, nullptr);
                indexes_.erase("idx_sable_key");
            }
            plan_cache_.clear();
            plan_hint_cache_.clear();
            return p;
        }
    }
    return Proposal{"none", "no pending proposals", "{}", "none", epoch_};
}

bool SableDB::rollback_last_adaptation() {
    if (journal_.empty()) return false;
    Proposal last = journal_.back();
    journal_.pop_back();
    if (last.type == "create_index") {
        sqlite3_exec(db_, "DROP INDEX IF EXISTS idx_sable_key;", nullptr, nullptr, nullptr);
        indexes_.erase("idx_sable_key");
    } else if (last.type == "drop_index") {
        sqlite3_exec(db_, "CREATE INDEX IF NOT EXISTS idx_sable_key ON sable_records(key);", nullptr, nullptr, nullptr);
        indexes_.insert("idx_sable_key");
    }
    plan_cache_.clear();
    plan_hint_cache_.clear();
    return true;
}

void SableDB::enable_policy(const std::string& policy) {
    policy_mode_ = policy;
}

int SableDB::archive_cold_records(int limit) {
    auto rows = execute("SELECT id, key, value, created_at FROM sable_records ORDER BY created_at ASC LIMIT " + std::to_string(limit) + ";");
    if (rows.empty()) return 0;
    std::ofstream out("cold_segments.log", std::ios::app);
    for (const auto& r : rows) {
        out << r.at("key") << "\t" << r.at("value") << "\n";
    }
    execute("DELETE FROM sable_records WHERE id IN (SELECT id FROM sable_records ORDER BY created_at ASC LIMIT " + std::to_string(limit) + ");");
    return static_cast<int>(rows.size());
}

std::string SableDB::status_json() const {
    std::ostringstream ss;
    ss << "{"
       << "\"epoch\":" << epoch_ << ","
       << "\"policy_mode\":\"" << policy_mode_ << "\","
       << "\"telemetry_events\":" << telemetry_.size() << ","
       << "\"active_indexes\":[";
    bool first = true;
    for (const auto& idx : indexes_) {
        if (!first) ss << ",";
        ss << "\"" << idx << "\"";
        first = false;
    }
    ss << "],\"workload\":" << model.snapshot_json() << "}";
    return ss.str();
}

void SableDB::export_telemetry(const std::string& filepath) {
    std::ofstream out(filepath);
    for (const auto& t : telemetry_) {
        out << t.to_json() << "\n";
    }
}

}
