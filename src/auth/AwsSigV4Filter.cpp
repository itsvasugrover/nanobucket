#include "AwsSigV4Filter.h"
#include "xml/S3XmlBuilder.h"

#include <drogon/HttpResponse.h>
#include <spdlog/spdlog.h>

namespace nanobucket {

// Static member definition
std::shared_ptr<ISigV4Verifier> AwsSigV4Filter::verifier_;

void AwsSigV4Filter::setVerifier(std::shared_ptr<ISigV4Verifier> v)
{
    verifier_ = std::move(v);
}

void AwsSigV4Filter::doFilter(const drogon::HttpRequestPtr&  req,
                               drogon::FilterCallback&&        fcb,
                               drogon::FilterChainCallback&&   fccb)
{
    if (!verifier_) {
        // Verifier not wired up — should never happen in production.
        spdlog::error("AwsSigV4Filter: verifier not initialised");
        auto res = drogon::HttpResponse::newHttpResponse();
        res->setStatusCode(drogon::k500InternalServerError);
        res->setContentTypeCode(drogon::CT_APPLICATION_XML);
        res->setBody(S3XmlBuilder::error("InternalError",
                                          "Auth verifier not configured."));
        fcb(res);
        return;
    }

    const VerifyResult result = verifier_->verify(req);

    if (result.valid) {
        fccb(); // pass to controller
        return;
    }

    spdlog::warn("SigV4 verification failed [{}]: {} - {}",
                 req->getPeerAddr().toIpPort(),
                 result.errorCode,
                 result.errorMessage);

    // AuthorizationHeaderMalformed → 400; everything else → 403
    const auto status = (result.errorCode == "AuthorizationHeaderMalformed")
                            ? drogon::k400BadRequest
                            : drogon::k403Forbidden;

    auto res = drogon::HttpResponse::newHttpResponse();
    res->setStatusCode(status);
    res->setContentTypeCode(drogon::CT_APPLICATION_XML);
    res->setBody(S3XmlBuilder::error(result.errorCode, result.errorMessage));
    fcb(res);
}

} // namespace nanobucket
