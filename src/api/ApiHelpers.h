#pragma once

// Internal helpers shared across all S3 API controllers.
// Not part of the public include/ API.

#include "xml/S3XmlBuilder.h"

#include <drogon/HttpResponse.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <string>
#include <utility>

namespace nanobucket::api {

// ── Standard headers ──────────────────────────────────────────────────────────

// Stamp every outgoing response with mandatory S3 headers.
inline void addStdHeaders(const drogon::HttpResponsePtr& res)
{
    static std::atomic<uint64_t> counter{0};
    const auto id = std::to_string(++counter);
    res->addHeader("x-amz-request-id", id);
    res->addHeader("x-amz-id-2",       id);
    res->addHeader("Server",           "nanobucket");
}

// ── Error factory ─────────────────────────────────────────────────────────────

// Build a complete S3 XML error response.
inline drogon::HttpResponsePtr xmlErr(
    drogon::HttpStatusCode status,
    const std::string&     code,
    const std::string&     message,
    const std::string&     key    = "",
    const std::string&     bucket = "")
{
    auto res = drogon::HttpResponse::newHttpResponse();
    res->setStatusCode(status);
    res->setContentTypeCode(drogon::CT_APPLICATION_XML);
    res->setBody(S3XmlBuilder::error(code, message, key, bucket));
    addStdHeaders(res);
    return res;
}

// ── Path parsing ──────────────────────────────────────────────────────────────

// Parse "/bucket/key/with/slashes" → {"bucket", "key/with/slashes"}.
// The key may be empty if the path has no component after the bucket.
inline std::pair<std::string, std::string>
parseBucketKey(const std::string& path)
{
    if (path.size() < 2) return {"", ""};
    const auto sep = path.find('/', 1); // first '/' after the leading one
    if (sep == std::string::npos)
        return {path.substr(1), ""};
    return {path.substr(1, sep - 1), path.substr(sep + 1)};
}

// ── Date formatting ───────────────────────────────────────────────────────────

// RFC 7231 format used for Last-Modified headers: "Mon, 15 Jan 2024 10:30:00 GMT"
inline std::string rfc7231Date(std::chrono::system_clock::time_point tp)
{
    const auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm gmt{};
    gmtime_r(&t, &gmt);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
    return buf;
}

// ── Bucket name validation ────────────────────────────────────────────────────

// S3 bucket name rules (subset sufficient for s3cmd/aws s3 compatibility):
//   3–63 chars, lowercase alphanumeric and hyphens, no leading/trailing hyphen,
//   no consecutive dots.
inline bool isValidBucketName(const std::string& name)
{
    if (name.size() < 3 || name.size() > 63) return false;
    for (char c : name) {
        if (!std::islower(static_cast<unsigned char>(c)) &&
            !std::isdigit(static_cast<unsigned char>(c)) &&
            c != '-' && c != '.')
            return false;
    }
    if (name.front() == '-' || name.back() == '-') return false;
    if (name.front() == '.' || name.back() == '.') return false;
    if (name.find("..") != std::string::npos)       return false;
    return true;
}

} // namespace nanobucket::api
