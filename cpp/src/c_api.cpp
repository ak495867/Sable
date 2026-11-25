#include "sable.hpp"
#include <cstring>
#include <cstdlib>
#include <sstream>

#if defined(_WIN32)
#define SABLE_EXPORT __declspec(dllexport)
#else
#define SABLE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

SABLE_EXPORT sable::SableDB* sable_db_open(const char* path) {
    try {
        return new sable::SableDB(path ? path : ":memory:");
    } catch (...) {
        return nullptr;
    }
}

SABLE_EXPORT void sable_db_close(sable::SableDB* db) {
    if (db) {
        delete db;
    }
}

SABLE_EXPORT const char* sable_db_execute_json(sable::SableDB* db, const char* sql, const char* user) {
    if (!db || !sql) return nullptr;
    if (user && *user) {
        db->security.current_user = user;
    }
    try {
        auto results = db->execute_script(sql);
        std::ostringstream ss;
        ss << "[";
        bool first_row = true;
        for (const auto& res : results) {
            if (!res.is_query) continue;
            for (const auto& r : res.rows) {
                if (!first_row) ss << ",";
                ss << "{";
                bool first_col = true;
                for (const auto& col : res.column_names) {
                    auto it = r.find(col);
                    std::string val = (it != r.end()) ? it->second : "";
                    if (!first_col) ss << ",";
                    ss << "\"" << sable::json_escape(col) << "\":\"" << sable::json_escape(val) << "\"";
                    first_col = false;
                }
                ss << "}";
                first_row = false;
            }
        }
        ss << "]";
        std::string out = ss.str();
        char* buf = static_cast<char*>(malloc(out.size() + 1));
        if (!buf) return nullptr;
        std::memcpy(buf, out.c_str(), out.size() + 1);
        return buf;
    } catch (const std::exception& ex) {
        std::string err = std::string("{\"error\":\"") + sable::json_escape(ex.what()) + "\"}";
        char* buf = static_cast<char*>(malloc(err.size() + 1));
        if (!buf) return nullptr;
        std::memcpy(buf, err.c_str(), err.size() + 1);
        return buf;
    }
}

SABLE_EXPORT const char* sable_db_status_json(sable::SableDB* db) {
    if (!db) return nullptr;
    std::string s = db->status_json();
    char* buf = static_cast<char*>(malloc(s.size() + 1));
    if (!buf) return nullptr;
    std::memcpy(buf, s.c_str(), s.size() + 1);
    return buf;
}

SABLE_EXPORT void sable_free_string(char* ptr) {
    if (ptr) {
        free(ptr);
    }
}

}
