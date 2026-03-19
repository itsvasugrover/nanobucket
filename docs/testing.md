# Testing & CLI Compatibility

## Overview

Testing is split into two layers:

- **Unit tests** — GoogleTest suites that verify individual components in isolation (no server, no network)
- **Integration tests** — `scripts/api-test.sh` runs `s3cmd` commands against a live server instance and asserts on exit codes and output

---

## File Map

```
test/
├── test_sigv4.cpp          — 29 tests: SigV4 crypto primitives + full verify() round-trips
├── test_meta_store.cpp     — 33 tests: SqliteMetaStore CRUD, pagination, delimiter folding
└── test_storage_engine.cpp — 30 tests: LocalFsStorageEngine CRUD, atomic writes, multipart

scripts/
└── api-test.sh             — 47 end-to-end s3cmd integration tests
```

Total: **92 unit tests**, **47 integration tests**.

---

## Unit Tests

### SigV4 Verifier — `test_sigv4.cpp` (29 tests)

Covers:

- **Crypto primitives**: `hmacSha256`, `sha256Hex` — verified against known vectors
- **AWS official test suite vectors** (`https://docs.aws.amazon.com/general/latest/gr/sigv4-test-suite.html`):
  - `get-header-key-duplicate` — duplicate header values
  - `get-header-value-order` — header ordering (case-insensitive sort)
  - `get-header-value-trim` — whitespace normalisation
  - `get-utf8` — UTF-8 encoded path
  - `post-sts-token` — STS session token in signed headers
  - `post-x-www-form-urlencoded` — form-encoded body
- **Full `verify()` round-trips**: construct a real `HttpRequest`, compute auth header with the correct key, call `verify()`, expect success
- **Negative cases**: wrong key, tampered body, missing headers → expect specific error codes

Key design: tests run against the raw `SigV4Verifier` class with synthetic `HttpRequest` objects — no Drogon server needed.

---

### Metadata Store — `test_meta_store.cpp` (33 tests)

All tests use an in-memory SQLite database (`:memory:`) — fast and leave no files on disk.

Test fixture:
```cpp
class MetaStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<SqliteMetaStore>(":memory:");
        ASSERT_TRUE(store_->init());
    }
    std::unique_ptr<SqliteMetaStore> store_;
};
```

Covers:

- **Bucket CRUD**: `createBucket`, `getBucket`, `deleteBucket`
  - 409 if bucket already exists
  - 404 if bucket missing
  - 409 `BucketNotEmpty` if objects remain
- **Object CRUD**: `putObject`, `getObject`, `deleteObject`
  - Overwrite (upsert) — last write wins
  - `getObject` returns `nullopt` for missing key
  - `deleteObject` is a no-op for missing key
- **ListObjectsV2 pagination**:
  - Insert 25 objects, query with `maxKeys=10` → 3 pages
  - Continuation token = `base64(first_excluded_key)`; page 2 resumes correctly
- **ListObjectsV2 delimiter folding**:
  - Keys: `photos/a.jpg`, `photos/b.jpg`, `photos/2024/c.jpg`
  - Query: `prefix="photos/"`, `delimiter="/"`
  - Expect: 2 `Contents`, 1 `CommonPrefix "photos/2024/"`
- **Multipart upload lifecycle**: create upload → add parts → list parts → delete upload (cascade)
- **Foreign key cascade**: deleting a multipart upload deletes all associated part records

---

### Storage Engine — `test_storage_engine.cpp` (30 tests)

All tests use a temporary directory created per-test and cleaned up in `TearDown`.

Covers:

- **Bucket directory management**: `createBucketDir`, `deleteBucketDir`
- **Object CRUD**: `put`, `get`, `remove`
  - Nested keys (`folder/sub/file.txt`) — intermediate directories created automatically
  - Atomic overwrite via `.tmp` + `rename(2)` — partial write never visible
  - `get` returns `false` for missing keys
- **ETag computation**: MD5 hex digest matches known vectors
- **Path traversal protection**: rejects `..` segments and absolute paths
- **Multipart part staging**: `putPart`, `getPart`, `assembleParts`, `removeParts`
  - Parts stored in `<dataRoot>/.multipart/<uploadId>/<partNumber>`
  - `assembleParts` concatenates parts in numeric order
  - Multipart ETag = `md5(concat(raw_md5_bytes_of_each_part))-N`
  - `removeParts` cleans up staging directory
- **Cascade cleanup**: `deleteBucketDir` removes all objects under the bucket

---

## Integration Tests — `scripts/api-test.sh`

47 end-to-end tests using `s3cmd` against a live nanobucket server.

### Features

- **s3cmd installation check** with install hints for Ubuntu/Debian, Fedora, Arch, macOS, pip
- **Server reachability check** (`curl -s --max-time 3`) — any HTTP response (including 403) is accepted as "server is up"
- **Auto-generated temp config** pointing at the target server, cleaned up on exit via `trap`
- **Unique timestamped bucket names** — avoids collisions across parallel runs
- **PASS/FAIL counters** with colored output; non-zero exit on any failure

### Options

| Flag | Default | Description |
|------|---------|-------------|
| `-H`, `--host` | `localhost` | Server hostname |
| `-p`, `--port` | `9000` | Server port |
| `-a`, `--access-key` | `nanobucketadmin` | Access key |
| `-s`, `--secret-key` | `nanobucketadmin` | Secret key |
| `--no-multipart` | off | Skip the 20 MB multipart test |
| `-v`, `--verbose` | off | Show full s3cmd output |

### Test Sections

| Section | Tests | What's Covered |
|---------|-------|----------------|
| Bucket ops | 4 | Create, duplicate-409, list, head |
| Put / Get / Head | 5 | Upload, download + content verify, head + size check |
| Nested keys | 4 | Upload to `folder/sub/file.txt`, list at each prefix level |
| CopyObject | 4 | Same-bucket copy, cross-bucket copy, content verified |
| DeleteObject | 2 | Delete key, idempotent re-delete (204 both times) |
| DeleteObjects | 3 | Bulk `POST ?delete`, verify keys gone, list empty |
| Bucket delete | 4 | Delete empty bucket, recreate it, fill it, delete non-empty (409) |
| Multipart | 14 | 20 MB upload via s3cmd (2 parts), SHA-256 round-trip, `ListParts`, abort with bad uploadId |
| Recursive delete | 7 | Populate bucket, `rb --recursive`, verify all gone |

### Usage

```bash
# Start the server
./scripts/start.sh

# Run with defaults (localhost:9000, nanobucketadmin/nanobucketadmin)
bash scripts/api-test.sh

# Run with verbose output
bash scripts/api-test.sh -v

# Run against a different host/port
bash scripts/api-test.sh -H 192.168.1.10 -p 9000

# Skip the slow 20 MB multipart test
bash scripts/api-test.sh --no-multipart
```

### Multipart Smoke Test

`s3cmd` automatically splits files larger than `multipart-chunk-size-mb` (default 15 MB). The 20 MB test covers the full multipart lifecycle:

1. `CreateMultipartUpload`
2. `UploadPart` × 2 (15 MB + 5 MB)
3. `CompleteMultipartUpload`
4. Download and `sha256sum` comparison — verifies byte-perfect round-trip
5. Abort test — sends `AbortMultipartUpload` with a bad uploadId, expects 404

---

## Running Unit Tests

```bash
# Build first (Conan + CMake)
bash scripts/build.sh

# Run all unit tests
bash scripts/test.sh

# Or run ctest directly from the build directory
cd build/Release
ctest --output-on-failure
```

The test binary is `build/Release/test/s3lite_tests`. Each GoogleTest suite can be filtered:

```bash
./build/Release/test/s3lite_tests --gtest_filter="SigV4*"
./build/Release/test/s3lite_tests --gtest_filter="MetaStore*"
./build/Release/test/s3lite_tests --gtest_filter="StorageEngine*"
```

---

## Known Gaps

| Area | Status |
|------|--------|
| `aws s3` CLI | Untested. s3cmd is the verified client. |
| `s3cmd sync` | Not in api-test.sh. Delta detection and recursive upload/download untested. |
| Presigned URL e2e | Verification logic implemented; not end-to-end tested via curl. |
| XML builder unit tests | No dedicated test file; XML correctness is covered transitively by the integration tests. |
