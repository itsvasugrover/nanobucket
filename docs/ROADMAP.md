# NanoBucket — Roadmap

## Goal

A lightweight, high-performance, S3-compatible object storage server written in C++ using Drogon. Compatible with `s3cmd`, `aws s3` CLI, and any AWS SDK using path-style addressing.

---

## Status Legend

| Symbol | Meaning |
|--------|---------|
| ✅ | Complete |
| 🔄 | Partial / good enough for now |
| ⬜ | Planned / not started |

---

## Feature Areas

| Area | Doc | Status |
|------|-----|--------|
| Build System & Project Scaffolding | [build-system.md](build-system.md) | ✅ |
| Core Interfaces & Type System | [core-interfaces.md](core-interfaces.md) | ✅ |
| XML Response Builder | [xml-response-builder.md](xml-response-builder.md) | ✅ |
| AWS SigV4 Authentication | [sigv4-authentication.md](sigv4-authentication.md) | ✅ |
| Metadata Store | [metadata-store.md](metadata-store.md) | ✅ |
| Local Filesystem Storage Engine | [storage-engine.md](storage-engine.md) | ✅ |
| S3 API Controllers | [api-controllers.md](api-controllers.md) | ✅ |
| Configuration & Server Entry Point | [configuration.md](configuration.md) | 🔄 |
| Testing & CLI Compatibility | [testing.md](testing.md) | 🔄 |

---

## ✅ Build System & Project Scaffolding
- Root `CMakeLists.txt` with C++17, all Conan deps wired
- `src/CMakeLists.txt` with `nanobucket` executable target
- Conan v2 + `cmake_layout` setup verified
- Scripts: `setup.sh`, `build.sh`, `start.sh`, `clean.sh`, `test.sh`, `api-test.sh`
- Graceful SIGINT/SIGTERM shutdown via `std::signal`
- Port configurable via `NANOBUCKET_PORT` env var

---

## ✅ Core Interfaces & Type System
- `common/Types.h` — `BucketInfo`, `ObjectRecord`, `PartInfo`, `MultipartUploadInfo`, `ListObjectsResult`, `S3Error`
- `storage/IStorageEngine.h` — bucket dir mgmt, object r/w/delete, multipart part staging + assembly
- `meta/IMetaStore.h` — bucket/object/multipart CRUD, `ListObjectsV2` query contract
- `auth/ISigV4Verifier.h` — `verify()` → `VerifyResult`, documents both header-auth and presigned-URL flows

---

## ✅ XML Response Builder
- `pugixml`-based builder for all S3 XML response schemas
- `ListBuckets`, `ListObjectsV2`, `InitiateMultipartUploadResult`, `CompleteMultipartUploadResult`, `ListPartsResult`, `ListMultipartUploadsResult`, `CopyObjectResult`, `DeleteResult`, `Error`

---

## ✅ AWS SigV4 Authentication
- OpenSSL HMAC-SHA256 implementation
- `AwsSigV4Filter` Drogon `HttpFilter` wired to **all routes** in both controllers
- Header-based (`Authorization:`) and presigned URL (`X-Amz-*` query params) auth
- Returns standard S3 XML error on failure (`403 AccessDenied`, `403 SignatureDoesNotMatch`, `400 AuthorizationHeaderMalformed`)
- 29 unit tests covering crypto primitives, AWS test vectors, and full `verify()` round-trips
- **Two Drogon-specific bugs fixed:**
  1. **HEAD→GET method conversion**: Drogon converts `HEAD` to `GET` before calling filters; fixed by using `req->isHead()` to restore the original method in the canonical request
  2. **Body pollution via `getParameters()`**: Drogon's `getParameters()` merges form-body bytes into query params for large binary PUT requests, corrupting the canonical query string; fixed by parsing `req->getQuery()` (raw URL query string) directly

---

## ✅ Metadata Store
- `SqliteMetaStore` implementing `IMetaStore`
- WAL mode SQLite, `PRAGMA foreign_keys`, cascade delete for parts
- Schema: `buckets`, `objects`, `multipart_uploads`, `parts` with 14 cached prepared statements
- Mutex-protected single connection
- Continuation token = `base64(first_excluded_key)`; page 2 uses `key >= token`
- 33 unit tests covering all CRUD paths, pagination, delimiter folding, cascade delete

---

## ✅ Local Filesystem Storage Engine
- `LocalFsStorageEngine` implementing `IStorageEngine`
- Layout: `<dataRoot>/<bucket>/<key>`; parts in `<dataRoot>/.multipart/<uploadId>/<partNumber>`
- Atomic writes via `.tmp` + `rename(2)`; last-write-wins concurrency
- MD5 ETag via OpenSSL EVP; multipart ETag = `md5(concat(part_md5_bytes))-N`
- Path traversal protection: rejects `..` segments and absolute paths
- 30 unit tests covering all CRUD paths, nested keys, atomic overwrite, part assembly, cascade cleanup

---

## ✅ S3 API Controllers
All 16 core S3 API operations implemented and s3cmd-verified. Auth filter applied to every route.

**BucketController** (`src/api/BucketController.h/.cpp`):
- `GET /` → `ListBuckets`
- `PUT /{bucket}` → `CreateBucket` (validates name; storage-first, rollback on meta failure)
- `DELETE /{bucket}` → `DeleteBucket` (404 if missing, 409 if non-empty)
- `HEAD /{bucket}` → `HeadBucket`
- `GET /{bucket}?list-type=2` → `ListObjectsV2` (prefix, delimiter, max-keys, continuation-token)
- `GET /{bucket}?uploads` → `ListMultipartUploads`
- `POST /{bucket}?delete` → `DeleteObjects` (bulk delete; parses `<Delete>` XML, returns `<DeleteResult>`)
- `GET /{bucket}?acl` → stub ACL (FULL_CONTROL for owner)
- `PUT /{bucket}?acl` → stub (200 OK, ACL not enforced)
- `GET /{bucket}?cors|policy|lifecycle|...` → 404 stub (not implemented)

**ObjectController** (`src/api/ObjectController.h/.cpp`):
- `PUT /{bucket}/{key}` → `PutObject`
- `PUT /{bucket}/{key}` + `x-amz-copy-source` → `CopyObject` (preserves source ETag for multipart sources)
- `PUT /{bucket}/{key}?partNumber=N&uploadId=X` → `UploadPart`
- `PUT /{bucket}/{key}?acl` → stub (200 OK, ACL not enforced)
- `GET /{bucket}/{key}` → `GetObject` (zero-copy via `newFileResponse`)
- `GET /{bucket}/{key}?uploadId=X` → `ListParts`
- `GET /{bucket}/{key}?acl` → stub ACL (FULL_CONTROL for owner)
- `HEAD /{bucket}/{key}` → `HeadObject`
- `DELETE /{bucket}/{key}` → `DeleteObject` (always 204, idempotent)
- `DELETE /{bucket}/{key}?uploadId=X` → `AbortMultipartUpload`
- `POST /{bucket}/{key}?uploads` → `CreateMultipartUpload`
- `POST /{bucket}/{key}?uploadId=X` → `CompleteMultipartUpload` (validates parts, assembles, computes multipart ETag)

**ApiHelpers** (`src/api/ApiHelpers.h`):
- `addStdHeaders` — injects `x-amz-request-id`, `x-amz-id-2`, `Server`
- `xmlErr` — builds standard S3 error response
- `parseBucketKey` — extracts bucket and key from raw path (immune to Drogon capture-group quirks)
- `rfc7231Date` — formats timestamps for HTTP headers
- `isValidBucketName` — 3–63 chars, lowercase alphanumeric + hyphens, no consecutive hyphens

---

## 🔄 Configuration & Server Entry Point

**Done:**
- All settings driven by environment variables with sensible defaults:
  - `NANOBUCKET_PORT` (default `9000`)
  - `NANOBUCKET_DATA_ROOT` (default `./data`)
  - `NANOBUCKET_DB_PATH` (default `<dataRoot>/meta.db`)
  - `NANOBUCKET_ACCESS_KEY` (default `nanobucketadmin`)
  - `NANOBUCKET_SECRET_KEY` (default `nanobucketadmin`)
- Thread count set to `hardware_concurrency()`
- Drogon body size configured for large multipart parts (256 MB)

**Not done (optional improvements):**
- JSON config file — env vars are sufficient for most use cases
- Log-level configuration — currently hardcoded to `kInfo`
- Multiple credential pairs / IAM simulation

---

## 🔄 Testing & CLI Compatibility

**Done:**
- **125 unit tests** across four GoogleTest suites:
  - `test_xml_builder` — 33 tests: all XML response schemas
  - `test_sigv4` — 29 tests: crypto primitives, AWS test vectors, full round-trips
  - `test_meta_store` — 33 tests: all CRUD paths, pagination, delimiter folding, cascade delete
  - `test_storage_engine` — 30 tests: all CRUD paths, nested keys, atomic overwrite, multipart assembly
- **`scripts/api-test.sh`** — 47 end-to-end s3cmd integration tests covering all 16 S3 operations

**Not done:**
- `aws s3` CLI integration tests — `s3cmd` passes; aws CLI untested
- `s3cmd sync` test (directory sync with delta detection)
- Presigned URL end-to-end test

---

## S3 API Coverage

| Operation | HTTP | Verified |
|-----------|------|---------|
| ListBuckets | `GET /` | ✅ s3cmd |
| CreateBucket | `PUT /{bucket}` | ✅ s3cmd |
| DeleteBucket | `DELETE /{bucket}` | ✅ s3cmd |
| HeadBucket | `HEAD /{bucket}` | ✅ s3cmd |
| ListObjectsV2 | `GET /{bucket}?list-type=2` | ✅ s3cmd |
| DeleteObjects | `POST /{bucket}?delete` | ✅ s3cmd |
| PutObject | `PUT /{bucket}/{key}` | ✅ s3cmd |
| GetObject | `GET /{bucket}/{key}` | ✅ s3cmd |
| DeleteObject | `DELETE /{bucket}/{key}` | ✅ s3cmd |
| HeadObject | `HEAD /{bucket}/{key}` | ✅ s3cmd |
| CopyObject | `PUT /{bucket}/{key}` + `x-amz-copy-source` | ✅ s3cmd |
| CreateMultipartUpload | `POST /{bucket}/{key}?uploads` | ✅ s3cmd |
| UploadPart | `PUT /{bucket}/{key}?partNumber=N&uploadId=X` | ✅ s3cmd (20 MB) |
| CompleteMultipartUpload | `POST /{bucket}/{key}?uploadId=X` | ✅ s3cmd |
| AbortMultipartUpload | `DELETE /{bucket}/{key}?uploadId=X` | ✅ s3cmd |
| ListParts | `GET /{bucket}/{key}?uploadId=X` | ✅ (impl; not in api-test) |
| ListMultipartUploads | `GET /{bucket}?uploads` | ✅ (impl; not in api-test) |
| GetObjectAcl | `GET /{bucket}/{key}?acl` | stub (200, always FULL_CONTROL) |
| PutObjectAcl | `PUT /{bucket}/{key}?acl` | stub (200, not enforced) |
| GetBucketAcl | `GET /{bucket}?acl` | stub (200, always FULL_CONTROL) |
| PutBucketAcl | `PUT /{bucket}?acl` | stub (200, not enforced) |
| GetBucketCors | `GET /{bucket}?cors` | stub (404 NoSuchCORSConfiguration) |
| GetBucketPolicy | `GET /{bucket}?policy` | stub (404 NoSuchBucketPolicy) |
| GetBucketLocation | `GET /{bucket}?location` | falls through to ListObjectsV2 (s3cmd accepts it) |

---

## Known Limitations

| Area | Limitation |
|------|-----------|
| **TLS** | HTTP only. Run behind nginx/Caddy for HTTPS. |
| **Auth** | Single static credential pair. No IAM, no multi-user. |
| **Concurrency** | SQLite single-mutex connection. No horizontal scaling. |
| **Memory** | Objects fully buffered in memory on write (up to 256 MB per request). |
| **Versioning** | Not implemented. Last write wins. |
| **ACL / Policy** | Stubs only. All authenticated requests treated as owner. |
| **Presigned URLs** | Verification logic implemented; not end-to-end tested. |
| **aws CLI** | Untested. s3cmd is the verified client. |

---

## Future Work

| Feature | Notes |
|---------|-------|
| `aws s3` CLI compatibility | Test + fix gaps |
| TLS via Drogon config | Or document nginx reverse-proxy setup |
| JSON config file | Replace env-var-only approach |
| Multiple credentials | Simple credentials map in config |
| Streaming large objects | Avoid full buffering for write path |
| `s3cmd sync` verification | Delta detection, recursive upload/download |
| Presigned URL e2e test | Verify expiry and download via curl |
| Metrics / health endpoint | `GET /health` → 200 JSON |
| Docker image | Single-container deployment |
