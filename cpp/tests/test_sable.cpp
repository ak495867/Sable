#include "sable.hpp"
#include <iostream>
#include <cassert>

int main() {
    std::cout << "running sable c++ unit tests...\n";

    {
        sable::SableDB db(":memory:");
        db.put("key1", "{\"val\": 42}");
        std::string val = db.get("key1");
        assert(val == "{\"val\": 42}");
        std::cout << "  test_put_get passed\n";
    }

    {
        sable::SableDB db(":memory:");
        std::vector<std::pair<std::string, std::string>> batch;
        for (int i = 0; i < 50; ++i) {
            batch.push_back({"k" + std::to_string(i), std::to_string(i)});
        }
        int inserted = db.bulk_put(batch);
        assert(inserted == 50);
        assert(db.get("k25") == "25");
        std::cout << "  test_bulk_put passed\n";
    }

    {
        sable::CostModel cm(100);
        for (int i = 0; i < 15; ++i) {
            cm.observe(1000, 10000, false, false, false);
            cm.observe(1000, 1000, true, false, false);
            cm.observe(1000, 25000, false, true, false);
        }
        assert(cm.index_penalty < 0.25);
        assert(cm.join_multiplier > 1.8);
        std::cout << "  test_cost_model passed\n";
    }

    {
        sable::LinUCBBandit bandit({"nested_loop", "hash", "merge"}, 0.5);
        std::vector<double> ctx_small = {1.0, 0.2, 0.2};
        std::vector<double> ctx_large = {1.0, 5.0, 5.0};
        for (int i = 0; i < 100; ++i) {
            bandit.update("nested_loop", ctx_small, -10.0);
            bandit.update("hash", ctx_small, -50.0);
            bandit.update("nested_loop", ctx_large, -2000.0);
            bandit.update("hash", ctx_large, -150.0);
        }
        std::string small_choice = bandit.select(ctx_small);
        std::string large_choice = bandit.select(ctx_large);
        assert(!small_choice.empty());
        assert(!large_choice.empty());
        std::cout << "  test_linucb_bandit passed\n";
    }

    {
        std::vector<double> baseline = {100.0, 105.0, 98.0, 102.0, 101.0, 99.0, 103.0};
        std::vector<double> candidate = {50.0, 52.0, 48.0, 51.0, 49.0, 53.0, 50.0};
        auto res = sable::ShadowValidator::compare(baseline, candidate);
        assert(res.validated);
        assert(res.speedup > 1.8);
        assert(res.t_stat > 2.0);
        std::cout << "  test_shadow_validator passed\n";
    }

    {
        sable::SableDB db(":memory:", 5);
        std::vector<std::pair<std::string, std::string>> batch;
        for (int i = 0; i < 20; ++i) {
            batch.push_back({"k" + std::to_string(i), std::to_string(i)});
        }
        db.bulk_put(batch);
        for (int i = 0; i < 40; ++i) {
            db.get("k" + std::to_string(i % 20));
        }
        auto proposal = db.approve_proposal();
        assert(proposal.status == "committed");
        assert(proposal.type == "create_index");

        for (int i = 0; i < 85; ++i) {
            db.put("w" + std::to_string(i), std::to_string(i));
        }
        db.adapt();
        auto drop_prop = db.approve_proposal();
        assert(drop_prop.type == "drop_index");
        assert(drop_prop.status == "committed");

        bool rolled_back = db.rollback_last_adaptation();
        assert(rolled_back);
        std::cout << "  test_index_lifecycle_and_rollback passed\n";
    }

    {
        sable::SableDB db(":memory:");
        db.put("doc1", "{\"v\": 1}");
        db.put("doc2", "{\"v\": 2}");
        int archived = db.archive_cold_records(5);
        assert(archived == 2);
        auto active = db.execute("SELECT COUNT(*) AS total FROM sable_records;");
        assert(!active.empty());
        assert(active[0]["total"] == "0");
        std::cout << "  test_archive_cold_records passed\n";
    }

    {
        sable::SableDB db(":memory:");
        bool ok = db.create_user("alice", "pass123", "analyst");
        assert(ok);
        assert(db.authenticate("alice", "pass123"));
        assert(!db.authenticate("alice", "wrong"));

        db.security.current_user = "admin";
        db.execute("CREATE TABLE metrics (id INT, score REAL);");
        db.execute("INSERT INTO metrics VALUES(1, 98.5);");

        db.security.current_user = "alice";
        bool denied = false;
        try {
            db.execute("SELECT * FROM metrics;");
        } catch (const std::exception& ex) {
            denied = true;
        }
        assert(denied);

        db.security.current_user = "admin";
        db.grant("alice", "metrics", "SELECT");

        db.security.current_user = "alice";
        auto rows = db.execute("SELECT * FROM metrics;");
        assert(rows.size() == 1);
        assert(rows[0]["score"] == "98.5");

        bool insert_denied = false;
        try {
            db.execute("INSERT INTO metrics VALUES(2, 75.0);");
        } catch (const std::exception& ex) {
            insert_denied = true;
        }
        assert(insert_denied);

        db.security.current_user = "admin";
        db.revoke("alice", "metrics", "SELECT");
        db.security.current_user = "alice";
        bool revoked = false;
        try {
            db.execute("SELECT * FROM metrics;");
        } catch (const std::exception& ex) {
            revoked = true;
        }
        assert(revoked);
        std::cout << "  test_rbac_permissions passed\n";
    }

    {
        sable::SableDB db(":memory:");
        db.execute("CREATE TABLE products (id INT, title TEXT, price REAL);");
        db.execute("INSERT INTO products VALUES (101, 'laptop', 1200.0);");
        db.add_rule("no_drop_products", "products", "drop", "FORBID");

        bool forbid_triggered = false;
        try {
            db.execute("DROP TABLE products;");
        } catch (const std::exception& ex) {
            forbid_triggered = true;
        }
        assert(forbid_triggered);

        db.add_rule("lock_products", "products", "READONLY", "READONLY");
        bool readonly_triggered = false;
        try {
            db.execute("INSERT INTO products VALUES (102, 'phone', 800.0);");
        } catch (const std::exception& ex) {
            readonly_triggered = true;
        }
        assert(readonly_triggered);

        db.create_user("bob", "secret", "user");
        db.grant("bob", "products", "SELECT");
        db.add_rule("mask_price", "products", "price", "MASK");

        db.security.current_user = "bob";
        auto rows = db.execute("SELECT * FROM products;");
        assert(!rows.empty());
        assert(rows[0]["price"] == "***MASKED***");
        assert(rows[0]["title"] == "laptop");
        std::cout << "  test_data_rules_and_masking passed\n";
    }

    {
        sable::SableDB db(":memory:");
        std::string script =
            "CREATE TABLE customers (id INT, name TEXT, email TEXT);\n"
            "INSERT INTO customers VALUES(1, 'Alice', 'alice@corp.com');\n"
            "INSERT INTO customers VALUES(2, 'Bob', 'bob@corp.com');\n"
            "CREATE USER analyst IDENTIFIED BY 'pwd123' ROLE 'analyst';\n"
            "GRANT SELECT ON customers TO analyst;\n"
            "CREATE RULE mask_email ON customers MASK 'email';\n"
            "AUTH analyst pwd123;\n"
            "SELECT id, name, email FROM customers ORDER BY id ASC;\n";

        auto results = db.execute_script(script);
        assert(!results.empty());
        assert(db.security.current_user == "analyst");
        const auto& last = results.back();
        assert(last.is_query);
        assert(last.rows.size() == 2);
        assert(last.column_names.size() == 3);
        assert(last.column_names[0] == "id");
        assert(last.column_names[1] == "name");
        assert(last.column_names[2] == "email");
        assert(last.rows[0].at("email") == "***MASKED***");
        assert(last.rows[1].at("email") == "***MASKED***");
        std::cout << "  test_sql_script_execution passed\n";
    }

    {
        sable::SableDB db(":memory:");
        auto stmts = sable::split_statements("SELECT 1; INSERT INTO t VALUES('hello; world'); SELECT 'done';");
        assert(stmts.size() == 3);
        assert(stmts[0] == "SELECT 1");
        assert(stmts[1] == "INSERT INTO t VALUES('hello; world')");
        assert(stmts[2] == "SELECT 'done'");
        std::cout << "  test_statement_splitter passed\n";
    }

    std::cout << "all sable c++ tests passed successfully!\n";
    return 0;
}
