# Contributing to SABLE

Thank you for your interest in contributing to SABLE. This document provides guidelines for contributing to the SABLE autonomous embedded database engine.

---

## 1. Project Overview

SABLE is a self-optimizing, embedded database engine that adapts physical design, query planning, and storage tiering to live workload behavior.

- **Native C++ Engine (`cpp/`)**: C++20 core engine (`libsable`), interactive SQLite-compatible CLI (`sable`), and test suite (`test_sable`).
- **Python Framework (`sable/`)**: Python SDK with full adaptive feedback loop, hot/warm/cold storage tiering, and research benchmark suite.
- **Storage Kernel**: SQLite Write-Ahead Logging (WAL) mode with high-throughput concurrent access.

---

## 2. Development Setup

### Prerequisites
- Modern C++20 compiler (`clang++`, `g++`, or MSVC)
- CMake 3.20 or later
- Ninja build tool (recommended)
- Python 3.10 or later

### Building the Native C++ Engine
```bash
cmake -B cpp/build -S cpp -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build --config Release
```

The build produces:
- `cpp/build/libsable.a` (or `.lib`): Static library
- `cpp/build/sable` (or `sable.exe`): Interactive CLI executable
- `cpp/build/test_sable` (or `test_sable.exe`): Native unit test runner

---

## 3. Testing and Verification

Before submitting any code changes, ensure all tests pass cleanly.

### Running C++ Unit Tests
```bash
./cpp/build/test_sable
```
Expected output:
```text
running sable c++ unit tests...
  test_put_get passed
  test_bulk_put passed
  test_cost_model passed
  test_linucb_bandit passed
  test_shadow_validator passed
  test_index_lifecycle_and_rollback passed
  test_archive_cold_records passed
  test_rbac_permissions passed
  test_data_rules_and_masking passed
  test_sql_script_execution passed
  test_statement_splitter passed
all sable c++ tests passed successfully!
```

### Running Python Verification Tests
```bash
python verify.py
```
Expected output:
```text
Ran 33 tests in 0.35s
OK
```

### Running Microbenchmarks
```bash
./cpp/build/sable bench :memory: 10000
```

---

## 4. Coding Standards and Strict Rules

To maintain codebase consistency and performance:

1. **Strict Zero Comments Policy**:
   - Do NOT include comments (`//`, `/* */`, or `#`) in any code files across C++, CMake, Python, Shell, PowerShell, or YAML.
   - Code must be self-explanatory with clear, descriptive naming and structure.
2. **Modern C++20 Idioms**:
   - Use standard library containers, smart pointers (`std::unique_ptr`), `std::string_view`, and structured bindings.
   - Manage all SQLite resources (`sqlite3*`, `sqlite3_stmt*`) cleanly via RAII to prevent memory or descriptor leaks.
3. **Database Safety & WAL Concurrency**:
   - Ensure all write paths preserve transactional atomicity.
   - Never mutate physical indices silently; index lifecycle events must pass through proposal tracking and approval.
4. **Platform Portability**:
   - Ensure code compiles cleanly across Windows (`x86_64`), Linux (`x86_64`), and macOS (`arm64`).

---

## 5. Pull Request Guidelines

1. **Fork and Branch**: Create a feature branch from `main` (`feature/my-feature`).
2. **Verify Tests**: Confirm all 11 C++ unit tests and 33 Python tests pass without errors or warnings.
3. **Check Code Cleanliness**: Ensure no comments are introduced in any modified or added code files.
4. **Commit Message**: Use clear, imperative commit messages (e.g. `Implement index cardinality estimator for joins`).
5. **Open Pull Request**: Submit your pull request to the `main` branch with a clear summary of changes and test results.

---

## 6. Reporting Issues

When reporting bugs or proposing features on GitHub Issues:
- Provide a minimal reproducible SQL script or code snippet.
- Include OS version, compiler details, and binary build mode.
- Describe observed behavior vs expected behavior.
