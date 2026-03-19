# Core Interfaces & Type System

## Overview

The core layer defines the contracts that all internal components program against. No implementation details live here — only pure virtual interfaces and shared data types. This decoupling means storage backends, metadata stores, and auth verifiers can be swapped or tested in isolation without touching the HTTP layer.

---

## File Map

```
include/nanobucket/
├── common/
│   └── Types.h             — all shared structs and the S3Error enum
├── storage/
│   └── IStorageEngine.h    — byte storage contract
├── meta/
│   └── IMetaStore.h        — metadata persistence contract
└── auth/
    └── ISigV4Verifier.h    — request authentication contract
```

---

## Type System (`common/Types.h`)

### `S3Error`

Returned by all mutating operations. Controllers map each value to the exact S3 XML `<Code>` string and HTTP status code.

| Value | S3 XML Code | HTTP |
|-------|------------|------|
| `None` | — | 2xx |
| `BucketAlreadyExists` | `BucketAlreadyOwnedByYou` | 409 |
| `BucketNotEmpty` | `BucketNotEmpty` | 409 |
| `BucketNotFound` | `NoSuchBucket` | 404 |
| `ObjectNotFound` | `NoSuchKey` | 404 |
| `UploadNotFound` | `NoSuchUpload` | 404 |
| `InvalidPart` | `InvalidPart` | 400 |
| `InvalidArgument` | `InvalidArgument` | 400 |
| `InternalError` | `InternalError` | 500 |

### `BucketInfo`

```cpp
struct BucketInfo {
    std::string name;
    std::string region;
    std::chrono::system_clock::time_point createdAt;
};
```

Returned by `listBuckets()` and `getBucket()`. The `createdAt` timestamp is formatted as ISO 8601 (`2024-01-15T10:30:00.000Z`) in the `ListBuckets` XML response.

### `ObjectRecord`

```cpp
struct ObjectRecord {
    std::string bucket;
    std::string key;
    uint64_t    size{0};
    std::string etag;         // MD5 hex digest, quoted: "d41d8cd98f00b204e9800998ecf8427e"
    std::string contentType;
    std::chrono::system_clock::time_point lastModified;
};
```

Central record for all object metadata. Written to the meta store on `PutObject` / `CompleteMultipartUpload`, read back on `HeadObject`, `GetObject`, and `ListObjectsV2`.

### `PartInfo`

```cpp
struct PartInfo {
    int         partNumber{0};   // 1-based, range [1, 10000]
    std::string etag;
    uint64_t    size{0};
    std::chrono::system_clock::time_point lastModified;
};
```

Stored per `UploadPart` call. Returned in `ListParts` and validated during `CompleteMultipartUpload` (client sends expected ETags; server checks them against stored values).

### `MultipartUploadInfo`

```cpp
struct MultipartUploadInfo {
    std::string uploadId;
    std::string bucket;
    std::string key;
    std::string contentType;
    std::chrono::system_clock::time_point initiatedAt;
};
```

Created on `CreateMultipartUpload`, deleted on `CompleteMultipartUpload` or `AbortMultipartUpload`.

### `ListObjectsResult`

```cpp
struct ListObjectsResult {
    std::vector<ObjectRecord> objects;
    std::vector<std::string>  commonPrefixes;
    std::string               nextContinuationToken;
    bool                      isTruncated{false};
};
```

`commonPrefixes` is populated when a `delimiter` is provided (typically `/`). Keys that share a sub-path up to the delimiter are collapsed into a single prefix entry — this is how S3 simulates directory listings.

---

## Storage Engine Interface (`IStorageEngine.h`)

### Responsibility

Manages all byte I/O on the filesystem. Has no knowledge of HTTP — it only reads and writes files.

### Bucket Management

```cpp
virtual S3Error createBucket(const std::string& bucket) = 0;
virtual S3Error deleteBucket(const std::string& bucket) = 0;
```

The local filesystem implementation creates and removes directories under the configured data root. `deleteBucket` returns `BucketNotEmpty` if any object files exist inside.

### Object Operations

```cpp
virtual S3Error put(const std::string& bucket,
                    const std::string& key,
                    std::string_view   data,
                    std::string&       outEtag) = 0;

virtual std::string objectPath(const std::string& bucket,
                               const std::string& key) const = 0;

virtual S3Error remove(const std::string& bucket,
                       const std::string& key) = 0;

virtual bool exists(const std::string& bucket,
                    const std::string& key) const = 0;
```

**Zero-copy GET**: `objectPath()` returns the absolute filesystem path. The controller passes this directly to `HttpResponse::newFileResponse()`, which uses the `sendfile(2)` syscall on Linux to transfer file bytes from the kernel page cache to the socket without copying them into user space. This is the primary performance advantage for large file downloads.

**ETag on write**: `put()` computes the MD5 digest of the written bytes and returns it via `outEtag` as a lowercase hex string. This is later stored in the meta store and returned in `Content-ETag` / `ETag` response headers.

**`std::string_view` for data**: Drogon provides the parsed request body as a `std::string`. Using `string_view` avoids copying the body bytes into the storage call.

### Multipart Operations

```cpp
virtual S3Error putPart(const std::string& uploadId, int partNumber,
                        std::string_view data, std::string& outEtag) = 0;

virtual S3Error assembleParts(const std::string& bucket,
                              const std::string& key,
                              const std::string& uploadId,
                              const std::vector<int>& partNumbers) = 0;

virtual S3Error removeParts(const std::string& uploadId) = 0;
```

Parts are staged in a temporary directory keyed by `uploadId`. `assembleParts()` concatenates them in `partNumbers` order into the final object path, then cleans up staging files. `removeParts()` is called independently on abort.

---

## Metadata Store Interface (`IMetaStore.h`)

### Responsibility

Persists and queries all object metadata — bucket records, object records, multipart upload state. Has no knowledge of filesystems or HTTP.

### Bucket Operations

```cpp
virtual S3Error createBucket(const std::string& name, const std::string& region) = 0;
virtual S3Error deleteBucket(const std::string& name) = 0;
virtual std::optional<BucketInfo> getBucket(const std::string& name) = 0;
virtual std::vector<BucketInfo> listBuckets() = 0;
```

`listBuckets()` returns all buckets ordered by creation time — this is the order expected by the `ListBuckets` XML response.

### Object Operations

```cpp
virtual S3Error putObject(const ObjectRecord& record) = 0;
virtual S3Error deleteObject(const std::string& bucket, const std::string& key) = 0;
virtual std::optional<ObjectRecord> getObject(const std::string& bucket,
                                              const std::string& key) = 0;
virtual ListObjectsResult listObjects(const std::string& bucket,
                                      const std::string& prefix,
                                      const std::string& delimiter,
                                      int maxKeys,
                                      const std::string& continuationToken) = 0;
```

`putObject()` is an upsert — if a record for `(bucket, key)` already exists, it is replaced. This matches S3 semantics where `PutObject` on an existing key overwrites the previous version.

`listObjects()` implements the full `ListObjectsV2` query contract:
- `prefix` — only return keys starting with this string
- `delimiter` — fold keys into `commonPrefixes` at this boundary
- `maxKeys` — page size (1–1000, default 1000)
- `continuationToken` — opaque cursor from a previous truncated response

### Multipart Operations

```cpp
virtual std::string createMultipartUpload(const std::string& bucket,
                                          const std::string& key,
                                          const std::string& contentType) = 0;

virtual std::optional<MultipartUploadInfo> getMultipartUpload(
    const std::string& uploadId) = 0;

virtual S3Error putPart(const std::string& uploadId, const PartInfo& part) = 0;

virtual std::vector<PartInfo> listParts(const std::string& uploadId) = 0;

virtual S3Error deleteMultipartUpload(const std::string& uploadId) = 0;

virtual std::vector<MultipartUploadInfo> listMultipartUploads(
    const std::string& bucket) = 0;
```

`createMultipartUpload()` generates and returns a UUID-based `uploadId`. `getMultipartUpload()` is used by the controller to validate that the `bucket` and `key` in the request match those recorded when the upload was initiated — this prevents part injection attacks.

---

## Auth Verifier Interface (`ISigV4Verifier.h`)

### Responsibility

Validates that every incoming request carries a valid AWS Signature Version 4 credential. Returns a typed result rather than throwing so the filter can build a proper S3 XML error response.

### Contract

```cpp
struct VerifyResult {
    bool        valid{false};
    std::string errorCode;     // S3 XML error code
    std::string errorMessage;  // Human-readable detail
};

virtual VerifyResult verify(const drogon::HttpRequestPtr& req) const = 0;
```

### Two Authentication Forms

**Header-based (standard):**
- `Authorization` header contains `AWS4-HMAC-SHA256 Credential=.../SignedHeaders=.../Signature=...`
- `x-amz-date` header carries the request timestamp
- `x-amz-content-sha256` carries the SHA256 hash of the request body

**Presigned URL (query-string):**
- `X-Amz-Credential`, `X-Amz-SignedHeaders`, `X-Amz-Signature` as query parameters
- Payload hash is always `UNSIGNED-PAYLOAD`
- `X-Amz-Expires` (in seconds) must not have elapsed

Both forms run through the same canonical request → string-to-sign → signing key pipeline. See [sigv4-authentication.md](sigv4-authentication.md) for the full algorithm.

---

## Design Principles

### Why pure virtual interfaces?

1. **Testability** — unit tests can inject a mock `IStorageEngine` or `IMetaStore` without touching the filesystem or SQLite.
2. **Extensibility** — a network storage backend or PostgreSQL metadata store can be dropped in without changing any controller code.
3. **Separation of concerns** — the HTTP layer (controllers, filters) never references SQLite or filesystem APIs directly.

### Why synchronous interface methods?

Drogon is asynchronous and the controllers run on its event loop thread pool. The async boundary belongs in the controller layer, not inside the store — keeping interface methods synchronous makes them easy to reason about and test. The `SqliteMetaStore` implementation will wrap its synchronous SQLite calls in `drogon::async_func` to yield the event loop thread during I/O.

### Why `std::optional` for lookups?

Returning `nullopt` on "not found" is explicit and zero-overhead compared to exceptions for the expected case. The controller checks the optional and returns a 404 XML response if empty.
