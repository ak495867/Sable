# SABLE Database User Guide (USE.md)

This comprehensive guide explains how anyone can create, manage, query, secure, and autonomously optimize databases using SABLE.

---

## 1. Overview

SABLE is a self-optimizing, embedded database engine with native SQLite CLI parity, built-in Role-Based Access Control (RBAC), fine-grained data security rules, and autonomous query planning.

- **Zero Configuration**: Single executable (`sable` or `sable.exe`), zero runtime dependencies.
- **SQLite Parity**: Interactive REPL, script execution, piped stdin, and ASCII table formatting.
- **Code Rules & Permissions of Data**: Define users, roles, grants, column data masking, and table mutation locks directly in SQL.
- **Autonomous Optimization**: Online multivariate cost modeling, LinUCB contextual bandit join planning, safe index proposal tracking, and zero-risk rollbacks.

---

## 2. Installation

### 1-Line Remote Installer

#### Windows (PowerShell)
```powershell
irm https://raw.githubusercontent.com/ak495867/Sable/main/scripts/install.ps1 | iex
```

#### Linux / macOS (Shell)
```bash
curl -fsSL https://raw.githubusercontent.com/ak495867/Sable/main/scripts/install.sh | bash
```

### Build From Source
```bash
cmake -B cpp/build -S cpp -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build --config Release
./cpp/build/sable --version
```

---

## 3. Creating and Connecting to a Database

### Create or Open a Database File
```bash
sable production.db
```
If `production.db` does not exist, SABLE automatically creates it with:
- SQLite Write-Ahead Logging (`PRAGMA journal_mode=WAL`)
- In-memory temporary storage (`PRAGMA temp_store=MEMORY`)
- 64 MB page cache (`PRAGMA cache_size=-64000`)
- Built-in security tables (`_sable_users`, `_sable_permissions`, `_sable_rules`)
- Default `admin` account with full system permissions

### In-Memory Database
```bash
sable :memory:
```
Or simply run:
```bash
sable
```

---

## 4. Writing Code in SQL (CLI & REPL)

SABLE features an interactive shell that accepts arbitrary SQL statements and dot-commands.

### Interactive SQL Execution
```text
sable [admin]> CREATE TABLE users (id INT, name TEXT, email TEXT, salary REAL);
sable [admin]> INSERT INTO users VALUES (1, 'Alice', 'alice@corp.com', 125000.0);
sable [admin]> INSERT INTO users VALUES (2, 'Bob', 'bob@corp.com', 95000.0);
sable [admin]> SELECT * FROM users;
id | name  | email          | salary  
---+-------+----------------+---------
1  | Alice | alice@corp.com | 125000.0
2  | Bob   | bob@corp.com   | 95000.0 
(2 rows)
```

Multi-line statements remain buffered until terminated with a semicolon `;`:
```text
sable [admin]> SELECT id, name
   ...> FROM users
   ...> WHERE salary > 100000;
id | name 
---+------
1  | Alice
(1 row)
```

### Direct CLI SQL Execution
Execute statements directly without entering the interactive shell:
```bash
sable production.db "SELECT id, name FROM users WHERE id = 1;"
```

Execute multi-statement scripts in sequence:
```bash
sable production.db "CREATE TABLE items (id INT, name TEXT); INSERT INTO items VALUES (1, 'widget'); SELECT * FROM items;"
```

### Piped Input and Batch Scripting
Execute entire SQL scripts via file redirection or pipe:
```bash
sable production.db < schema.sql
```
```bash
cat migration.sql | sable production.db
```

---

## 5. Output Formatting Modes and Dot-Commands

### Output Formatting Flags
Format query results for terminals, pipelines, or JSON consumers:

- `-table`: Aligned ASCII box table with column headers.
- `-column`: Clean column alignment.
- `-json`: Formatted JSON array of objects.
- `-csv`: Comma-separated values with quoted strings.
- `-tabs`: Tab-delimited values.
- `-line`: One line per column (`key = value`).
- `-list`: Pipe-delimited values (`val1|val2|val3`).
- `-timer`: Prints real query execution time in milliseconds.
- `-bail`: Halts batch scripts immediately on first error.
- `-echo`: Echoes statements before execution.

#### Examples:
```bash
sable -json production.db "SELECT * FROM users;"
```
Output:
```json
[
  {"email": "alice@corp.com", "id": "1", "name": "Alice", "salary": "125000.0"},
  {"email": "bob@corp.com", "id": "2", "name": "Bob", "salary": "95000.0"}
]
```

```bash
sable -csv production.db "SELECT * FROM users;"
```
Output:
```csv
"id","name","email","salary"
"1","Alice","alice@corp.com","125000.0"
"2","Bob","bob@corp.com","95000.0"
```

```bash
sable -table -timer production.db "SELECT COUNT(*) FROM users;"
```

### Dot-Commands Reference

| Dot-Command | Description |
| :--- | :--- |
| `.help` | Display interactive help manual |
| `.tables` | List all user tables in database |
| `.schema [table]` | Display CREATE TABLE DDL |
| `.indices [table]` | Display active indices |
| `.dump [table]` | Dump table schema and rows as SQL INSERT statements |
| `.mode <type>` | Switch mode (`table`, `column`, `csv`, `tabs`, `line`, `json`, `list`) |
| `.headers on\|off` | Toggle column headers |
| `.timer on\|off` | Toggle elapsed execution timer |
| `.bail on\|off` | Toggle stop on error |
| `.echo on\|off` | Toggle SQL echo |
| `.read <file>` | Read and execute external SQL script file |
| `.output <file\|stdout>` | Redirect query output to file or back to stdout |
| `.exit` / `.quit` | Exit shell |

---

## 6. Data Security and Role-Based Access Control (RBAC)

SABLE embeds role-based access control inside the database catalog. Permissions can be written directly in SQL or administered via dot-commands.

### User Management in SQL
```sql
CREATE USER analyst IDENTIFIED BY 'analyst_pass' ROLE 'analyst';
CREATE USER auditor IDENTIFIED BY 'auditor_pass' ROLE 'readonly';
ALTER USER analyst IDENTIFIED BY 'new_secret_pass';
DROP USER auditor;
SHOW USERS;
```

### Granting and Revoking Permissions
Supported permissions: `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `ADMIN`, `ALL`.

```sql
GRANT SELECT ON users TO analyst;
GRANT SELECT, INSERT ON orders TO analyst;
REVOKE INSERT ON orders FROM analyst;
SHOW PERMISSIONS;
```

### Switching User Session (Authentication)
Authenticate within the shell or script:
```sql
AUTH analyst analyst_pass;
```
Or use the dot-command:
```text
.auth analyst analyst_pass
```

### Permission Enforcement
If user `analyst` attempts an unauthorized mutation:
```sql
AUTH analyst analyst_pass;
DELETE FROM users WHERE id = 1;
```
Result:
```text
Error: Permission denied: user 'analyst' lacks 'DELETE' permission on table 'users'
```

---

## 7. Data Pattern & Privacy Rules

SABLE allows developers and administrators to attach programmable rules to data tables.

### 1. Data Masking (`MASK`)
Automatically masks sensitive columns (e.g. salaries, SSNs, credit cards, emails) when queried by non-admin users:
```sql
CREATE RULE mask_user_pii ON users MASK 'salary, email';
```
When queried by user `analyst`:
```sql
SELECT id, name, email, salary FROM users;
```
Output:
```text
id | name  | email       | salary      
---+-------+-------------+-------------
1  | Alice | ***MASKED*** | ***MASKED***
2  | Bob   | ***MASKED*** | ***MASKED***
(2 rows)
```
When queried by user `admin`, original plaintext values are returned.

### 2. Mutation Lock (`READONLY`)
Locks tables (e.g. audit logs, ledger entries, historical archives) against modification:
```sql
CREATE RULE lock_audit_trail ON audit_log READONLY;
```
Attempting an `INSERT`, `UPDATE`, `DELETE`, or `DROP` triggers an immediate error:
```text
Error: Data rule violation: table 'audit_log' is marked READONLY by rule 'lock_audit_trail'
```

### 3. Query & Keyword Forbidding (`FORBID`)
Restricts queries matching specific substrings, keywords, or patterns:
```sql
CREATE RULE forbid_drop ON users FORBID 'drop';
CREATE RULE forbid_truncate ON users FORBID 'truncate';
```
Any statement matching the forbidden pattern is halted before execution:
```text
Error: Data rule violation: rule 'forbid_drop' triggered by pattern: drop
```

### Inspecting and Removing Rules
```sql
SHOW RULES;
DROP RULE mask_user_pii;
```
Or via dot-commands:
```text
.rules
.rule drop mask_user_pii
```

---

## 8. Autonomous Optimization & Self-Tuning

SABLE continuously monitors incoming queries and adapts physical index topology and join strategies.

### Workload Observation
The workload model tracks query patterns across shifting phases:
- `point_lookup`: High proportion of primary key / single-record lookups.
- `write_heavy`: Ingestion bursts exceeding 50% of recent traffic.
- `analytical`: Heavy aggregations and grouped queries.
- `bulk_scan`: Table scans across large ranges.

### Contextual Bandit Join Planning
A LinUCB multi-armed contextual bandit evaluates input cardinality and feature context to select among:
- `nested_loop`
- `hash`
- `merge`

### Proposal Lifecycle
SABLE never mutates physical indices without tracking. Index additions and retirements are proposed with reasons:
```text
sable [admin]> .status
{"epoch":12,"policy_mode":"heuristic","telemetry_events":500,"active_indexes":["idx_sable_key"],"workload":{"total":500,"point":420,"scan":80,"write":0,"phase":"point_lookup","p50_latency_us":10.0,"p95_latency_us":32.0}}
```

### Approving and Rolling Back Adaptations
```text
sable [admin]> .proposals
sable [admin]> .approve
Approved: create_index (committed)

sable [admin]> .rollback
Rolled back last adaptation
```

### Cold Tier Segment Archiving
Offload cold historical records from primary tables into compressed segment logs:
```text
sable [admin]> .archive 5000
Archived 5000 records to cold store
```

### Exporting Telemetry
Export execution telemetry for offline benchmarking and analytics:
```text
sable [admin]> .export telemetry.jsonl
Telemetry exported to: telemetry.jsonl
```

---

## 9. Python API Integration

SABLE can also be imported and used directly in Python applications:

```python
from sable import SableDB

db = SableDB("production.db", epoch_size=100)

db.put("config:cache_timeout", "300")
val = db.get("config:cache_timeout")
print("Timeout:", val)

db.execute("CREATE TABLE IF NOT EXISTS metrics (id INT, latency REAL);")
db.execute("INSERT INTO metrics VALUES (1, 14.5);")

rows = db.execute("SELECT * FROM metrics;")
for r in rows:
    print(r)

db.adaptive.enable_policy("linucb")

print(db.adaptive.status())
db.close()
```

---

## 10. Production Best Practices

1. **Use WAL Mode for High Concurrency**: SABLE enables WAL mode by default, supporting non-blocking concurrent reads during active writes.
2. **Apply Principle of Least Privilege**: Create application-specific users with `CREATE USER` and grant only required table actions (`SELECT`, `INSERT`).
3. **Protect Sensitive Fields**: Place `MASK` rules on sensitive columns such as tokens, passwords, emails, and financial identifiers.
4. **Lock Reference & Audit Tables**: Mark append-only audit tables as `READONLY` to prevent accidental deletion.
5. **Periodic Cold Archiving**: Schedule cold archiving (`.archive`) on write-intensive tables to maintain microsecond index lookups.
