#ifndef SABLE_TYPES_HPP
#define SABLE_TYPES_HPP

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <sstream>
#include <iomanip>

namespace sable {

using Row = std::unordered_map<std::string, std::string>;

inline std::string escape_json(const std::string& s) {
    std::ostringstream o;
    for (char c : s) {
        if (c == '"') o << "\\\"";
        else if (c == '\\') o << "\\\\";
        else if (c == '\b') o << "\\b";
        else if (c == '\f') o << "\\f";
        else if (c == '\n') o << "\\n";
        else if (c == '\r') o << "\\r";
        else if (c == '\t') o << "\\t";
        else if ('\x00' <= c && c <= '\x1f') {
            o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
        } else {
            o << c;
        }
    }
    return o.str();
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
    int64_t cpu_time_us = 0;
    std::vector<std::string> candidate_plans;
    int64_t rows_returned_exact = 0;
    std::string logical_plan;
    std::vector<std::string> sqlite_plan;
    std::string index_name;

    std::string to_json() const {
        std::ostringstream ss;
        ss << "{"
           << "\"query_id\":" << query_id << ","
           << "\"fingerprint\":\"" << escape_json(fingerprint) << "\","
           << "\"plan\":\"" << escape_json(plan) << "\","
           << "\"rows_scanned\":" << rows_scanned << ","
           << "\"rows_returned\":" << rows_returned << ","
           << "\"latency_us\":" << latency_us << ","
           << "\"index_used\":" << (index_used ? "true" : "false") << ","
           << "\"phase\":\"" << escape_json(phase) << "\","
           << "\"epoch\":" << epoch << ","
           << "\"operation\":\"" << escape_json(operation) << "\","
           << "\"cache_hit\":" << (cache_hit ? "true" : "false") << ","
           << "\"bytes_read\":" << bytes_read << ","
           << "\"bytes_written\":" << bytes_written << ","
           << "\"cpu_time_us\":" << cpu_time_us << ","
           << "\"candidate_plans\":[";
        for (size_t i = 0; i < candidate_plans.size(); ++i) {
            ss << "\"" << escape_json(candidate_plans[i]) << "\"";
            if (i + 1 < candidate_plans.size()) ss << ",";
        }
        ss << "],"
           << "\"rows_returned_exact\":" << rows_returned_exact << ","
           << "\"logical_plan\":\"" << escape_json(logical_plan) << "\","
           << "\"sqlite_plan\":[";
        for (size_t i = 0; i < sqlite_plan.size(); ++i) {
            ss << "\"" << escape_json(sqlite_plan[i]) << "\"";
            if (i + 1 < sqlite_plan.size()) ss << ",";
        }
        ss << "],"
           << "\"index_name\":\"" << escape_json(index_name) << "\""
           << "}";
        return ss.str();
    }
};

std::string sha256(const std::string& input);

}

#endif
