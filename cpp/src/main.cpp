#include "sable.hpp"
#include <iostream>
#include <chrono>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "SABLE v3.2 Autonomous Database Engine\n"
                  << "Usage: sable <command> [args...]\n\n"
                  << "Commands:\n"
                  << "  put <db> <key> <val>       Store a key-value record\n"
                  << "  get <db> <key>             Retrieve a value by key\n"
                  << "  query <db> <sql>           Execute an arbitrary SQL query\n"
                  << "  bulk <db> <count>          Bulk ingest sequential records\n"
                  << "  status <db>                Display autonomous engine state\n"
                  << "  bench <db> [count]         Run internal microbenchmark\n"
                  << "  archive <db> [limit]       Offload cold records to segment store\n"
                  << "  --version                  Print version information\n";
        return 1;
    }

    std::string cmd = argv[1];
    if (cmd == "--version" || cmd == "-v") {
        std::cout << "sable version 3.2.0 (cpp native release)\n";
        return 0;
    }

    if (cmd == "put" && argc >= 5) {
        sable::SableDB db(argv[2]);
        db.put(argv[3], argv[4]);
        std::cout << "{\"status\":\"ok\",\"key\":\"" << argv[3] << "\"}\n";
        return 0;
    }

    if (cmd == "get" && argc >= 4) {
        sable::SableDB db(argv[2]);
        std::string val = db.get(argv[3]);
        if (!val.empty()) {
            std::cout << val << "\n";
            return 0;
        } else {
            std::cerr << "key not found\n";
            return 1;
        }
    }

    if (cmd == "query" && argc >= 4) {
        sable::SableDB db(argv[2]);
        auto rows = db.execute(argv[3]);
        std::cout << "[\n";
        for (size_t i = 0; i < rows.size(); ++i) {
            std::cout << "  {";
            bool first = true;
            for (const auto& kv : rows[i]) {
                if (!first) std::cout << ", ";
                std::cout << "\"" << sable::json_escape(kv.first) << "\": \"" << sable::json_escape(kv.second) << "\"";
                first = false;
            }
            std::cout << "}" << (i + 1 < rows.size() ? ",\n" : "\n");
        }
        std::cout << "]\n";
        return 0;
    }

    if (cmd == "bulk" && argc >= 4) {
        sable::SableDB db(argv[2]);
        int count = std::stoi(argv[3]);
        std::vector<std::pair<std::string, std::string>> items;
        items.reserve(count);
        for (int i = 0; i < count; ++i) {
            items.push_back({"k" + std::to_string(i), "{\"id\":" + std::to_string(i) + "}"});
        }
        int inserted = db.bulk_put(items);
        std::cout << "{\"inserted\":" << inserted << "}\n";
        return 0;
    }

    if (cmd == "status" && argc >= 3) {
        sable::SableDB db(argv[2]);
        std::cout << db.status_json() << "\n";
        return 0;
    }

    if (cmd == "archive" && argc >= 3) {
        sable::SableDB db(argv[2]);
        int limit = (argc >= 4) ? std::stoi(argv[3]) : 1000;
        int archived = db.archive_cold_records(limit);
        std::cout << "{\"archived\":" << archived << "}\n";
        return 0;
    }

    if (cmd == "bench") {
        std::string db_path = (argc >= 3) ? argv[2] : ":memory:";
        int count = (argc >= 4) ? std::stoi(argv[3]) : 10000;
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

    std::cerr << "unknown command: " << cmd << "\n";
    return 1;
}
