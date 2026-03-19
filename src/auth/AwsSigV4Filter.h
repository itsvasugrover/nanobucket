#pragma once

#include "nanobucket/auth/ISigV4Verifier.h"

#include <drogon/HttpFilter.h>

#include <memory>

namespace nanobucket {

// Drogon HttpFilter that gates every request on AWS SigV4 verification.
//
// Because Drogon constructs filters with their default constructor, the
// verifier is injected via a static setter before app().run():
//
//   AwsSigV4Filter::setVerifier(
//       std::make_shared<SigV4Verifier>(accessKey, secretKey));
//
// On failure the filter short-circuits the request and returns a 403
// response whose body is a well-formed S3 XML Error document.
class AwsSigV4Filter : public drogon::HttpFilter<AwsSigV4Filter> {
public:
    // Called by Drogon for each intercepted request.
    void doFilter(const drogon::HttpRequestPtr&  req,
                  drogon::FilterCallback&&        fcb,
                  drogon::FilterChainCallback&&   fccb) override;

    // Inject the verifier once at startup before app().run().
    static void setVerifier(std::shared_ptr<ISigV4Verifier> v);

private:
    static std::shared_ptr<ISigV4Verifier> verifier_;
};

} // namespace nanobucket
