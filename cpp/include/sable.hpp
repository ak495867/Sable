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

inline std::vector<std::string> split_statements(const std::string& sql) {
    std::vector<std::string> stmts;
    std::string current;
    bool in_single = false;
    bool in_double = false;
    for (size_t i = 0; i < sql.size(); ++i) {
        char c = sql[i];
        if (c == '\'' && !in_double) {
            if (in_single && i + 1 < sql.size() && sql[i + 1] == '\'') {
                current += c;
                current += sql[++i];
                continue;
            }
            in_single = !in_single;
        } else if (c == '"' && !in_single) {
            in_double = !in_double;
        } else if (c == ';' && !in_single && !in_double) {
            size_t start = current.find_first_not_of(" \t\n\r");
            if (start != std::string::npos) {
                stmts.push_back(current.substr(start));
            }
            current.clear();
            continue;
        }
        current += c;
    }
    size_t start = current.find_first_not_of(" \t\n\r");
    if (start != std::string::npos) {
        stmts.push_back(current.substr(start));
    }
    return stmts;
}

inline std::pair<std::string, std::string> extract_action_and_table(const std::string& sql) {
    std::string trimmed = sql;
    size_t first = trimmed.find_first_not_of(" \t\n\r");
    if (first != std::string::npos) trimmed = trimmed.substr(first);
    std::string action = "QUERY";
    std::string table = "*";

    std::string low = trimmed;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return std::tolower(c); });

    if (low.rfind("select", 0) == 0) {
        action = "SELECT";
        size_t from_pos = low.find(" from ");
        if (from_pos != std::string::npos) {
            size_t start = low.find_first_not_of(" \t\n\r", from_pos + 6);
            if (start != std::string::npos) {
                size_t end = low.find_first_of(" \t\n\r;,()", start);
                table = (end != std::string::npos) ? trimmed.substr(start, end - start) : trimmed.substr(start);
            }
        }
    } else if (low.rfind("insert", 0) == 0) {
        action = "INSERT";
        size_t into_pos = low.find(" into ");
        if (into_pos != std::string::npos) {
            size_t start = low.find_first_not_of(" \t\n\r", into_pos + 6);
            if (start != std::string::npos) {
                size_t end = low.find_first_of(" \t\n\r;,()", start);
                table = (end != std::string::npos) ? trimmed.substr(start, end - start) : trimmed.substr(start);
            }
        }
    } else if (low.rfind("update", 0) == 0) {
        action = "UPDATE";
        size_t start = low.find_first_not_of(" \t\n\r", 6);
        if (start != std::string::npos) {
            size_t end = low.find_first_of(" \t\n\r;,()", start);
            table = (end != std::string::npos) ? trimmed.substr(start, end - start) : trimmed.substr(start);
        }
    } else if (low.rfind("delete", 0) == 0) {
        action = "DELETE";
        size_t from_pos = low.find(" from ");
        if (from_pos != std::string::npos) {
            size_t start = low.find_first_not_of(" \t\n\r", from_pos + 6);
            if (start != std::string::npos) {
                size_t end = low.find_first_of(" \t\n\r;,()", start);
                table = (end != std::string::npos) ? trimmed.substr(start, end - start) : trimmed.substr(start);
            }
        }
    } else if (low.rfind("create table", 0) == 0) {
        action = "ADMIN";
        size_t start = low.find_first_not_of(" \t\n\r", 12);
        if (start != std::string::npos) {
            size_t end = low.find_first_of(" \t\n\r;(,", start);
            table = (end != std::string::npos) ? trimmed.substr(start, end - start) : trimmed.substr(start);
        }
    } else if (low.rfind("drop table", 0) == 0) {
        action = "ADMIN";
        size_t start = low.find_first_not_of(" \t\n\r", 10);
        if (start != std::string::npos) {
            size_t end = low.find_first_of(" \t\n\r;,", start);
            table = (end != std::string::npos) ? trimmed.substr(start, end - start) : trimmed.substr(start);
        }
    } else if (low.rfind("alter table", 0) == 0) {
        action = "ADMIN";
        size_t start = low.find_first_not_of(" \t\n\r", 11);
        if (start != std::string::npos) {
            size_t end = low.find_first_of(" \t\n\r;,", start);
            table = (end != std::string::npos) ? trimmed.substr(start, end - start) : trimmed.substr(start);
        }
    } else if (low.rfind("create index", 0) == 0 || low.rfind("drop index", 0) == 0) {
        action = "ADMIN";
    } else if (low.rfind("create user", 0) == 0 || low.rfind("drop user", 0) == 0 ||
               low.rfind("alter user", 0) == 0 || low.rfind("grant", 0) == 0 ||
               low.rfind("revoke", 0) == 0 || low.rfind("create rule", 0) == 0 ||
               low.rfind("drop rule", 0) == 0) {
        action = "SECURITY";
    } else if (low.rfind("auth", 0) == 0 || low.rfind("login", 0) == 0) {
        action = "AUTH";
    } else if (low.rfind("show", 0) == 0 || low.rfind("sable", 0) == 0) {
        action = "SELECT";
    }
    return {action, table};
}

struct Permission {
    std::string grantee;
    std::string target_table;
    std::string action;
    bool is_allowed = true;
};

struct DataRule {
    std::string rule_name;
    std::string target_table;
    std::string rule_type;
    std::string expression;
};

struct QueryResult {
    std::string sql;
    std::string action;
    std::string table;
    std::vector<std::string> column_names;
    std::vector<Row> rows;
    int changes = 0;
    bool is_query = false;
    std::string error;
};

class SecurityManager {
public:
    sqlite3* db = nullptr;
    std::string current_user = "admin";
    std::unordered_map<std::string, std::string> user_roles;
    std::unordered_map<std::string, std::string> user_passwords;
    std::vector<Permission> permissions;
    std::vector<DataRule> rules;

    explicit SecurityManager(sqlite3* db_ptr = nullptr);
    void init_security_tables();
    void load_cache();
    bool create_user(const std::string& username, const std::string& password, const std::string& role = "user");
    bool alter_user(const std::string& username, const std::string& password);
    bool drop_user(const std::string& username);
    bool authenticate(const std::string& username, const std::string& password);
    bool grant(const std::string& grantee, const std::string& target_table, const std::string& action);
    bool revoke(const std::string& grantee, const std::string& target_table, const std::string& action);
    bool check_permission(const std::string& username, const std::string& table, const std::string& action) const;
    bool add_rule(const std::string& name, const std::string& table, const std::string& type, const std::string& expr);
    bool drop_rule(const std::string& name);
    bool check_data_rules(const std::string& table, const std::string& sql, std::string& out_violation) const;
    void apply_masking(const std::string& table, const std::vector<std::string>& col_names, std::vector<Row>& rows) const;
    bool handle_custom_command(const std::string& sql, QueryResult& out_result);
};

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
    SecurityManager security;

    explicit SableDB(const std::string& db_path = ":memory:", int epoch_size = 100);
    ~SableDB();

    bool put(const std::string& key, const std::string& value);
    std::string get(const std::string& key);
    int bulk_put(const std::vector<std::pair<std::string, std::string>>& items);
    std::vector<Row> execute(const std::string& sql, const std::vector<std::string>& params = {});
    QueryResult execute_single(const std::string& sql, const std::vector<std::string>& params = {});
    std::vector<QueryResult> execute_script(const std::string& script);

    void record_telemetry(Telemetry t);
    void adapt();
    Proposal approve_proposal();
    bool rollback_last_adaptation();
    void enable_policy(const std::string& policy);
    int archive_cold_records(int limit = 1000);

    bool create_user(const std::string& username, const std::string& password, const std::string& role = "user");
    bool authenticate(const std::string& username, const std::string& password);
    bool grant(const std::string& grantee, const std::string& table, const std::string& action);
    bool revoke(const std::string& grantee, const std::string& table, const std::string& action);
    bool add_rule(const std::string& name, const std::string& table, const std::string& expr, const std::string& type = "FORBID");
    bool drop_rule(const std::string& name);
    std::vector<std::string> list_tables(bool include_internal = false);
    std::string table_schema(const std::string& table_name);
    std::vector<std::string> list_indices(const std::string& table_name = "");
    std::string dump_table(const std::string& table_name);

    std::string status_json() const;
    void export_telemetry(const std::string& filepath);
    void close();
};

}

#endif
