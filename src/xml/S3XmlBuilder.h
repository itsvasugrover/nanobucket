#pragma once

#include "nanobucket/common/Types.h"
#include <chrono>
#include <string>
#include <vector>

namespace nanobucket {

// Builds all S3-compatible XML response bodies using pugixml.
//
// Every method returns a complete UTF-8 XML document as a std::string.
// Callers set this directly as the Drogon response body with
// content-type application/xml.
//
// ETag convention:
//   - Callers pass the bare MD5 hex digest (no quotes).
//   - Methods that embed ETags in XML bodies wrap them in double-quotes,
//     producing the form S3 clients expect: "d41d8cd98f00b204e9800998ecf8427e"
//   - For the ETag HTTP *header*, callers add the quotes themselves.
//
// Namespace:
//   - All list/result responses carry xmlns="http://s3.amazonaws.com/doc/2006-03-01/"
//   - Error responses carry no namespace (matches real S3 behaviour)
class S3XmlBuilder {
public:
    // ── Bucket ────────────────────────────────────────────────────────────────

    // GET / → ListBuckets
    static std::string listBuckets(
        const std::string&             ownerId,
        const std::string&             ownerName,
        const std::vector<BucketInfo>& buckets);

    // ── Object listing ────────────────────────────────────────────────────────

    // GET /{bucket}?list-type=2 → ListObjectsV2
    static std::string listObjectsV2(
        const std::string&     bucket,
        const std::string&     prefix,
        const std::string&     delimiter,
        int                    maxKeys,
        const ListObjectsResult& result);

    // ── Multipart ─────────────────────────────────────────────────────────────

    // POST /{bucket}/{key}?uploads → CreateMultipartUpload response
    static std::string initiateMultipartUpload(
        const std::string& bucket,
        const std::string& key,
        const std::string& uploadId);

    // POST /{bucket}/{key}?uploadId=X → CompleteMultipartUpload response
    // etag should be the bare digest in "<md5>-<partCount>" format
    static std::string completeMultipartUpload(
        const std::string& location,
        const std::string& bucket,
        const std::string& key,
        const std::string& etag);

    // GET /{bucket}/{key}?uploadId=X → ListParts response
    static std::string listParts(
        const std::string&           bucket,
        const std::string&           key,
        const std::string&           uploadId,
        const std::vector<PartInfo>& parts,
        bool                         isTruncated = false);

    // GET /{bucket}?uploads → ListMultipartUploads response
    static std::string listMultipartUploads(
        const std::string&                     bucket,
        const std::vector<MultipartUploadInfo>& uploads);

    // ── Object operations ─────────────────────────────────────────────────────

    // PUT /{bucket}/{key} with x-amz-copy-source → CopyObject response
    static std::string copyObjectResult(
        const std::string&                           etag,
        const std::chrono::system_clock::time_point& lastModified);

    // POST /{bucket}?delete → DeleteObjects response
    // deleted = keys successfully deleted; errors = keys that failed
    static std::string deleteObjects(
        const std::vector<std::string>& deleted,
        const std::vector<std::string>& errors);

    // ── Errors ────────────────────────────────────────────────────────────────

    // Any 4xx / 5xx response body.
    // key and bucketName are included only when non-empty.
    static std::string error(
        const std::string& code,
        const std::string& message,
        const std::string& key        = "",
        const std::string& bucketName = "");

    // ── Timestamp helper (public for use in controllers / tests) ──────────────

    // Formats a time_point as "2024-01-15T10:30:00.000Z"
    static std::string formatTimestamp(
        const std::chrono::system_clock::time_point& tp);

private:
    static constexpr const char* kNamespace =
        "http://s3.amazonaws.com/doc/2006-03-01/";

    // Wraps a bare digest in double-quotes: d41d... → "d41d..."
    static std::string quotedEtag(const std::string& digest);
};

} // namespace nanobucket
