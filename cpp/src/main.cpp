#include "sable.hpp"
#include <iostream>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>

#if defined(_WIN32)
#include <io.h>
#define IS_TTY() _isatty(_fileno(stdin))
#else
#include <unistd.h>
#define IS_TTY() isatty(fileno(stdin))
#endif

struct ShellConfig {
    std::string mode = "table";
    bool headers = true;
    bool timer = false;
    bool bail = false;
    bool echo = false;
    std::string output_path;
};

static void print_formatted(const sable::QueryResult& res, const ShellConfig& cfg, std::ostream& out) {
    if (!res.is_query) {
        if (res.changes > 0) {
            out << "(" << res.changes << " " << (res.changes == 1 ? "row" : "rows") << " affected)\n";
        }
        return;
    }

    const auto& cols = res.column_names;
    const auto& rows = res.rows;

    if (cfg.mode == "json") {
        out << "[\n";
        for (size_t i = 0; i < rows.size(); ++i) {
            out << "  {";
            for (size_t c = 0; c < cols.size(); ++c) {
                auto it = rows[i].find(cols[c]);
                std::string val = (it != rows[i].end()) ? it->second : "";
                out << "\"" << sable::json_escape(cols[c]) << "\": \"" << sable::json_escape(val) << "\"";
                if (c + 1 < cols.size()) out << ", ";
            }
            out << "}" << (i + 1 < rows.size() ? ",\n" : "\n");
        }
        out << "]\n";
        return;
    }

    if (cfg.mode == "csv") {
        if (cfg.headers && !cols.empty()) {
            for (size_t c = 0; c < cols.size(); ++c) {
                out << "\"" << cols[c] << "\"";
                if (c + 1 < cols.size()) out << ",";
            }
            out << "\n";
        }
        for (const auto& r : rows) {
            for (size_t c = 0; c < cols.size(); ++c) {
                auto it = r.find(cols[c]);
                std::string val = (it != r.end()) ? it->second : "";
                std::string escaped;
                for (char ch : val) {
                    if (ch == '"') escaped += "\"\"";
                    else escaped += ch;
                }
                out << "\"" << escaped << "\"";
                if (c + 1 < cols.size()) out << ",";
            }
            out << "\n";
        }
        return;
    }

    if (cfg.mode == "tabs") {
        if (cfg.headers && !cols.empty()) {
            for (size_t c = 0; c < cols.size(); ++c) {
                out << cols[c];
                if (c + 1 < cols.size()) out << "\t";
            }
            out << "\n";
        }
        for (const auto& r : rows) {
            for (size_t c = 0; c < cols.size(); ++c) {
                auto it = r.find(cols[c]);
                out << ((it != r.end()) ? it->second : "");
                if (c + 1 < cols.size()) out << "\t";
            }
            out << "\n";
        }
        return;
    }

    if (cfg.mode == "list") {
        if (cfg.headers && !cols.empty()) {
            for (size_t c = 0; c < cols.size(); ++c) {
                out << cols[c];
                if (c + 1 < cols.size()) out << "|";
            }
            out << "\n";
        }
        for (const auto& r : rows) {
            for (size_t c = 0; c < cols.size(); ++c) {
                auto it = r.find(cols[c]);
                out << ((it != r.end()) ? it->second : "");
                if (c + 1 < cols.size()) out << "|";
            }
            out << "\n";
        }
        return;
    }

    if (cfg.mode == "line") {
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i > 0) out << "\n";
            for (const auto& c : cols) {
                auto it = rows[i].find(c);
                out << std::setw(15) << std::left << c << " = " << ((it != rows[i].end()) ? it->second : "") << "\n";
            }
        }
        return;
    }

    if (rows.empty()) {
        if (cfg.headers) {
            for (size_t c = 0; c < cols.size(); ++c) {
                out << cols[c];
                if (c + 1 < cols.size()) out << " | ";
            }
            if (!cols.empty()) out << "\n";
        }
        out << "(0 rows)\n";
        return;
    }

    std::unordered_map<std::string, size_t> widths;
    for (const auto& c : cols) widths[c] = c.size();
    for (const auto& r : rows) {
        for (const auto& c : cols) {
            auto it = r.find(c);
            if (it != r.end()) widths[c] = std::max(widths[c], it->second.size());
        }
    }

    if (cfg.headers) {
        for (size_t i = 0; i < cols.size(); ++i) {
            out << std::left << std::setw(widths[cols[i]]) << cols[i];
            if (i + 1 < cols.size()) out << " | ";
        }
        out << "\n";

        for (size_t i = 0; i < cols.size(); ++i) {
            out << std::string(widths[cols[i]], '-');
            if (i + 1 < cols.size()) out << "-+-";
        }
        out << "\n";
    }

    for (const auto& r : rows) {
        for (size_t i = 0; i < cols.size(); ++i) {
            auto it = r.find(cols[i]);
            std::string val = (it != r.end()) ? it->second : "";
            out << std::left << std::setw(widths[cols[i]]) << val;
            if (i + 1 < cols.size()) out << " | ";
        }
        out << "\n";
    }

    out << "(" << rows.size() << " " << (rows.size() == 1 ? "row" : "rows") << ")\n";
}

static void print_help() {
    std::cout << "SABLE Interactive Database Shell (SQLite-Compatible)\n\n"
              << "General Dot-Commands:\n"
              << "  .help                               Show this help manual\n"
              << "  .tables                             List tables in database\n"
              << "  .schema [table]                     Show CREATE TABLE statement(s)\n"
              << "  .indices [table]                    List database indices\n"
              << "  .dump [table]                       Dump table as SQL script\n"
              << "  .mode <table|column|csv|tabs|line|json|list> Set output formatting\n"
              << "  .headers on|off                     Toggle column headers\n"
              << "  .timer on|off                       Toggle execution timing\n"
              << "  .bail on|off                        Stop on first error\n"
              << "  .echo on|off                        Echo statements before executing\n"
              << "  .read <filename>                    Execute SQL script from file\n"
              << "  .output <filename|stdout>           Redirect output to file or stdout\n"
              << "  .exit / .quit                       Exit shell\n\n"
              << "Data Security & Access Control (RBAC):\n"
              << "  .auth <user> <pass>                 Authenticate as user\n"
              << "  .user create <user> <pass> [role]   Create a new database user\n"
              << "  .user alter <user> <new_pass>       Update user password\n"
              << "  .user drop <user>                   Delete a database user\n"
              << "  .user list                          List registered database users\n"
              << "  .grant <action> ON <tbl> TO <user>  Grant permissions (SELECT, INSERT, UPDATE, DELETE, ALL)\n"
              << "  .revoke <action> ON <tbl> FROM <u>  Revoke permissions\n"
              << "  .permissions                        List active table permissions\n\n"
              << "Data Pattern & Constraint Rules:\n"
              << "  .rule add <name> <tbl> <type> <exp> Add data rule (FORBID, READONLY, MASK, CHECK)\n"
              << "  .rule drop <name>                   Delete an existing data rule\n"
              << "  .rules                              List active data rules\n\n"
              << "Autonomous Engine Optimization:\n"
              << "  .status                             Show autonomous metrics and active indexes\n"
              << "  .policy [heuristic|bandit|linucb]   View or change indexing policy\n"
              << "  .proposals                          Show candidate index proposals\n"
              << "  .approve                            Approve pending physical adaptation\n"
              << "  .rollback                           Rollback last applied index\n"
              << "  .bench [count]                      Run microbenchmark\n"
              << "  .archive [limit]                    Archive cold records\n"
              << "  .export <file>                      Export telemetry log\n\n"
              << "SQL Statements:\n"
              << "  Standard SQL: SELECT, INSERT, UPDATE, DELETE, CREATE TABLE, DROP TABLE, etc.\n"
              << "  SABLE SQL:    CREATE USER, DROP USER, GRANT, REVOKE, CREATE RULE, DROP RULE,\n"
              << "                AUTH, SHOW USERS, SHOW PERMISSIONS, SHOW RULES, SHOW TABLES.\n";
}

static bool run_statement_batch(sable::SableDB& db, const std::string& sql, ShellConfig& cfg, std::ostream& out) {
    if (cfg.echo) {
        out << sql << "\n";
    }
    auto t0 = std::chrono::steady_clock::now();
    try {
        auto results = db.execute_script(sql);
        auto t1 = std::chrono::steady_clock::now();
        double elapsed_ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;

        for (const auto& res : results) {
            print_formatted(res, cfg, out);
        }
        if (cfg.timer) {
            out << "Run Time: real " << std::fixed << std::setprecision(3) << elapsed_ms << " ms\n";
        }
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return !cfg.bail;
    }
}

static void run_repl(sable::SableDB& db, const std::string& db_path, ShellConfig& cfg) {
    std::cout << "SABLE Interactive Database Shell (v4.0.0)\n"
              << "Connected to: " << db_path << "\n"
              << "Enter \".help\" for usage hints. Enter SQL statements ending with ';'.\n";

    std::string line;
    std::string sql_buffer;
    std::ofstream out_file_stream;

    while (true) {
        std::ostream& cur_out = cfg.output_path.empty() ? std::cout : out_file_stream;

        if (sql_buffer.empty()) {
            std::cout << "sable [" << db.security.current_user << "]> ";
        } else {
            std::cout << "   ...> ";
        }
        std::cout.flush();

        if (!std::getline(std::cin, line)) break;
        std::string trimmed = line;
        size_t first = trimmed.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        trimmed = trimmed.substr(first);

        if (sql_buffer.empty() && trimmed[0] == '.') {
            std::istringstream iss(trimmed);
            std::string dot_cmd;
            iss >> dot_cmd;

            if (dot_cmd == ".exit" || dot_cmd == ".quit") {
                break;
            } else if (dot_cmd == ".help") {
                print_help();
            } else if (dot_cmd == ".tables") {
                auto tbls = db.list_tables(false);
                for (const auto& t : tbls) cur_out << t << "\n";
            } else if (dot_cmd == ".schema") {
                std::string target;
                iss >> target;
                if (target.empty()) {
                    auto tbls = db.list_tables(false);
                    for (const auto& t : tbls) {
                        cur_out << db.table_schema(t) << ";\n\n";
                    }
                } else {
                    cur_out << db.table_schema(target) << ";\n";
                }
            } else if (dot_cmd == ".indices" || dot_cmd == ".indexes") {
                std::string target;
                iss >> target;
                auto idxs = db.list_indices(target);
                for (const auto& idx : idxs) cur_out << idx << "\n";
            } else if (dot_cmd == ".dump") {
                std::string target;
                iss >> target;
                if (target.empty()) {
                    auto tbls = db.list_tables(false);
                    for (const auto& t : tbls) {
                        cur_out << db.dump_table(t);
                    }
                } else {
                    cur_out << db.dump_table(target);
                }
            } else if (dot_cmd == ".mode") {
                std::string m;
                if (iss >> m) {
                    if (m == "table" || m == "column" || m == "csv" || m == "tabs" || m == "line" || m == "json" || m == "list") {
                        cfg.mode = m;
                        std::cout << "Output mode set to: " << m << "\n";
                    } else {
                        std::cerr << "Unknown mode: " << m << ". Valid: table, column, csv, tabs, line, json, list\n";
                    }
                } else {
                    std::cout << "Current mode: " << cfg.mode << "\n";
                }
            } else if (dot_cmd == ".headers" || dot_cmd == ".header") {
                std::string val;
                if (iss >> val) {
                    cfg.headers = (val == "on" || val == "1" || val == "yes" || val == "true");
                    std::cout << "Headers " << (cfg.headers ? "on" : "off") << "\n";
                } else {
                    std::cout << "Headers: " << (cfg.headers ? "on" : "off") << "\n";
                }
            } else if (dot_cmd == ".timer") {
                std::string val;
                if (iss >> val) {
                    cfg.timer = (val == "on" || val == "1" || val == "yes" || val == "true");
                    std::cout << "Timer " << (cfg.timer ? "on" : "off") << "\n";
                } else {
                    std::cout << "Timer: " << (cfg.timer ? "on" : "off") << "\n";
                }
            } else if (dot_cmd == ".bail") {
                std::string val;
                if (iss >> val) {
                    cfg.bail = (val == "on" || val == "1" || val == "yes" || val == "true");
                    std::cout << "Bail " << (cfg.bail ? "on" : "off") << "\n";
                }
            } else if (dot_cmd == ".echo") {
                std::string val;
                if (iss >> val) {
                    cfg.echo = (val == "on" || val == "1" || val == "yes" || val == "true");
                    std::cout << "Echo " << (cfg.echo ? "on" : "off") << "\n";
                }
            } else if (dot_cmd == ".output") {
                std::string target;
                if (iss >> target) {
                    if (target == "stdout") {
                        if (out_file_stream.is_open()) out_file_stream.close();
                        cfg.output_path.clear();
                        std::cout << "Output directed to stdout\n";
                    } else {
                        if (out_file_stream.is_open()) out_file_stream.close();
                        out_file_stream.open(target, std::ios::out | std::ios::app);
                        if (out_file_stream.is_open()) {
                            cfg.output_path = target;
                            std::cout << "Output directed to: " << target << "\n";
                        } else {
                            std::cerr << "Cannot open output file: " << target << "\n";
                        }
                    }
                }
            } else if (dot_cmd == ".read") {
                std::string filepath;
                if (iss >> filepath) {
                    std::ifstream f(filepath);
                    if (f.is_open()) {
                        std::string script((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                        run_statement_batch(db, script, cfg, cur_out);
                    } else {
                        std::cerr << "Cannot open script file: " << filepath << "\n";
                    }
                }
            } else if (dot_cmd == ".auth") {
                std::string u, p;
                iss >> u >> p;
                if (db.authenticate(u, p)) {
                    std::cout << "Authenticated successfully as '" << u << "'\n";
                } else {
                    std::cerr << "Authentication failed for '" << u << "'\n";
                }
            } else if (dot_cmd == ".user") {
                std::string sub;
                iss >> sub;
                if (sub == "create") {
                    std::string u, p, r = "user";
                    iss >> u >> p;
                    if (iss >> r) {}
                    if (db.create_user(u, p, r)) {
                        std::cout << "User '" << u << "' created with role '" << r << "'\n";
                    } else {
                        std::cerr << "Failed to create user '" << u << "'\n";
                    }
                } else if (sub == "alter") {
                    std::string u, p;
                    iss >> u >> p;
                    if (db.security.alter_user(u, p)) {
                        std::cout << "User '" << u << "' password updated\n";
                    } else {
                        std::cerr << "Failed to alter user '" << u << "'\n";
                    }
                } else if (sub == "drop") {
                    std::string u;
                    iss >> u;
                    if (db.security.drop_user(u)) {
                        std::cout << "User '" << u << "' dropped\n";
                    } else {
                        std::cerr << "Failed to drop user '" << u << "'\n";
                    }
                } else if (sub == "list") {
                    auto res = db.execute_single("SELECT username, role, created_at FROM _sable_users;");
                    print_formatted(res, cfg, cur_out);
                }
            } else if (dot_cmd == ".grant") {
                std::string act, on_kw, tbl, to_kw, grantee;
                iss >> act >> on_kw >> tbl >> to_kw >> grantee;
                if (db.grant(grantee, tbl, act)) {
                    std::cout << "Granted " << act << " ON " << tbl << " TO " << grantee << "\n";
                } else {
                    std::cerr << "Grant failed\n";
                }
            } else if (dot_cmd == ".revoke") {
                std::string act, on_kw, tbl, from_kw, grantee;
                iss >> act >> on_kw >> tbl >> from_kw >> grantee;
                if (db.revoke(grantee, tbl, act)) {
                    std::cout << "Revoked " << act << " ON " << tbl << " FROM " << grantee << "\n";
                } else {
                    std::cerr << "Revoke failed\n";
                }
            } else if (dot_cmd == ".permissions") {
                auto res = db.execute_single("SELECT grantee, target_table, action, is_allowed FROM _sable_permissions;");
                print_formatted(res, cfg, cur_out);
            } else if (dot_cmd == ".rule") {
                std::string sub;
                iss >> sub;
                if (sub == "add") {
                    std::string name, tbl, type;
                    iss >> name >> tbl >> type;
                    std::string expr;
                    std::getline(iss, expr);
                    size_t p_first = expr.find_first_not_of(" \t");
                    if (p_first != std::string::npos) expr = expr.substr(p_first);
                    if (expr.size() >= 2 && (expr.front() == '\'' || expr.front() == '"')) {
                        expr = expr.substr(1, expr.size() - 2);
                    }
                    if (db.add_rule(name, tbl, expr, type)) {
                        std::cout << "Rule '" << name << "' added for table '" << tbl << "' (" << type << ")\n";
                    } else {
                        std::cerr << "Failed to add rule\n";
                    }
                } else if (sub == "drop") {
                    std::string name;
                    iss >> name;
                    if (db.drop_rule(name)) {
                        std::cout << "Rule '" << name << "' dropped\n";
                    } else {
                        std::cerr << "Failed to drop rule\n";
                    }
                }
            } else if (dot_cmd == ".rules") {
                auto res = db.execute_single("SELECT rule_name, target_table, rule_type, expression FROM _sable_rules;");
                print_formatted(res, cfg, cur_out);
            } else if (dot_cmd == ".status") {
                cur_out << db.status_json() << "\n";
            } else if (dot_cmd == ".policy") {
                std::string pol;
                if (iss >> pol) {
                    db.enable_policy(pol);
                    std::cout << "Policy updated to: " << pol << "\n";
                } else {
                    std::cout << "Current Policy: " << db.status_json() << "\n";
                }
            } else if (dot_cmd == ".proposals") {
                auto res = db.execute_single("SELECT * FROM _sable_state WHERE state_key='adaptive';");
                print_formatted(res, cfg, cur_out);
            } else if (dot_cmd == ".approve") {
                auto p = db.approve_proposal();
                std::cout << "Approved: " << p.type << " (" << p.status << ")\n";
            } else if (dot_cmd == ".rollback") {
                bool ok = db.rollback_last_adaptation();
                std::cout << (ok ? "Rolled back last adaptation\n" : "No adaptation to rollback\n");
            } else if (dot_cmd == ".bench") {
                int count = 5000;
                iss >> count;
                std::vector<std::pair<std::string, std::string>> b;
                b.reserve(count);
                for (int i = 0; i < count; ++i) b.push_back({"k" + std::to_string(i), std::to_string(i)});
                auto t0 = std::chrono::steady_clock::now();
                db.bulk_put(b);
                auto t1 = std::chrono::steady_clock::now();
                for (int i = 0; i < count; ++i) db.get("k" + std::to_string(i % count));
                auto t2 = std::chrono::steady_clock::now();
                double l_sec = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count() / 1000000.0;
                std::cout << "Benchmark (" << count << " ops): " << (count / (l_sec > 0 ? l_sec : 1e-6)) << " lookups/sec\n";
            } else if (dot_cmd == ".archive") {
                int limit = 1000;
                iss >> limit;
                int n = db.archive_cold_records(limit);
                std::cout << "Archived " << n << " records to cold store\n";
            } else if (dot_cmd == ".export") {
                std::string path;
                iss >> path;
                db.export_telemetry(path);
                std::cout << "Telemetry exported to: " << path << "\n";
            } else {
                std::cerr << "Unknown dot-command: " << dot_cmd << ". Type \".help\" for commands.\n";
            }
            continue;
        }

        if (!sql_buffer.empty()) sql_buffer += " ";
        sql_buffer += trimmed;

        if (!sql_buffer.empty() && sql_buffer.back() == ';') {
            run_statement_batch(db, sql_buffer, cfg, cur_out);
            sql_buffer.clear();
        }
    }

    if (out_file_stream.is_open()) {
        out_file_stream.close();
    }
}

int main(int argc, char* argv[]) {
    ShellConfig cfg;
    cfg.headers = true;
    cfg.mode = IS_TTY() ? "table" : "list";

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version" || a == "-v") {
            std::cout << "sable version 4.0.0 (cpp native release)\n";
            return 0;
        }
        if (a == "--help" || a == "-h") {
            std::cout << "SABLE v4.0 Autonomous Database Engine (SQLite-Compatible)\n"
                      << "Usage: sable [options] [db_file] [sql_statement]\n\n"
                      << "Options:\n"
                      << "  -header / -noheader        Enable or disable column headers\n"
                      << "  -table / -column           Tabular formatted output\n"
                      << "  -csv / -tabs / -list       Delimited data export\n"
                      << "  -line                      One line per column\n"
                      << "  -json                      JSON array output\n"
                      << "  -timer                     Print real elapsed time for queries\n"
                      << "  -bail                      Stop on first error\n"
                      << "  -echo                      Echo SQL before execution\n\n"
                      << "Execution Modes:\n"
                      << "  sable [db_file]            Interactive SQLite-compatible shell\n"
                      << "  sable <db> \"<sql>;\"        Execute SQL query directly\n"
                      << "  sable <db> < script.sql    Execute SQL script from stdin\n\n"
                      << "Convenience Commands:\n"
                      << "  sable put <db> <key> <val> Ingest key-value record\n"
                      << "  sable get <db> <key>       Retrieve record value\n"
                      << "  sable query <db> <sql>     Execute SQL and print JSON\n"
                      << "  sable bulk <db> <count>    Bulk ingest sequential records\n"
                      << "  sable status <db>          Show autonomous engine status\n"
                      << "  sable bench <db> [count]   Run throughput benchmark\n"
                      << "  sable archive <db> [limit] Offload cold records to segment store\n";
            return 0;
        }
        if (a == "-header") { cfg.headers = true; continue; }
        if (a == "-noheader") { cfg.headers = false; continue; }
        if (a == "-table") { cfg.mode = "table"; continue; }
        if (a == "-column") { cfg.mode = "column"; continue; }
        if (a == "-csv") { cfg.mode = "csv"; continue; }
        if (a == "-tabs") { cfg.mode = "tabs"; continue; }
        if (a == "-list") { cfg.mode = "list"; continue; }
        if (a == "-line") { cfg.mode = "line"; continue; }
        if (a == "-json") { cfg.mode = "json"; continue; }
        if (a == "-timer") { cfg.timer = true; continue; }
        if (a == "-bail") { cfg.bail = true; continue; }
        if (a == "-echo") { cfg.echo = true; continue; }
        args.push_back(a);
    }

    if (!args.empty() && args[0] == "put" && args.size() >= 4) {
        sable::SableDB db(args[1]);
        db.put(args[2], args[3]);
        std::cout << "{\"status\":\"ok\",\"key\":\"" << args[2] << "\"}\n";
        return 0;
    }
    if (!args.empty() && args[0] == "get" && args.size() >= 3) {
        sable::SableDB db(args[1]);
        std::string val = db.get(args[2]);
        if (!val.empty()) {
            std::cout << val << "\n";
            return 0;
        }
        std::cerr << "key not found\n";
        return 1;
    }
    if (!args.empty() && args[0] == "query" && args.size() >= 3) {
        sable::SableDB db(args[1]);
        cfg.mode = "json";
        run_statement_batch(db, args[2], cfg, std::cout);
        return 0;
    }
    if (!args.empty() && args[0] == "bulk" && args.size() >= 3) {
        sable::SableDB db(args[1]);
        int count = std::stoi(args[2]);
        std::vector<std::pair<std::string, std::string>> items;
        items.reserve(count);
        for (int i = 0; i < count; ++i) {
            items.push_back({"k" + std::to_string(i), "{\"id\":" + std::to_string(i) + "}"});
        }
        int inserted = db.bulk_put(items);
        std::cout << "{\"inserted\":" << inserted << "}\n";
        return 0;
    }
    if (!args.empty() && args[0] == "status" && args.size() >= 2) {
        sable::SableDB db(args[1]);
        std::cout << db.status_json() << "\n";
        return 0;
    }
    if (!args.empty() && args[0] == "archive" && args.size() >= 2) {
        sable::SableDB db(args[1]);
        int limit = (args.size() >= 3) ? std::stoi(args[2]) : 1000;
        int archived = db.archive_cold_records(limit);
        std::cout << "{\"archived\":" << archived << "}\n";
        return 0;
    }
    if (!args.empty() && args[0] == "bench") {
        std::string db_path = (args.size() >= 2) ? args[1] : ":memory:";
        int count = (args.size() >= 3) ? std::stoi(args[2]) : 10000;
        sable::SableDB db(db_path, 100);
        std::vector<std::pair<std::string, std::string>> batch;
        batch.reserve(count);
        for (int i = 0; i < count; ++i) {
            batch.push_back({"k" + std::to_string(i), "{\"x\":" + std::to_string(i) + "}"});
        }
        auto t0 = std::chrono::steady_clock::now();
        db.bulk_put(batch);
        auto t1 = std::chrono::steady_clock::now();
        double write_sec = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000000.0;
        for (int i = 0; i < count; ++i) {
            db.get("k" + std::to_string(i % count));
        }
        auto t2 = std::chrono::steady_clock::now();
        double lookup_sec = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count() / 1000000.0;
        double ops_sec = count / (lookup_sec > 0.0 ? lookup_sec : 1e-6);
        std::cout << "SABLE C++ Benchmark Results (" << count << " items)\n"
                  << "  Bulk Write Time : " << write_sec << " s\n"
                  << "  Point Lookup Time: " << lookup_sec << " s\n"
                  << "  Lookup Throughput: " << ops_sec << " ops/sec\n"
                  << "  Engine Status    : " << db.status_json() << "\n";
        return 0;
    }

    std::string db_path = ":memory:";
    if (!args.empty()) {
        db_path = args[0];
    }

    if (args.size() <= 1) {
        if (IS_TTY()) {
            sable::SableDB db(db_path);
            run_repl(db, db_path, cfg);
            return 0;
        } else {
            sable::SableDB db(db_path);
            std::string line, script;
            while (std::getline(std::cin, line)) {
                script += line + "\n";
            }
            if (!script.empty()) {
                bool ok = run_statement_batch(db, script, cfg, std::cout);
                return ok ? 0 : 1;
            }
            return 0;
        }
    }

    sable::SableDB db(db_path);
    std::string script;
    for (size_t i = 1; i < args.size(); ++i) {
        if (i > 1) script += " ";
        script += args[i];
    }
    bool ok = run_statement_batch(db, script, cfg, std::cout);
    return ok ? 0 : 1;
}
