#include "sable.hpp"
#include <fstream>
#include <iostream>
#include <cmath>

namespace sable {

SecurityManager::SecurityManager(sqlite3* db_ptr) : db(db_ptr) {
    if (db) {
        init_security_tables();
        load_cache();
    }
}

void SecurityManager::init_security_tables() {
    if (!db) return;
    char* err = nullptr;
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS _sable_users (username TEXT PRIMARY KEY, password_hash TEXT, role TEXT, created_at REAL);"
        "CREATE TABLE IF NOT EXISTS _sable_permissions (grantee TEXT, target_table TEXT, action TEXT, is_allowed INTEGER, PRIMARY KEY(grantee, target_table, action));"
        "CREATE TABLE IF NOT EXISTS _sable_rules (rule_name TEXT PRIMARY KEY, target_table TEXT, rule_type TEXT, expression TEXT, created_at REAL);",
        nullptr, nullptr, &err);
    if (err) sqlite3_free(err);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM _sable_users;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int cnt = sqlite3_column_int(stmt, 0);
            if (cnt == 0) {
                sqlite3_exec(db,
                    "INSERT INTO _sable_users(username, password_hash, role, created_at) VALUES('admin', 'admin', 'admin', 0.0);"
                    "INSERT INTO _sable_permissions(grantee, target_table, action, is_allowed) VALUES('admin', '*', 'ALL', 1);",
                    nullptr, nullptr, nullptr);
            }
        }
        sqlite3_finalize(stmt);
    }
}

void SecurityManager::load_cache() {
    if (!db) return;
    user_roles.clear();
    user_passwords.clear();
    permissions.clear();
    rules.clear();

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT username, password_hash, role FROM _sable_users;", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* u = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* r = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            if (u && r) {
                user_roles[u] = r;
                if (p) user_passwords[u] = p;
            }
        }
        sqlite3_finalize(stmt);
    }

    if (sqlite3_prepare_v2(db, "SELECT grantee, target_table, action, is_allowed FROM _sable_permissions;", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* g = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* a = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            int allowed = sqlite3_column_int(stmt, 3);
            if (g && t && a) {
                permissions.push_back({g, t, a, allowed != 0});
            }
        }
        sqlite3_finalize(stmt);
    }

    if (sqlite3_prepare_v2(db, "SELECT rule_name, target_table, rule_type, expression FROM _sable_rules;", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* rn = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* tt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* rt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            const char* ex = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            if (rn && tt && rt && ex) {
                rules.push_back({rn, tt, rt, ex});
            }
        }
        sqlite3_finalize(stmt);
    }
}

bool SecurityManager::create_user(const std::string& username, const std::string& password, const std::string& role) {
    if (!db) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO _sable_users(username, password_hash, role, created_at) VALUES(?, ?, ?, ?) "
                      "ON CONFLICT(username) DO UPDATE SET password_hash=excluded.password_hash, role=excluded.role;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    std::string h = hash_fingerprint(password);
    sqlite3_bind_text(stmt, 2, h.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, role.c_str(), -1, SQLITE_TRANSIENT);
    double now = static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    sqlite3_bind_double(stmt, 4, now);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        load_cache();
        return true;
    }
    return false;
}

bool SecurityManager::alter_user(const std::string& username, const std::string& password) {
    if (!db) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE _sable_users SET password_hash = ? WHERE username = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    std::string h = hash_fingerprint(password);
    sqlite3_bind_text(stmt, 1, h.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        load_cache();
        return true;
    }
    return false;
}

bool SecurityManager::drop_user(const std::string& username) {
    if (!db || username == "admin") return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM _sable_users WHERE username = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        sqlite3_stmt* stmt_p = nullptr;
        if (sqlite3_prepare_v2(db, "DELETE FROM _sable_permissions WHERE grantee = ?;", -1, &stmt_p, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt_p, 1, username.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt_p);
            sqlite3_finalize(stmt_p);
        }
        load_cache();
        return true;
    }
    return false;
}

bool SecurityManager::authenticate(const std::string& username, const std::string& password) {
    if (!db) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT password_hash FROM _sable_users WHERE username = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* stored = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string expected = hash_fingerprint(password);
        if (stored && (expected == stored || password == stored)) {
            ok = true;
            current_user = username;
        }
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool SecurityManager::grant(const std::string& grantee, const std::string& target_table, const std::string& action) {
    if (!db) return false;
    std::string act = action;
    std::transform(act.begin(), act.end(), act.begin(), [](unsigned char c) { return std::toupper(c); });
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO _sable_permissions(grantee, target_table, action, is_allowed) VALUES(?, ?, ?, 1) "
                      "ON CONFLICT(grantee, target_table, action) DO UPDATE SET is_allowed=1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, grantee.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, target_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, act.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        load_cache();
        return true;
    }
    return false;
}

bool SecurityManager::revoke(const std::string& grantee, const std::string& target_table, const std::string& action) {
    if (!db) return false;
    std::string act = action;
    std::transform(act.begin(), act.end(), act.begin(), [](unsigned char c) { return std::toupper(c); });
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM _sable_permissions WHERE grantee = ? AND target_table = ? AND action = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, grantee.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, target_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, act.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        load_cache();
        return true;
    }
    return false;
}

bool SecurityManager::check_permission(const std::string& username, const std::string& table, const std::string& action) const {
    if (username == "admin") return true;
    auto it_role = user_roles.find(username);
    std::string role = (it_role != user_roles.end()) ? it_role->second : "user";
    if (role == "admin") return true;

    std::string act = action;
    std::transform(act.begin(), act.end(), act.begin(), [](unsigned char c) { return std::toupper(c); });
    std::string tbl = table;
    std::transform(tbl.begin(), tbl.end(), tbl.begin(), [](unsigned char c) { return std::tolower(c); });

    for (const auto& p : permissions) {
        if (!p.is_allowed) continue;
        bool match_grantee = (p.grantee == username || p.grantee == role || p.grantee == "*");
        std::string p_tbl = p.target_table;
        std::transform(p_tbl.begin(), p_tbl.end(), p_tbl.begin(), [](unsigned char c) { return std::tolower(c); });
        bool match_table = (p_tbl == "*" || p_tbl == tbl);
        bool match_action = (p.action == "ALL" || p.action == "ADMIN" || p.action == act);
        if (match_grantee && match_table && match_action) {
            return true;
        }
    }
    return false;
}

bool SecurityManager::add_rule(const std::string& name, const std::string& table, const std::string& type, const std::string& expr) {
    if (!db) return false;
    std::string typ = type;
    std::transform(typ.begin(), typ.end(), typ.begin(), [](unsigned char c) { return std::toupper(c); });
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO _sable_rules(rule_name, target_table, rule_type, expression, created_at) VALUES(?, ?, ?, ?, ?) "
                      "ON CONFLICT(rule_name) DO UPDATE SET target_table=excluded.target_table, rule_type=excluded.rule_type, expression=excluded.expression;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, typ.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, expr.c_str(), -1, SQLITE_TRANSIENT);
    double now = static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    sqlite3_bind_double(stmt, 5, now);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        load_cache();
        return true;
    }
    return false;
}

bool SecurityManager::drop_rule(const std::string& name) {
    if (!db) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM _sable_rules WHERE rule_name = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        load_cache();
        return true;
    }
    return false;
}

bool SecurityManager::check_data_rules(const std::string& table, const std::string& sql, std::string& out_violation) const {
    if (rules.empty() || table.empty() || table == "*") return true;
    std::string tbl = table;
    std::transform(tbl.begin(), tbl.end(), tbl.begin(), [](unsigned char c) { return std::tolower(c); });

    std::string low_sql = sql;
    std::transform(low_sql.begin(), low_sql.end(), low_sql.begin(), [](unsigned char c) { return std::tolower(c); });

    auto [action, t_act] = extract_action_and_table(sql);

    for (const auto& r : rules) {
        std::string r_tbl = r.target_table;
        std::transform(r_tbl.begin(), r_tbl.end(), r_tbl.begin(), [](unsigned char c) { return std::tolower(c); });
        if (r_tbl != "*" && r_tbl != tbl) continue;

        if (r.rule_type == "READONLY") {
            if (action == "INSERT" || action == "UPDATE" || action == "DELETE" || action == "ADMIN") {
                out_violation = "table '" + table + "' is marked READONLY by rule '" + r.rule_name + "'";
                return false;
            }
        } else if (r.rule_type == "FORBID" || r.rule_type == "RESTRICT" || r.rule_type == "CHECK_SQL") {
            std::string pat = r.expression;
            std::transform(pat.begin(), pat.end(), pat.begin(), [](unsigned char c) { return std::tolower(c); });
            if (!pat.empty() && low_sql.find(pat) != std::string::npos) {
                out_violation = "rule '" + r.rule_name + "' triggered by pattern: " + r.expression;
                return false;
            }
        }
    }
    return true;
}

void SecurityManager::apply_masking(const std::string& table, const std::vector<std::string>& col_names, std::vector<Row>& rows) const {
    if (current_user == "admin" || rows.empty() || rules.empty()) return;
    std::string tbl = table;
    std::transform(tbl.begin(), tbl.end(), tbl.begin(), [](unsigned char c) { return std::tolower(c); });

    std::unordered_set<std::string> masked_columns;
    for (const auto& r : rules) {
        if (r.rule_type != "MASK") continue;
        std::string r_tbl = r.target_table;
        std::transform(r_tbl.begin(), r_tbl.end(), r_tbl.begin(), [](unsigned char c) { return std::tolower(c); });
        if (r_tbl != "*" && r_tbl != tbl) continue;

        std::stringstream ss(r.expression);
        std::string col;
        while (std::getline(ss, col, ',')) {
            size_t start = col.find_first_not_of(" \t\r\n");
            size_t end = col.find_last_not_of(" \t\r\n");
            if (start != std::string::npos && end != std::string::npos) {
                std::string c_trim = col.substr(start, end - start + 1);
                std::transform(c_trim.begin(), c_trim.end(), c_trim.begin(), [](unsigned char c) { return std::tolower(c); });
                masked_columns.insert(c_trim);
            }
        }
    }

    if (masked_columns.empty()) return;

    for (auto& row : rows) {
        for (const auto& c : col_names) {
            std::string low_c = c;
            std::transform(low_c.begin(), low_c.end(), low_c.begin(), [](unsigned char ch) { return std::tolower(ch); });
            if (masked_columns.count(low_c)) {
                row[c] = "***MASKED***";
            }
        }
    }
}

bool SecurityManager::handle_custom_command(const std::string& sql, QueryResult& out) {
    std::string trimmed = sql;
    size_t first = trimmed.find_first_not_of(" \t\n\r");
    if (first != std::string::npos) trimmed = trimmed.substr(first);
    while (!trimmed.empty() && (trimmed.back() == ';' || trimmed.back() == ' ' || trimmed.back() == '\t' || trimmed.back() == '\r' || trimmed.back() == '\n')) {
        trimmed.pop_back();
    }
    std::string low = trimmed;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return std::tolower(c); });

    if (low.rfind("auth ", 0) == 0 || low.rfind("login ", 0) == 0) {
        std::istringstream iss(trimmed);
        std::string cmd, u, p;
        iss >> cmd >> u >> p;
        if (authenticate(u, p)) {
            out.action = "AUTH";
            out.column_names = {"status", "user"};
            out.rows = {{{"status", "authenticated"}, {"user", u}}};
            out.is_query = true;
            return true;
        } else {
            throw std::runtime_error("Authentication failed for user '" + u + "'");
        }
    }

    if (low.rfind("create user ", 0) == 0) {
        if (current_user != "admin" && user_roles[current_user] != "admin") {
            throw std::runtime_error("Permission denied: only admin can execute CREATE USER");
        }
        std::string rest = trimmed.substr(12);
        std::istringstream iss(rest);
        std::string u, token;
        iss >> u;
        std::string pwd, role = "user";
        while (iss >> token) {
            std::string low_t = token;
            std::transform(low_t.begin(), low_t.end(), low_t.begin(), [](unsigned char c) { return std::tolower(c); });
            if (low_t == "identified" || low_t == "with" || low_t == "by" || low_t == "password") {
                continue;
            }
            if (low_t == "role") {
                iss >> role;
                continue;
            }
            if (pwd.empty()) {
                pwd = token;
            }
        }
        auto strip_quotes = [](std::string& s) {
            if (s.size() >= 2 && ((s.front() == '\'' && s.back() == '\'') || (s.front() == '"' && s.back() == '"'))) {
                s = s.substr(1, s.size() - 2);
            }
        };
        strip_quotes(pwd);
        strip_quotes(role);
        if (!create_user(u, pwd, role)) {
            throw std::runtime_error("Failed to create user '" + u + "'");
        }
        out.action = "SECURITY";
        out.column_names = {"status", "user", "role"};
        out.rows = {{{"status", "created"}, {"user", u}, {"role", role}}};
        out.is_query = true;
        return true;
    }

    if (low.rfind("alter user ", 0) == 0) {
        if (current_user != "admin" && user_roles[current_user] != "admin") {
            throw std::runtime_error("Permission denied: only admin can execute ALTER USER");
        }
        std::string rest = trimmed.substr(11);
        std::istringstream iss(rest);
        std::string u, token, pwd;
        iss >> u;
        while (iss >> token) {
            std::string low_t = token;
            std::transform(low_t.begin(), low_t.end(), low_t.begin(), [](unsigned char c) { return std::tolower(c); });
            if (low_t == "identified" || low_t == "with" || low_t == "by" || low_t == "password") {
                continue;
            }
            if (pwd.empty()) {
                pwd = token;
            }
        }
        if (pwd.size() >= 2 && ((pwd.front() == '\'' && pwd.back() == '\'') || (pwd.front() == '"' && pwd.back() == '"'))) {
            pwd = pwd.substr(1, pwd.size() - 2);
        }
        if (!alter_user(u, pwd)) {
            throw std::runtime_error("Failed to alter user '" + u + "'");
        }
        out.action = "SECURITY";
        out.column_names = {"status", "user"};
        out.rows = {{{"status", "password_updated"}, {"user", u}}};
        out.is_query = true;
        return true;
    }

    if (low.rfind("drop user ", 0) == 0) {
        if (current_user != "admin" && user_roles[current_user] != "admin") {
            throw std::runtime_error("Permission denied: only admin can execute DROP USER");
        }
        std::string u = trimmed.substr(10);
        size_t s = u.find_first_not_of(" \t");
        size_t e = u.find_last_not_of(" \t");
        if (s != std::string::npos && e != std::string::npos) u = u.substr(s, e - s + 1);
        if (!drop_user(u)) {
            throw std::runtime_error("Failed to drop user '" + u + "'");
        }
        out.action = "SECURITY";
        out.column_names = {"status", "user"};
        out.rows = {{{"status", "dropped"}, {"user", u}}};
        out.is_query = true;
        return true;
    }

    if (low.rfind("grant ", 0) == 0) {
        if (current_user != "admin" && user_roles[current_user] != "admin") {
            throw std::runtime_error("Permission denied: only admin can execute GRANT");
        }
        std::string rest = trimmed.substr(6);
        size_t on_pos = low.find(" on ");
        size_t to_pos = low.find(" to ");
        if (on_pos == std::string::npos || to_pos == std::string::npos || to_pos < on_pos) {
            throw std::runtime_error("Syntax error: expected GRANT <action> ON <target> TO <grantee>");
        }
        std::string act = trimmed.substr(6, on_pos - 6);
        std::string tbl = trimmed.substr(on_pos + 4, to_pos - (on_pos + 4));
        std::string grantee = trimmed.substr(to_pos + 4);
        auto clean = [](std::string& s) {
            size_t st = s.find_first_not_of(" \t");
            size_t en = s.find_last_not_of(" \t");
            s = (st != std::string::npos && en != std::string::npos) ? s.substr(st, en - st + 1) : "";
        };
        clean(act);
        clean(tbl);
        clean(grantee);

        std::stringstream ss(act);
        std::string sub_act;
        while (std::getline(ss, sub_act, ',')) {
            clean(sub_act);
            if (!sub_act.empty()) grant(grantee, tbl, sub_act);
        }
        out.action = "SECURITY";
        out.column_names = {"status", "grantee", "table", "action"};
        out.rows = {{{"status", "granted"}, {"grantee", grantee}, {"table", tbl}, {"action", act}}};
        out.is_query = true;
        return true;
    }

    if (low.rfind("revoke ", 0) == 0) {
        if (current_user != "admin" && user_roles[current_user] != "admin") {
            throw std::runtime_error("Permission denied: only admin can execute REVOKE");
        }
        size_t on_pos = low.find(" on ");
        size_t from_pos = low.find(" from ");
        if (on_pos == std::string::npos || from_pos == std::string::npos || from_pos < on_pos) {
            throw std::runtime_error("Syntax error: expected REVOKE <action> ON <target> FROM <grantee>");
        }
        std::string act = trimmed.substr(7, on_pos - 7);
        std::string tbl = trimmed.substr(on_pos + 4, from_pos - (on_pos + 4));
        std::string grantee = trimmed.substr(from_pos + 6);
        auto clean = [](std::string& s) {
            size_t st = s.find_first_not_of(" \t");
            size_t en = s.find_last_not_of(" \t");
            s = (st != std::string::npos && en != std::string::npos) ? s.substr(st, en - st + 1) : "";
        };
        clean(act);
        clean(tbl);
        clean(grantee);

        std::stringstream ss(act);
        std::string sub_act;
        while (std::getline(ss, sub_act, ',')) {
            clean(sub_act);
            if (!sub_act.empty()) revoke(grantee, tbl, sub_act);
        }
        out.action = "SECURITY";
        out.column_names = {"status", "grantee", "table", "action"};
        out.rows = {{{"status", "revoked"}, {"grantee", grantee}, {"table", tbl}, {"action", act}}};
        out.is_query = true;
        return true;
    }

    if (low.rfind("create rule ", 0) == 0) {
        if (current_user != "admin" && user_roles[current_user] != "admin") {
            throw std::runtime_error("Permission denied: only admin can execute CREATE RULE");
        }
        size_t on_pos = low.find(" on ");
        if (on_pos == std::string::npos) {
            throw std::runtime_error("Syntax error: expected CREATE RULE <name> ON <target> <type> '<expr>'");
        }
        std::string r_name = trimmed.substr(12, on_pos - 12);
        size_t s = r_name.find_first_not_of(" \t");
        size_t e = r_name.find_last_not_of(" \t");
        if (s != std::string::npos && e != std::string::npos) r_name = r_name.substr(s, e - s + 1);

        std::string rem = trimmed.substr(on_pos + 4);
        std::istringstream iss(rem);
        std::string tbl, r_type, expr;
        iss >> tbl >> r_type;
        std::getline(iss, expr);
        size_t es = expr.find_first_not_of(" \t");
        size_t ee = expr.find_last_not_of(" \t");
        if (es != std::string::npos && ee != std::string::npos) expr = expr.substr(es, ee - es + 1);
        if (expr.size() >= 2 && (expr.front() == '\'' || expr.front() == '"')) {
            expr = expr.substr(1, expr.size() - 2);
        }

        std::string r_type_upper = r_type;
        std::transform(r_type_upper.begin(), r_type_upper.end(), r_type_upper.begin(), [](unsigned char c) { return std::toupper(c); });
        if (r_type_upper == "READONLY" && expr.empty()) {
            expr = "READONLY";
        }

        if (!add_rule(r_name, tbl, r_type_upper, expr)) {
            throw std::runtime_error("Failed to add data rule '" + r_name + "'");
        }
        out.action = "SECURITY";
        out.column_names = {"status", "rule_name", "table", "type"};
        out.rows = {{{"status", "rule_created"}, {"rule_name", r_name}, {"table", tbl}, {"type", r_type_upper}}};
        out.is_query = true;
        return true;
    }

    if (low.rfind("drop rule ", 0) == 0) {
        if (current_user != "admin" && user_roles[current_user] != "admin") {
            throw std::runtime_error("Permission denied: only admin can execute DROP RULE");
        }
        std::string r_name = trimmed.substr(10);
        size_t s = r_name.find_first_not_of(" \t");
        size_t e = r_name.find_last_not_of(" \t");
        if (s != std::string::npos && e != std::string::npos) r_name = r_name.substr(s, e - s + 1);
        if (!drop_rule(r_name)) {
            throw std::runtime_error("Failed to drop rule '" + r_name + "'");
        }
        out.action = "SECURITY";
        out.column_names = {"status", "rule_name"};
        out.rows = {{{"status", "rule_dropped"}, {"rule_name", r_name}}};
        out.is_query = true;
        return true;
    }

    if (low == "show users") {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT username, role, created_at FROM _sable_users ORDER BY username;", -1, &stmt, nullptr) == SQLITE_OK) {
            out.column_names = {"username", "role", "created_at"};
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                Row r;
                const char* u = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* ro = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                const char* ca = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                r["username"] = u ? u : "";
                r["role"] = ro ? ro : "";
                r["created_at"] = ca ? ca : "";
                out.rows.push_back(r);
            }
            sqlite3_finalize(stmt);
            out.action = "SELECT";
            out.is_query = true;
            return true;
        }
    }

    if (low == "show permissions") {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT grantee, target_table, action, is_allowed FROM _sable_permissions ORDER BY grantee, target_table;", -1, &stmt, nullptr) == SQLITE_OK) {
            out.column_names = {"grantee", "target_table", "action", "is_allowed"};
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                Row r;
                const char* g = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* tt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                const char* a = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                int al = sqlite3_column_int(stmt, 3);
                r["grantee"] = g ? g : "";
                r["target_table"] = tt ? tt : "";
                r["action"] = a ? a : "";
                r["is_allowed"] = al ? "1" : "0";
                out.rows.push_back(r);
            }
            sqlite3_finalize(stmt);
            out.action = "SELECT";
            out.is_query = true;
            return true;
        }
    }

    if (low == "show rules") {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT rule_name, target_table, rule_type, expression FROM _sable_rules ORDER BY rule_name;", -1, &stmt, nullptr) == SQLITE_OK) {
            out.column_names = {"rule_name", "target_table", "rule_type", "expression"};
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                Row r;
                const char* rn = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* tt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                const char* rt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                const char* ex = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                r["rule_name"] = rn ? rn : "";
                r["target_table"] = tt ? tt : "";
                r["rule_type"] = rt ? rt : "";
                r["expression"] = ex ? ex : "";
                out.rows.push_back(r);
            }
            sqlite3_finalize(stmt);
            out.action = "SELECT";
            out.is_query = true;
            return true;
        }
    }

    return false;
}

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
    fingerprints[fp]++;
    plan_counts[plan]++;
    plan_latency[plan].push_back(latency_us);
    if (plan_latency[plan].size() > 500) {
        plan_latency[plan].erase(plan_latency[plan].begin(), plan_latency[plan].begin() + 250);
    }
    latencies.push_back(latency_us);
    if (latencies.size() > 1000) {
        latencies.erase(latencies.begin(), latencies.begin() + 500);
    }

    std::string op_type = "scan";
    if (op == "insert" || op == "update" || op == "delete" || op == "write") {
        op_type = "write";
        write++;
    } else if (plan == "index_lookup" || (rows_returned == 1 && rows_scanned <= 2)) {
        op_type = "point_lookup";
        point++;
    } else if (plan == "aggregate") {
        op_type = "aggregate";
        aggregate++;
    } else if (plan == "join") {
        op_type = "join";
        join++;
    } else {
        scan++;
    }

    recent.push_back(op_type);
    recent_counts[op_type]++;
    if (static_cast<int>(recent.size()) > window) {
        std::string evicted = recent.front();
        recent.pop_front();
        recent_counts[evicted]--;
    }
}

std::string WorkloadModel::phase() const {
    if (recent.empty()) return "startup";
    double n = static_cast<double>(recent.size());
    double pt = 0.0, wr = 0.0, sc = 0.0, ag = 0.0, jn = 0.0;
    auto it = recent_counts.find("point_lookup"); if (it != recent_counts.end()) pt = it->second / n;
    it = recent_counts.find("write"); if (it != recent_counts.end()) wr = it->second / n;
    it = recent_counts.find("scan"); if (it != recent_counts.end()) sc = it->second / n;
    it = recent_counts.find("aggregate"); if (it != recent_counts.end()) ag = it->second / n;
    it = recent_counts.find("join"); if (it != recent_counts.end()) jn = it->second / n;

    if (wr > 0.50) return "write_heavy";
    if (pt > 0.60) return "point_lookup";
    if (ag > 0.35) return "analytical";
    if (jn > 0.35) return "join_heavy";
    if (sc > 0.50) return "bulk_scan";
    return "mixed";
}

double WorkloadModel::p50_latency() const {
    if (latencies.empty()) return 0.0;
    auto s = latencies;
    std::sort(s.begin(), s.end());
    return static_cast<double>(s[s.size() / 2]);
}

double WorkloadModel::p95_latency() const {
    if (latencies.empty()) return 0.0;
    auto s = latencies;
    std::sort(s.begin(), s.end());
    size_t idx = static_cast<size_t>(s.size() * 0.95);
    if (idx >= s.size()) idx = s.size() - 1;
    return static_cast<double>(s[idx]);
}

std::string WorkloadModel::snapshot_json() const {
    std::ostringstream ss;
    ss << "{"
       << "\"total\":" << total << ","
       << "\"point\":" << point << ","
       << "\"scan\":" << scan << ","
       << "\"aggregate\":" << aggregate << ","
       << "\"join\":" << join << ","
       << "\"write\":" << write << ","
       << "\"phase\":\"" << phase() << "\","
       << "\"p50_latency_us\":" << p50_latency() << ","
       << "\"p95_latency_us\":" << p95_latency()
       << "}";
    return ss.str();
}

CostModel::CostModel(int win) : window(win) {}

void CostModel::observe(int64_t rows_scanned, int64_t latency_us, bool index_used, bool join, bool aggregate) {
    samples.push_back({rows_scanned, latency_us, index_used, join, aggregate});
    if (static_cast<int>(samples.size()) > window) {
        samples.erase(samples.begin(), samples.begin() + (window / 4));
    }
    dirty++;
    if (dirty >= 10) {
        fit();
    }
}

void CostModel::fit() {
    if (samples.size() < 10) return;
    double sum_idx = 0.0, sum_scan = 0.0;
    int cnt_idx = 0, cnt_scan = 0;
    double sum_join = 0.0, sum_agg = 0.0;
    int cnt_join = 0, cnt_agg = 0;

    for (const auto& s : samples) {
        if (s.index_used) {
            sum_idx += s.latency_us;
            cnt_idx++;
        } else if (!s.join && !s.aggregate) {
            sum_scan += s.latency_us;
            cnt_scan++;
        }
        if (s.join) {
            sum_join += s.latency_us;
            cnt_join++;
        }
        if (s.aggregate) {
            sum_agg += s.latency_us;
            cnt_agg++;
        }
    }

    double scan_mean = (cnt_scan > 0) ? (sum_scan / cnt_scan) : 100.0;
    if (cnt_idx > 0 && scan_mean > 0.0) {
        double idx_mean = sum_idx / cnt_idx;
        index_penalty = std::clamp(idx_mean / scan_mean, 0.01, 0.99);
    }
    if (cnt_join > 0 && scan_mean > 0.0) {
        join_multiplier = std::clamp((sum_join / cnt_join) / scan_mean, 1.1, 10.0);
    }
    if (cnt_agg > 0 && scan_mean > 0.0) {
        aggregate_multiplier = std::clamp((sum_agg / cnt_agg) / scan_mean, 1.1, 5.0);
    }
    dirty = 0;
    version++;
}

double CostModel::estimate(int64_t rows_scanned, bool index_used, bool join, bool aggregate) const {
    double cost = base + per_row * static_cast<double>(rows_scanned);
    if (index_used) cost *= index_penalty;
    if (join) cost *= join_multiplier;
    if (aggregate) cost *= aggregate_multiplier;
    return cost;
}

AdaptivePlanner::AdaptivePlanner(CostModel* cm) : cost_model(cm) {}

std::string AdaptivePlanner::select(const std::string& sql, int64_t estimated_rows,
                                    const std::unordered_set<std::string>& indexes,
                                    std::vector<std::string>& out_candidates, bool& cache_hit) {
    std::string low = sql;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return std::tolower(c); });

    bool is_join = (low.find(" join ") != std::string::npos);
    bool is_agg = (low.find("count(") != std::string::npos || low.find("sum(") != std::string::npos ||
                   low.find("avg(") != std::string::npos || low.find("group by") != std::string::npos);

    if (cost_model && cost_model->version != fit_version) {
        plan_cache.clear();
        fit_version = cost_model->version;
    }

    std::string key = low.substr(0, 80);
    auto it_cache = plan_cache.find(key);
    if (it_cache != plan_cache.end()) {
        cache_hit = true;
        out_candidates = std::get<1>(it_cache->second);
        last_plan = std::get<0>(it_cache->second);
        return last_plan;
    }
    cache_hit = false;

    out_candidates.clear();
    out_candidates.push_back("scan");
    if (!indexes.empty()) {
        out_candidates.push_back("index_lookup");
    }
    if (is_join) {
        out_candidates.push_back("join");
    }
    if (is_agg) {
        out_candidates.push_back("aggregate");
    }

    std::string best_plan = "scan";
    double best_cost = cost_model ? cost_model->estimate(estimated_rows, false, is_join, is_agg) : 1000.0;

    if (!indexes.empty()) {
        double idx_cost = cost_model ? cost_model->estimate(std::min<int64_t>(estimated_rows, 1), true, is_join, is_agg) : 200.0;
        if (idx_cost < best_cost) {
            best_cost = idx_cost;
            best_plan = "index_lookup";
        }
    }
    if (is_join) {
        best_plan = "join";
    } else if (is_agg) {
        best_plan = "aggregate";
    }

    last_plan = best_plan;
    plan_cache[key] = {best_plan, out_candidates, cache_hit};
    return best_plan;
}

LinUCBBandit::LinUCBBandit(const std::vector<std::string>& arm_names, double a) : arms(arm_names), alpha(a) {
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
    double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                 m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                 m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::abs(det) < 1e-9) det = 1e-9;
    double invdet = 1.0 / det;
    std::vector<std::vector<double>> inv(3, std::vector<double>(3, 0.0));
    inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * invdet;
    inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * invdet;
    inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * invdet;
    inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * invdet;
    inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * invdet;
    inv[1][2] = (m[1][0] * m[0][2] - m[0][0] * m[1][2]) * invdet;
    inv[2][0] = (m[1][0] * m[2][1] - m[2][0] * m[1][1]) * invdet;
    inv[2][1] = (m[2][0] * m[0][1] - m[0][0] * m[2][1]) * invdet;
    inv[2][2] = (m[0][0] * m[1][1] - m[1][0] * m[0][1]) * invdet;
    return inv;
}

std::string LinUCBBandit::select(const std::vector<double>& ctx) {
    std::string best_arm = arms[0];
    double best_p = -1e9;
    for (const auto& arm : arms) {
        auto invA = invert3x3(A[arm]);
        std::vector<double> theta(3, 0.0);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                theta[i] += invA[i][j] * b[arm][j];
            }
        }
        double mean = 0.0;
        for (int i = 0; i < 3; ++i) mean += theta[i] * ctx[i];
        double var = 0.0;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                var += ctx[i] * invA[i][j] * ctx[j];
            }
        }
        double p = mean + alpha * std::sqrt(std::max(0.0, var));
        if (p > best_p) {
            best_p = p;
            best_arm = arm;
        }
    }
    return best_arm;
}

void LinUCBBandit::update(const std::string& arm, const std::vector<double>& ctx, double reward) {
    if (!A.count(arm)) return;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            A[arm][i][j] += ctx[i] * ctx[j];
        }
        b[arm][i] += reward * ctx[i];
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

ShadowValidator::ValidationResult ShadowValidator::compare(const std::vector<double>& baseline,
                                                           const std::vector<double>& candidate) {
    ValidationResult res;
    if (baseline.size() < 2 || candidate.size() < 2) return res;
    double sum_b = 0.0, sum_c = 0.0;
    for (double v : baseline) sum_b += v;
    for (double v : candidate) sum_c += v;
    res.baseline_mean = sum_b / baseline.size();
    res.candidate_mean = sum_c / candidate.size();
    if (res.candidate_mean > 0.0) {
        res.speedup = res.baseline_mean / res.candidate_mean;
    }

    double var_b = 0.0, var_c = 0.0;
    for (double v : baseline) var_b += (v - res.baseline_mean) * (v - res.baseline_mean);
    for (double v : candidate) var_c += (v - res.candidate_mean) * (v - res.candidate_mean);
    var_b /= (baseline.size() - 1);
    var_c /= (candidate.size() - 1);

    double se = std::sqrt((var_b / baseline.size()) + (var_c / candidate.size()));
    if (se > 1e-9) {
        res.t_stat = (res.baseline_mean - res.candidate_mean) / se;
    }
    res.p_value = (res.t_stat > 1.96) ? 0.01 : 0.50;
    res.validated = (res.speedup > 1.15 && res.t_stat > 1.64);
    return res;
}

SableDB::SableDB(const std::string& db_path, int epoch_size)
    : path_(db_path), epoch_size_(epoch_size) {
    int rc = sqlite3_open(path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string err = db_ ? sqlite3_errmsg(db_) : "failed to open db";
        throw std::runtime_error("Cannot open database: " + err);
    }
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA cache_size=-64000;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA temp_store=MEMORY;", nullptr, nullptr, nullptr);

    security = SecurityManager(db_);
    planner = std::make_unique<AdaptivePlanner>(&cost_model);
    init_tables();
}

SableDB::~SableDB() {
    close();
}

void SableDB::close() {
    if (db_) {
        persist_state(true);
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

void SableDB::init_tables() {
    if (!db_) return;
    char* err = nullptr;
    sqlite3_exec(db_,
        "CREATE TABLE IF NOT EXISTS sable_records (id INTEGER PRIMARY KEY AUTOINCREMENT, key TEXT UNIQUE, value TEXT, created_at REAL);"
        "CREATE TABLE IF NOT EXISTS _sable_cold_records (id INTEGER PRIMARY KEY, key TEXT, value TEXT, archived_at REAL);"
        "CREATE TABLE IF NOT EXISTS _sable_state (state_key TEXT PRIMARY KEY, state_value TEXT, updated_at REAL);",
        nullptr, nullptr, &err);
    if (err) sqlite3_free(err);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT name FROM sqlite_master WHERE type='index' AND tbl_name='sable_records';", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* n = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (n) indexes_.insert(n);
        }
        sqlite3_finalize(stmt);
    }
}

void SableDB::persist_state(bool force) {
    if (!db_) return;
    if (!force && query_id_ % 50 != 0) return;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO _sable_state(state_key, state_value, updated_at) VALUES(?, ?, ?) "
                      "ON CONFLICT(state_key) DO UPDATE SET state_value=excluded.state_value, updated_at=excluded.updated_at;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, "adaptive", -1, SQLITE_TRANSIENT);
        std::string val = status_json();
        sqlite3_bind_text(stmt, 2, val.c_str(), -1, SQLITE_TRANSIENT);
        double now = static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        sqlite3_bind_double(stmt, 3, now);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::tuple<std::vector<std::string>, bool, std::string> SableDB::inspect_sqlite_plan(const std::string& sql) {
    std::string key = sql.substr(0, 80);
    auto it = plan_cache_.find(key);
    if (it != plan_cache_.end()) return it->second;

    std::vector<std::string> lines;
    bool index_used = false;
    std::string index_name;

    sqlite3_stmt* stmt = nullptr;
    std::string explain_sql = "EXPLAIN QUERY PLAN " + sql;
    if (sqlite3_prepare_v2(db_, explain_sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* detail = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            if (detail) {
                std::string s = detail;
                lines.push_back(s);
                if (s.find("USING INDEX") != std::string::npos || s.find("USING COVERING INDEX") != std::string::npos) {
                    index_used = true;
                    size_t p = s.find("INDEX ");
                    if (p != std::string::npos) {
                        size_t p_end = s.find_first_of(" )", p + 6);
                        index_name = (p_end != std::string::npos) ? s.substr(p + 6, p_end - (p + 6)) : s.substr(p + 6);
                    }
                }
            }
        }
        sqlite3_finalize(stmt);
    }
    auto res = std::make_tuple(lines, index_used, index_name);
    plan_cache_[key] = res;
    return res;
}

std::string SableDB::apply_plan_hint(const std::string& sql, const std::string& plan) {
    if (plan != "index_lookup" || !indexes_.count("idx_sable_key")) return sql;
    auto it = plan_hint_cache_.find(sql);
    if (it != plan_hint_cache_.end()) return it->second;

    std::string low = sql;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return std::tolower(c); });

    size_t pos = low.find("from sable_records");
    if (pos == std::string::npos) return sql;

    std::string modified = sql.substr(0, pos + 18) + " INDEXED BY idx_sable_key" + sql.substr(pos + 18);
    plan_hint_cache_[sql] = modified;
    return modified;
}

bool SableDB::create_user(const std::string& username, const std::string& password, const std::string& role) {
    return security.create_user(username, password, role);
}

bool SableDB::authenticate(const std::string& username, const std::string& password) {
    return security.authenticate(username, password);
}

bool SableDB::grant(const std::string& grantee, const std::string& table, const std::string& action) {
    return security.grant(grantee, table, action);
}

bool SableDB::revoke(const std::string& grantee, const std::string& table, const std::string& action) {
    return security.revoke(grantee, table, action);
}

bool SableDB::add_rule(const std::string& name, const std::string& table, const std::string& expr, const std::string& type) {
    return security.add_rule(name, table, type, expr);
}

bool SableDB::drop_rule(const std::string& name) {
    return security.drop_rule(name);
}

std::vector<std::string> SableDB::list_tables(bool include_internal) {
    std::vector<std::string> tbls;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* n = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (n) {
                std::string s = n;
                if (include_internal || s.rfind("_sable_", 0) != 0) {
                    tbls.push_back(s);
                }
            }
        }
        sqlite3_finalize(stmt);
    }
    return tbls;
}

std::string SableDB::table_schema(const std::string& table_name) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT sql FROM sqlite_master WHERE type='table' AND name = ?;";
    std::string schema;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, table_name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (s) schema = s;
        }
        sqlite3_finalize(stmt);
    }
    return schema;
}

std::vector<std::string> SableDB::list_indices(const std::string& table_name) {
    std::vector<std::string> indices;
    sqlite3_stmt* stmt = nullptr;
    std::string sql = table_name.empty()
        ? "SELECT name FROM sqlite_master WHERE type='index' AND name NOT LIKE 'sqlite_autoindex_%' ORDER BY name;"
        : "SELECT name FROM sqlite_master WHERE type='index' AND tbl_name=? AND name NOT LIKE 'sqlite_autoindex_%' ORDER BY name;";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (!table_name.empty()) {
            sqlite3_bind_text(stmt, 1, table_name.c_str(), -1, SQLITE_TRANSIENT);
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* n = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (n) indices.push_back(n);
        }
        sqlite3_finalize(stmt);
    }
    return indices;
}

std::string SableDB::dump_table(const std::string& table_name) {
    std::string out;
    std::string ddl = table_schema(table_name);
    if (ddl.empty()) return out;
    out += ddl + ";\n";

    sqlite3_stmt* stmt = nullptr;
    std::string q = "SELECT * FROM " + table_name + ";";
    if (sqlite3_prepare_v2(db_, q.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        int cols = sqlite3_column_count(stmt);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            out += "INSERT INTO " + table_name + " VALUES(";
            for (int c = 0; c < cols; ++c) {
                const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, c));
                if (val) {
                    std::string s = val;
                    std::string escaped;
                    for (char ch : s) {
                        if (ch == '\'') escaped += "''";
                        else escaped += ch;
                    }
                    out += "'" + escaped + "'";
                } else {
                    out += "NULL";
                }
                if (c + 1 < cols) out += ", ";
            }
            out += ");\n";
        }
        sqlite3_finalize(stmt);
    }
    return out;
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
    if (!security.check_permission(security.current_user, "sable_records", "INSERT")) {
        throw std::runtime_error("Permission denied: user '" + security.current_user + "' lacks 'INSERT' permission on table 'sable_records'");
    }
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

QueryResult SableDB::execute_single(const std::string& sql, const std::vector<std::string>& params) {
    QueryResult res;
    res.sql = sql;

    if (security.handle_custom_command(sql, res)) {
        return res;
    }

    auto [action, table] = extract_action_and_table(sql);
    res.action = action;
    res.table = table;

    if (!security.check_permission(security.current_user, table, action)) {
        throw std::runtime_error("Permission denied: user '" + security.current_user + "' lacks '" + action + "' permission on table '" + table + "'");
    }
    std::string violation;
    if (!security.check_data_rules(table, sql, violation)) {
        throw std::runtime_error("Data rule violation: " + violation);
    }

    auto start = std::chrono::steady_clock::now();
    std::string op = action;
    std::transform(op.begin(), op.end(), op.begin(), [](unsigned char c) { return std::tolower(c); });

    std::string plan = "scan";
    std::vector<std::string> candidates = {"scan"};
    bool cache_hit = false;
    if (op == "insert" || op == "update" || op == "delete" || op == "replace" || op == "admin") {
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
    if (rc != SQLITE_OK) {
        std::string err_msg = db_ ? sqlite3_errmsg(db_) : "sqlite error";
        throw std::runtime_error("SQL error: " + err_msg);
    }

    for (size_t i = 0; i < params.size(); ++i) {
        sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
    }

    int col_count = sqlite3_column_count(stmt);
    res.is_query = (col_count > 0);
    for (int c = 0; c < col_count; ++c) {
        const char* name = sqlite3_column_name(stmt, c);
        res.column_names.push_back(name ? name : "");
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        Row row;
        for (int c = 0; c < col_count; ++c) {
            const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, c));
            row[res.column_names[c]] = val ? val : "";
        }
        res.rows.push_back(std::move(row));
    }
    res.changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);

    security.apply_masking(table, res.column_names, res.rows);

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    query_id_++;
    int64_t scanned = std::max<int64_t>(res.changes > 0 ? res.changes : res.rows.size(), res.rows.size());

    Telemetry t;
    t.query_id = query_id_;
    t.fingerprint = hash_fingerprint(sql);
    t.plan = plan;
    t.rows_scanned = scanned;
    t.rows_returned = res.rows.size();
    t.latency_us = std::max<int64_t>(1, elapsed);
    t.index_used = index_used;
    t.phase = last_phase_;
    t.epoch = epoch_;
    t.operation = op;
    t.cache_hit = cache_hit;
    t.bytes_read = res.rows.size() * 64;
    t.candidate_plans = candidates;
    t.sqlite_plan = sq_plan;
    t.index_name = index_name;

    record_telemetry(t);
    cost_model.observe(scanned, t.latency_us, index_used, plan == "join", plan == "aggregate");
    persist_state();
    return res;
}

std::vector<QueryResult> SableDB::execute_script(const std::string& script) {
    std::vector<QueryResult> results;
    auto stmts = split_statements(script);
    for (const auto& s : stmts) {
        if (s.empty()) continue;
        results.push_back(execute_single(s));
    }
    return results;
}

std::vector<Row> SableDB::execute(const std::string& sql, const std::vector<std::string>& params) {
    if (!params.empty()) {
        return execute_single(sql, params).rows;
    }
    auto results = execute_script(sql);
    if (results.empty()) return {};
    for (auto it = results.rbegin(); it != results.rend(); ++it) {
        if (!it->rows.empty()) return it->rows;
    }
    return results.back().rows;
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
