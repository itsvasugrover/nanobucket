#pragma once

#include "nanobucket/common/Types.h"
#include <string>
#include <string_view>
#include <vector>

namespace nanobucket {

// Pure virtual interface for byte storage.
//
// Responsibilities:
//   - Bucket directory management (create / delete on disk)
//   - Object read / write / delete
//   - Temporary part storage for multipart uploads
//   - Provide the on-disk path for a completed object so the HTTP layer can
//     serve it with zero-copy sendfile via Drogon's newFileResponse()
//
// All implementations must be thread-safe — Drogon dispatches requests
// concurrently across its thread pool.
class IStorageEngine {
public:
    virtual ~IStorageEngine() = default;

    // ── Bucket ────────────────────────────────────────────────────────────────

    // Create the on-disk root for a bucket.
    // Returns BucketAlreadyExists if the directory is already present.
    virtual S3Error createBucket(const std::string& bucket) = 0;

    // Remove the on-disk root for a bucket.
    // Returns BucketNotEmpty if any objects remain.
    // Returns BucketNotFound if the bucket does not exist.
    virtual S3Error deleteBucket(const std::string& bucket) = 0;

    // ── Object ────────────────────────────────────────────────────────────────

    // Write data to <bucket>/<key>. Computes and returns the MD5 ETag in outEtag.
    // Returns BucketNotFound if the bucket directory does not exist.
    virtual S3Error put(const std::string& bucket,
                        const std::string& key,
                        std::string_view   data,
                        std::string&       outEtag) = 0;

    // Return the absolute filesystem path to the object file, or empty string
    // if the object does not exist. Used by the controller to call
    // HttpResponse::newFileResponse() for zero-copy GET.
    virtual std::string objectPath(const std::string& bucket,
                                   const std::string& key) const = 0;

    // Delete the object file. Returns ObjectNotFound if it does not exist.
    virtual S3Error remove(const std::string& bucket,
                           const std::string& key) = 0;

    // True if the object file exists on disk.
    virtual bool exists(const std::string& bucket,
                        const std::string& key) const = 0;

    // ── Multipart ─────────────────────────────────────────────────────────────

    // Write a single part to a temporary staging area keyed by uploadId.
    // Computes and returns the MD5 ETag of this part in outEtag.
    virtual S3Error putPart(const std::string& uploadId,
                            int                partNumber,
                            std::string_view   data,
                            std::string&       outEtag) = 0;

    // Concatenate parts in partNumbers order into <bucket>/<key> and remove
    // all staging files for this upload. Called on CompleteMultipartUpload.
    virtual S3Error assembleParts(const std::string&      bucket,
                                  const std::string&      key,
                                  const std::string&      uploadId,
                                  const std::vector<int>& partNumbers) = 0;

    // Remove all staging files for an aborted or completed upload.
    virtual S3Error removeParts(const std::string& uploadId) = 0;
};

} // namespace nanobucket
