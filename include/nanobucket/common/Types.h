#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace nanobucket {

// ── Error codes returned by storage and metadata operations ──────────────────
enum class S3Error {
    None,
    BucketAlreadyExists,   // 409 BucketAlreadyOwnedByYou / BucketAlreadyExists
    BucketNotEmpty,        // 409 BucketNotEmpty
    BucketNotFound,        // 404 NoSuchBucket
    ObjectNotFound,        // 404 NoSuchKey
    UploadNotFound,        // 404 NoSuchUpload
    InvalidPart,           // 400 InvalidPart (bad part number or etag mismatch)
    InvalidArgument,       // 400 InvalidArgument
    InternalError,         // 500 InternalError
};

// ── Shared data structures ────────────────────────────────────────────────────

struct BucketInfo {
    std::string name;
    std::string region;
    std::chrono::system_clock::time_point createdAt;
};

struct ObjectRecord {
    std::string bucket;
    std::string key;
    uint64_t    size{0};
    std::string etag;
    std::string contentType;
    std::chrono::system_clock::time_point lastModified;
};

struct PartInfo {
    int         partNumber{0};
    std::string etag;
    uint64_t    size{0};
    std::chrono::system_clock::time_point lastModified;
};

struct MultipartUploadInfo {
    std::string uploadId;
    std::string bucket;
    std::string key;
    std::string contentType;
    std::chrono::system_clock::time_point initiatedAt;
};

// Result of ListObjectsV2 — used by both meta store and bucket controller.
struct ListObjectsResult {
    std::vector<ObjectRecord> objects;
    std::vector<std::string>  commonPrefixes;   // populated when delimiter is set
    std::string               nextContinuationToken;
    bool                      isTruncated{false};
};

} // namespace nanobucket
