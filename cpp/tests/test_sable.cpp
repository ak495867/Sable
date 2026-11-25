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

    std::cout << "all sable c++ tests passed successfully!\n";
    return 0;
}
