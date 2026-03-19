#pragma once

#include "nanobucket/auth/ISigV4Verifier.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nanobucket {

// Parsed components extracted from either the Authorization header
// or the presigned URL query parameters.
struct AuthContext {
    std::string accessKeyId;
    std::string date;          // YYYYMMDD extracted from Credential scope
    std::string region;        // e.g. "us-east-1"
    std::string service;       // always "s3" for our server
    std::string signedHeaders; // semicolon-separated lowercase names
    std::string signature;     // hex string provided by client
    std::string timestamp;     // full compact timestamp: 20240115T103000Z
    bool        presigned{false};
};

// Implements ISigV4Verifier using OpenSSL primitives.
//
// The crypto primitives (hmacSha256, sha256Hex, uriEncode) and the
// intermediate building blocks (buildCanonicalRequest, buildStringToSign,
// computeSignature) are public so the unit tests can drive them with the
// official AWS SigV4 test vectors independently.
class SigV4Verifier : public ISigV4Verifier {
public:
    SigV4Verifier(std::string accessKey, std::string secretKey);

    // ISigV4Verifier
    VerifyResult verify(const drogon::HttpRequestPtr& req) const override;

    // ── Building blocks (public for unit testing) ─────────────────────────────

    // Assemble the canonical request string from its components.
    // signedHeaderNames must already be sorted and lowercased.
    // presigned=true → payload hash is always "UNSIGNED-PAYLOAD".
    std::string buildCanonicalRequest(
        const drogon::HttpRequestPtr&   req,
        const std::vector<std::string>& signedHeaderNames,
        bool                            presigned) const;

    // Format the string-to-sign from its parts.
    static std::string buildStringToSign(
        const std::string& timestamp,
        const std::string& credentialScope,
        const std::string& canonicalRequestHash);

    // Derive the signing key and return HexEncode(HMAC(kSigning, stringToSign)).
    static std::string computeSignature(
        const std::string& secretKey,
        const std::string& date,
        const std::string& region,
        const std::string& service,
        const std::string& stringToSign);

    // ── Crypto primitives (public for testing) ────────────────────────────────

    // HMAC-SHA256. Key is raw bytes; returns raw bytes.
    static std::vector<uint8_t> hmacSha256(const std::vector<uint8_t>& key,
                                            std::string_view             data);

    // SHA256 of data returned as lowercase hex string.
    static std::string sha256Hex(std::string_view data);

    // Percent-encode input per RFC 3986 unreserved character set.
    // encodeSlash=true  → '/' is encoded as %2F  (used for key segments, values)
    // encodeSlash=false → '/' is passed through   (not used in SigV4 directly)
    static std::string uriEncode(std::string_view input, bool encodeSlash = true);

    // Lowercase hex encoding of raw bytes.
    static std::string hexEncode(const uint8_t* data, size_t len);

    // Build the canonical query string from a raw URL query string.
    // Exported as public so unit tests can drive it directly without needing a
    // real Drogon HttpRequest object (Drogon has no setQuery() setter).
    // presigned=true excludes "X-Amz-Signature" from the output.
    static std::string canonicalQueryStringFromRaw(const std::string& rawQuery,
                                                    bool presigned);

private:
    std::string accessKey_;
    std::string secretKey_;

    // ── Parsing ───────────────────────────────────────────────────────────────

    static std::optional<AuthContext> parseAuthHeader(
        const drogon::HttpRequestPtr& req);

    static std::optional<AuthContext> parsePresignedParams(
        const drogon::HttpRequestPtr& req);

    // ── Canonical request components ──────────────────────────────────────────

    // URI-encode each path segment; keep '/' separators.
    static std::string canonicalUri(const std::string& path);

    // Sort, encode and join query parameters.
    // presigned=true excludes X-Amz-Signature from the output.
    static std::string canonicalQueryString(
        const drogon::HttpRequestPtr& req, bool presigned);

    // Build the canonical headers block for the given signed header names.
    static std::string canonicalHeaders(
        const drogon::HttpRequestPtr&   req,
        const std::vector<std::string>& headerNames);

    // Return the payload hash: honour x-amz-content-sha256 header if present,
    // otherwise compute SHA256 of the request body.
    // For presigned requests always returns "UNSIGNED-PAYLOAD".
    static std::string payloadHash(const drogon::HttpRequestPtr& req,
                                   bool presigned);

    // ── Helpers ───────────────────────────────────────────────────────────────

    // 4-step HMAC key derivation chain.
    static std::vector<uint8_t> deriveSigningKey(const std::string& secretKey,
                                                  const std::string& date,
                                                  const std::string& region,
                                                  const std::string& service);

    // Constant-time string comparison to prevent timing attacks.
    static bool constantTimeEqual(const std::string& a, const std::string& b);

    // Strip leading/trailing whitespace; collapse internal runs to one space.
    static std::string normalizeHeaderValue(const std::string& v);

    // Parse "20240115T103000Z" → Unix timestamp. Returns -1 on failure.
    static std::time_t parseAmzTimestamp(const std::string& ts);
};

} // namespace nanobucket
