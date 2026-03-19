# Changelog

All notable changes to NanoBucket are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [0.1.0] — 2026-03-19

Initial public release. A self-hosted, single-binary, S3-compatible object storage server in C++17. All 16 core S3 API operations are implemented and verified against `s3cmd`. The server passes 125 unit tests and 47 end-to-end integration tests.

---

### Build System

- CMake 3.15+ with C++17, `CMAKE_CXX_EXTENSIONS OFF`, `compile_commands.json` export
- Conan v2 dependency manifest: `drogon/1.9.12`, `spdlog/1.17.0`, `openssl/3.6.1`, `pugixml/1.15`, `sqlite3/3.51.0`, `gtest/1.14.0`
- `cmake_layout` places all generated files under `build/Release/generators/` — no double-nesting
- `scripts/setup.sh` — Conan v2-aware install; supports `--dry-run`, `--no-conan`, `--profile`
- `scripts/build.sh` — `cmake --preset conan-release` + `cmake --build`
- `scripts/start.sh` — launches binary; resolves port via flag → `NANOBUCKET_PORT` → default `9000`
- `scripts/clean.sh` — removes build artifacts; `--conan-cache` to purge `~/.conan2`
- `scripts/test.sh` — runs `ctest --output-on-failure`
- Graceful shutdown on `SIGINT` / `SIGTERM` via `drogon::app().quit()`

---

### Core Interfaces & Type System

- `include/nanobucket/common/Types.h` — shared value types: `BucketInfo`, `ObjectRecord`, `PartInfo`, `MultipartUploadInfo`, `ListObjectsResult`, `S3Error`
- `include/nanobucket/storage/IStorageEngine.h` — storage contract: bucket dirs, object r/w/delete, multipart staging + assembly
- `include/nanobucket/meta/IMetaStore.h` — metadata contract: bucket/object/multipart CRUD, `ListObjectsV2` query with prefix, delimiter, pagination
- `include/nanobucket/auth/ISigV4Verifier.h` — auth contract: `verify(req)` → `VerifyResult`; documents header-auth and presigned-URL flows

---

### XML Response Builder

- `src/xml/S3XmlBuilder.h/.cpp` — pugixml-based builder for all S3 XML schemas:
  - `listBuckets`, `listObjectsV2`, `initiateMultipartUpload`, `completeMultipartUpload`
  - `listParts`, `listMultipartUploads`, `copyObjectResult`
  - `deleteObjects` — `<DeleteResult>` with `<Deleted>` and `<Error>` entries
  - `error` — standard `<Error>` with `Code`, `Message`, `Key`, `BucketName`
- All responses include the `http://s3.amazonaws.com/doc/2006-03-01/` namespace

---

### AWS SigV4 Authentication

- Full AWS Signature Version 4 implementation via OpenSSL:
  - HMAC-SHA256 signing, SHA-256 payload hashing, RFC 3986 percent-encoding
  - 4-step signing key derivation: `DateKey → DateRegionKey → DateRegionServiceKey → SigningKey`
  - Canonical request assembly: method, URI, query string (sorted), headers, signed-headers, payload hash
  - String-to-sign: `AWS4-HMAC-SHA256` algorithm, timestamp, credential scope, canonical request hash
- Header-based auth (`Authorization:` header) and presigned URL (`X-Amz-*` query params) both supported
- `AwsSigV4Filter` Drogon `HttpFilter` applied to every route — returns `403`/`400` XML on failure
- `canonicalQueryStringFromRaw(rawQuery, presigned)` exposed as public static for direct unit testing

**Bugs fixed:**
- **HEAD→GET conversion**: Drogon internally converts `HEAD` to `GET` before invoking filters. Fixed by using `req->isHead()` to restore the original method in the canonical request.
- **Body pollution via `getParameters()`**: Drogon's `getParameters()` merges URL query params with parsed form-body content, corrupting the canonical query string for binary `PUT` requests. Fixed by using `req->getQuery()` (raw URL query string only) and parsing it manually.

---

### Metadata Store

- `src/meta/SqliteMetaStore.h/.cpp` — SQLite-backed `IMetaStore`:
  - WAL journal mode, `PRAGMA foreign_keys = ON`, cascade delete on multipart upload removal
  - Schema: `buckets`, `objects`, `multipart_uploads`, `parts`
  - 14 cached prepared statements to avoid repeated parse/compile overhead
  - Mutex-protected single connection — safe for Drogon's multi-threaded handlers
  - `ListObjectsV2`: prefix filter, delimiter folding into `CommonPrefixes`, max-keys, continuation-token
  - Continuation token = `base64(first_excluded_key)`

---

### Local Filesystem Storage Engine

- `src/storage/LocalFsStorageEngine.h/.cpp` — filesystem-backed `IStorageEngine`:
  - Layout: `<dataRoot>/<bucket>/<key>` — nested keys map directly to subdirectories
  - Multipart staging: `<dataRoot>/.multipart/<uploadId>/<partNumber>` (zero-padded 5 digits)
  - Atomic writes via `.tmp` + `rename(2)` — partial writes are never visible
  - MD5 ETag via OpenSSL `EVP_DigestUpdate`
  - Multipart ETag: `md5(concat(raw_md5_bytes_of_each_part))-<partCount>` — matches AWS format
  - Path traversal protection: rejects keys containing `..` segments or starting with `/`

---

### S3 API Controllers

All 16 core S3 operations implemented. `AwsSigV4Filter` applied to every route.

**BucketController** (`src/api/BucketController.h/.cpp`):

| Operation | Endpoint |
|-----------|----------|
| ListBuckets | `GET /` |
| CreateBucket | `PUT /{bucket}` — name validation, storage-first with meta rollback |
| DeleteBucket | `DELETE /{bucket}` — 404 if missing, 409 if non-empty |
| HeadBucket | `HEAD /{bucket}` |
| ListObjectsV2 | `GET /{bucket}?list-type=2` — prefix, delimiter, max-keys, continuation-token |
| ListMultipartUploads | `GET /{bucket}?uploads` |
| DeleteObjects | `POST /{bucket}?delete` — parses `<Delete>` XML, returns `<DeleteResult>` |
| GetBucketAcl | `GET /{bucket}?acl` — stub, FULL_CONTROL |
| PutBucketAcl | `PUT /{bucket}?acl` — stub, 200 OK |
| Sub-resources | `GET /{bucket}?cors\|policy\|lifecycle\|…` — 404 with correct S3 error codes |

**ObjectController** (`src/api/ObjectController.h/.cpp`):

| Operation | Endpoint |
|-----------|----------|
| PutObject | `PUT /{bucket}/{key}` |
| CopyObject | `PUT /{bucket}/{key}` + `x-amz-copy-source` header |
| UploadPart | `PUT /{bucket}/{key}?partNumber=N&uploadId=X` |
| PutObjectAcl | `PUT /{bucket}/{key}?acl` — stub, 200 OK |
| GetObject | `GET /{bucket}/{key}` — zero-copy via `newFileResponse` |
| ListParts | `GET /{bucket}/{key}?uploadId=X` |
| GetObjectAcl | `GET /{bucket}/{key}?acl` — stub, FULL_CONTROL |
| HeadObject | `HEAD /{bucket}/{key}` |
| DeleteObject | `DELETE /{bucket}/{key}` — always 204, idempotent |
| AbortMultipartUpload | `DELETE /{bucket}/{key}?uploadId=X` |
| CreateMultipartUpload | `POST /{bucket}/{key}?uploads` |
| CompleteMultipartUpload | `POST /{bucket}/{key}?uploadId=X` — validates parts, assembles, computes multipart ETag |

**ApiHelpers** (`src/api/ApiHelpers.h`): `addStdHeaders`, `xmlErr`, `parseBucketKey`, `rfc7231Date`, `isValidBucketName`

**Bugs fixed:**
- **413 on multipart uploads**: Drogon's default max body is 1 MB; parts are up to 15 MB. Fixed with `.setClientMaxBodySize(256 MB)` and `.setClientMaxMemoryBodySize(256 MB)`.
- **`s3cmd info` returning listing XML as policy**: `GET /{bucket}?cors|policy` fell through to `ListObjectsV2`. Fixed with 404 sub-resource stubs.
- **`s3cmd cp` overwriting object with ACL XML**: `PUT /{bucket}/{key}?acl` had no handler and fell through to `PutObject`, overwriting the copied file. Fixed with a `PUT ?acl` stub.
- **`s3cmd rb --recursive` returning 405**: Bulk delete uses `POST /{bucket}?delete`; `POST` was missing from bucket route registrations.

---

### Configuration

All settings are environment variables with sensible defaults — no config files required:

| Variable | Default | Description |
|----------|---------|-------------|
| `NANOBUCKET_PORT` | `9000` | TCP port |
| `NANOBUCKET_DATA_ROOT` | `./data` | Object storage root |
| `NANOBUCKET_DB_PATH` | `<dataRoot>/meta.db` | SQLite database path |
| `NANOBUCKET_ACCESS_KEY` | `nanobucketadmin` | AWS access key |
| `NANOBUCKET_SECRET_KEY` | `nanobucketadmin` | AWS secret key |

Thread count set to `std::thread::hardware_concurrency()`.

---

### Testing

| Suite | Tests |
|-------|-------|
| XML Builder — `test/unit/test_xml_builder.cpp` | 33 |
| SigV4 Verifier — `test/unit/test_sigv4.cpp` | 29 |
| Metadata Store — `test/unit/test_meta_store.cpp` | 33 |
| Storage Engine — `test/unit/test_storage_engine.cpp` | 30 |
| **Unit total** | **125** |
| s3cmd end-to-end — `scripts/api-test.sh` | 47 |

`api-test.sh` covers: bucket CRUD, put/get/head, nested keys, CopyObject (same + cross-bucket), DeleteObject (idempotent), DeleteObjects (bulk), multipart upload (20 MB, SHA-256 round-trip), multipart abort.

---

### Known Limitations

| Area | Limitation |
|------|-----------|
| TLS | HTTP only. Run behind nginx or Caddy for HTTPS. |
| Auth | Single static credential pair. No IAM, no multi-user. |
| Versioning | Not implemented. Last write wins. |
| ACL / Policy | Stubs only. All authenticated requests treated as owner. |
| Concurrency | SQLite single-mutex connection. Not horizontally scalable. |
| aws CLI | Untested. `s3cmd` is the verified client. |
