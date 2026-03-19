#pragma once

#include <drogon/HttpRequest.h>
#include <string>

namespace nanobucket {

// Result of an AWS SigV4 verification attempt.
struct VerifyResult {
    bool        valid{false};
    std::string errorCode;    // S3 XML error code, e.g. "SignatureDoesNotMatch"
    std::string errorMessage; // Human-readable detail for the XML error body
};

// Pure virtual interface for AWS Signature Version 4 request verification.
//
// The AwsSigV4Filter calls verify() for every incoming request before it
// reaches any controller. Presigned URL requests (query-string auth) carry
// X-Amz-Signature as a query parameter instead of an Authorization header —
// implementations must handle both forms.
class ISigV4Verifier {
public:
    virtual ~ISigV4Verifier() = default;

    // Verify the SigV4 signature on a request.
    //
    // Standard (header-based) auth flow:
    //   1. Parse Authorization header → Credential, SignedHeaders, Signature
    //   2. Reconstruct the canonical request from method, URI, headers, body hash
    //   3. Build the string-to-sign (date, region, scope, canonical request hash)
    //   4. Derive the signing key via HMAC-SHA256 key chain
    //   5. Compare computed signature against the provided Signature value
    //
    // Presigned URL (query-string) auth:
    //   - Same steps but credential/signed-headers/signature come from
    //     X-Amz-Credential, X-Amz-SignedHeaders, X-Amz-Signature query params
    //   - Payload hash is always "UNSIGNED-PAYLOAD"
    //   - Must check X-Amz-Expires has not elapsed
    virtual VerifyResult verify(const drogon::HttpRequestPtr& req) const = 0;
};

} // namespace nanobucket
