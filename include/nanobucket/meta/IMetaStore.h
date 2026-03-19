#pragma once

#include "nanobucket/common/Types.h"
#include <optional>
#include <string>
#include <vector>

namespace nanobucket {

// Pure virtual interface for metadata storage.
//
// Responsibilities:
//   - Bucket records (existence, region, creation time)
//   - Object records (key, size, etag, content-type, last-modified)
//   - Multipart upload state (upload tracking, per-part etag / size records)
//
// The implementation (SqliteMetaStore) uses Drogon's async SQLite client so
// all methods here are synchronous value-returning — the async wrapping is
// done at the controller level, not inside the store.
class IMetaStore {
public:
    virtual ~IMetaStore() = default;

    // ── Bucket ────────────────────────────────────────────────────────────────

    // Insert a new bucket record.
    // Returns BucketAlreadyExists if a record with this name is present.
    virtual S3Error createBucket(const std::string& name,
                                 const std::string& region) = 0;

    // Delete a bucket record.
    // Returns BucketNotFound  if the bucket does not exist.
    // Returns BucketNotEmpty  if any object records reference this bucket.
    virtual S3Error deleteBucket(const std::string& name) = 0;

    // Returns the bucket record, or nullopt if it does not exist.
    virtual std::optional<BucketInfo> getBucket(const std::string& name) = 0;

    // Returns all bucket records ordered by creation time ascending.
    virtual std::vector<BucketInfo> listBuckets() = 0;

    // ── Object ────────────────────────────────────────────────────────────────

    // Upsert an object record (insert or replace on <bucket, key>).
    virtual S3Error putObject(const ObjectRecord& record) = 0;

    // Delete an object record.
    // Returns ObjectNotFound if no matching record exists.
    virtual S3Error deleteObject(const std::string& bucket,
                                 const std::string& key) = 0;

    // Returns the object record, or nullopt if it does not exist.
    virtual std::optional<ObjectRecord> getObject(const std::string& bucket,
                                                  const std::string& key) = 0;

    // ListObjectsV2 query.
    //   prefix            – only return keys with this prefix (empty = all)
    //   delimiter         – group keys sharing a sub-path (typically "/")
    //   maxKeys           – page size (1–1000)
    //   continuationToken – opaque token from a previous truncated response
    virtual ListObjectsResult listObjects(const std::string& bucket,
                                          const std::string& prefix,
                                          const std::string& delimiter,
                                          int                maxKeys,
                                          const std::string& continuationToken) = 0;

    // ── Multipart ─────────────────────────────────────────────────────────────

    // Create a new multipart upload record and return the generated uploadId.
    virtual std::string createMultipartUpload(const std::string& bucket,
                                              const std::string& key,
                                              const std::string& contentType) = 0;

    // Retrieve a multipart upload record (to validate bucket/key ownership).
    // Returns nullopt if the uploadId does not exist.
    virtual std::optional<MultipartUploadInfo>
    getMultipartUpload(const std::string& uploadId) = 0;

    // Upsert a part record under an upload. Called after each UploadPart.
    virtual S3Error putPart(const std::string& uploadId,
                            const PartInfo&    part) = 0;

    // Returns all part records for an upload ordered by partNumber ascending.
    virtual std::vector<PartInfo> listParts(const std::string& uploadId) = 0;

    // Remove the upload record and all its part records (abort or complete).
    virtual S3Error deleteMultipartUpload(const std::string& uploadId) = 0;

    // Returns all in-progress uploads for a bucket.
    virtual std::vector<MultipartUploadInfo>
    listMultipartUploads(const std::string& bucket) = 0;
};

} // namespace nanobucket
