# Metadata Store

## Overview

The metadata store persists all structural information about buckets, objects, and multipart uploads. It does **not** store file bytes — only records describing what exists and where. The implementation is `SqliteMetaStore`, an embedded SQLite3 database running in WAL (Write-Ahead Logging) mode for concurrent read performance.

---

## File Map

```
src/meta/
└── SqliteMetaStore.h / .cpp
```

---

## SQLite Configuration

```sql
PRAGMA journal_mode = WAL;     -- concurrent readers don't block writers
PRAGMA synchronous  = NORMAL;  -- safe with WAL, faster than FULL
PRAGMA foreign_keys = ON;      -- enforce referential integrity
PRAGMA temp_store   = MEMORY;  -- temp tables in RAM
```

WAL mode is critical: Drogon's thread pool means multiple controllers can be reading while one is writing. Without WAL, every read blocks on a write lock.

---

## Schema

### `buckets`

```sql
CREATE TABLE IF NOT EXISTS buckets (
    name        TEXT     PRIMARY KEY,
    region      TEXT     NOT NULL DEFAULT 'us-east-1',
    created_at  INTEGER  NOT NULL   -- Unix epoch milliseconds
);
```

### `objects`

```sql
CREATE TABLE IF NOT EXISTS objects (
    bucket        TEXT     NOT NULL,
    key           TEXT     NOT NULL,
    size          INTEGER  NOT NULL DEFAULT 0,
    etag          TEXT     NOT NULL,
    content_type  TEXT     NOT NULL DEFAULT 'application/octet-stream',
    last_modified INTEGER  NOT NULL,   -- Unix epoch milliseconds
    PRIMARY KEY (bucket, key),
    FOREIGN KEY (bucket) REFERENCES buckets(name)
);

CREATE INDEX IF NOT EXISTS idx_objects_bucket_key
    ON objects (bucket, key);
```

The `(bucket, key)` primary key provides O(log n) lookups for `GetObject` / `HeadObject` and efficient prefix scans for `ListObjectsV2`.

### `multipart_uploads`

```sql
CREATE TABLE IF NOT EXISTS multipart_uploads (
    upload_id    TEXT     PRIMARY KEY,
    bucket       TEXT     NOT NULL,
    key          TEXT     NOT NULL,
    content_type TEXT     NOT NULL DEFAULT 'application/octet-stream',
    initiated_at INTEGER  NOT NULL,   -- Unix epoch milliseconds
    FOREIGN KEY (bucket) REFERENCES buckets(name)
);

CREATE INDEX IF NOT EXISTS idx_mpu_bucket
    ON multipart_uploads (bucket);
```

### `parts`

```sql
CREATE TABLE IF NOT EXISTS parts (
    upload_id    TEXT     NOT NULL,
    part_number  INTEGER  NOT NULL,
    etag         TEXT     NOT NULL,
    size         INTEGER  NOT NULL DEFAULT 0,
    last_modified INTEGER NOT NULL,
    PRIMARY KEY (upload_id, part_number),
    FOREIGN KEY (upload_id) REFERENCES multipart_uploads(upload_id)
        ON DELETE CASCADE
);
```

`ON DELETE CASCADE` means deleting a multipart upload record automatically removes all its part records — used on abort and complete.

---

## Key Query Patterns

### `ListObjectsV2` — Prefix + Delimiter

The most complex query in the store. The full S3 semantics:

1. Filter keys where `key LIKE prefix || '%'`
2. If no delimiter: return all matching objects as `Contents`
3. If delimiter is set:
   - Keys that contain the delimiter **after** the prefix are folded into `CommonPrefixes`
   - The prefix entry is the key up to and including the first delimiter occurrence after the prefix

Example: bucket contains `photos/2024/jan.jpg`, `photos/2024/feb.jpg`, `photos/cat.jpg`. Query with `prefix=photos/`, `delimiter=/`:
- `photos/cat.jpg` → `Contents` (no further `/` after prefix)
- `photos/2024/jan.jpg` and `photos/2024/feb.jpg` → folded into `CommonPrefixes` as `photos/2024/`

SQLite implementation strategy:
```sql
-- Get all keys with prefix, ordered for pagination
SELECT key, size, etag, content_type, last_modified
FROM objects
WHERE bucket = ? AND key >= ? AND key < ?   -- range scan using prefix + \xFF sentinel
ORDER BY key
LIMIT ?                                      -- maxKeys + 1 to detect truncation
OFFSET ?;                                    -- continuation token is an offset
```

Delimiter folding is done in C++ after the query — it's simpler than doing it in SQL and allows the same query plan regardless of delimiter presence.

### Continuation Token

The continuation token is a base64-encoded offset or the last key seen. Using the last-key approach is more robust (offset shifts if objects are deleted between pages):

```
token = base64_encode(last_key_on_page)
```

On next request, decode the token and use `WHERE key > last_key` instead of `OFFSET`.

### `deleteBucket` Guard

Before deleting a bucket, check for existing objects:

```sql
SELECT COUNT(*) FROM objects WHERE bucket = ?;
```

Return `BucketNotEmpty` if count > 0.

---

## `SqliteMetaStore` Implementation Sketch

```cpp
class SqliteMetaStore : public IMetaStore {
public:
    explicit SqliteMetaStore(const std::string& dbPath);

    // Opens / creates the database, runs PRAGMA config and CREATE TABLE IF NOT EXISTS.
    S3Error init();

    // IMetaStore overrides ...

private:
    sqlite3* db_{nullptr};

    // Prepared statement cache — avoids re-parsing SQL on every call
    sqlite3_stmt* stmtGetObject_{nullptr};
    sqlite3_stmt* stmtPutObject_{nullptr};
    // ...

    // Execute a statement and step through results with a callback
    template<typename RowCallback>
    S3Error query(sqlite3_stmt* stmt, RowCallback&& cb);
};
```

### Thread Safety

SQLite in WAL mode supports one writer + multiple concurrent readers. Drogon's thread pool will call `SqliteMetaStore` from multiple threads. Options:

1. **Single connection per thread** — use `thread_local` connection or connection pool. Cleanest for WAL mode.
2. **Single shared connection with mutex** — simpler but serialises all metadata access.

Initial implementation uses option 2 (mutex-protected single connection). The bottleneck is storage I/O, not metadata, so this is acceptable for v1.

---

## `uploadId` Generation

A UUID v4 (128 random bits) formatted as a lowercase hex string without dashes — matches the format used by MinIO and real S3:

```cpp
std::string generateUploadId() {
    // Read 16 random bytes from /dev/urandom via OpenSSL RAND_bytes
    unsigned char buf[16];
    RAND_bytes(buf, sizeof(buf));
    // Format as hex string
    std::ostringstream oss;
    for (auto b : buf) oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    return oss.str();
}
```

---

## Drogon Async Integration

`SqliteMetaStore` methods are synchronous (see [core-interfaces.md](core-interfaces.md) for rationale). In controllers, wrap blocking calls using `drogon::async_func` to avoid blocking the event loop thread:

```cpp
// Inside a Drogon controller handler
auto task = [bucket, key, this]() -> drogon::Task<> {
    auto record = co_await drogon::async_task([&]() {
        return metaStore_->getObject(bucket, key);
    });
    // build response ...
};
```

This keeps the event loop free to handle other requests while the SQLite call executes on a worker thread.
