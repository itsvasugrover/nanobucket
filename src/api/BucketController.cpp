#include "BucketController.h"
#include "ApiHelpers.h"
#include "xml/S3XmlBuilder.h"

#include <pugixml.hpp>
#include <spdlog/spdlog.h>

#include <unordered_map>

namespace nanobucket {

using namespace api;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;
using drogon::HttpRequestPtr;

// Static member definitions
std::shared_ptr<IMetaStore>    BucketController::meta_;
std::shared_ptr<IStorageEngine> BucketController::storage_;

void BucketController::setDeps(std::shared_ptr<IMetaStore>    meta,
                                std::shared_ptr<IStorageEngine> storage)
{
    meta_    = std::move(meta);
    storage_ = std::move(storage);
}

// ── GET / — ListBuckets ───────────────────────────────────────────────────────

void BucketController::listBuckets(const HttpRequestPtr& /*req*/,
                                    std::function<void(const HttpResponsePtr&)>&& cb)
{
    const auto buckets = meta_->listBuckets();
    auto res = HttpResponse::newHttpResponse();
    res->setStatusCode(drogon::k200OK);
    res->setContentTypeCode(drogon::CT_APPLICATION_XML);
    res->setBody(S3XmlBuilder::listBuckets("nanobucket", "nanobucket", buckets));
    addStdHeaders(res);
    cb(res);
}

// ── /{bucket} dispatcher ──────────────────────────────────────────────────────

void BucketController::handleBucket(const HttpRequestPtr& req,
                                     std::function<void(const HttpResponsePtr&)>&& cb,
                                     const std::string& bucket)
{
    const auto method = req->method();

    // ── PUT ?acl → PutBucketAcl (stub) ───────────────────────────────────────
    if (method == drogon::Put && req->getParameters().count("acl")) {
        auto res = HttpResponse::newHttpResponse();
        res->setStatusCode(drogon::k200OK);
        addStdHeaders(res);
        cb(res);
        return;
    }

    // ── PUT → CreateBucket ────────────────────────────────────────────────────
    if (method == drogon::Put) {
        if (!isValidBucketName(bucket)) {
            cb(xmlErr(drogon::k400BadRequest, "InvalidBucketName",
                      "The specified bucket is not valid.", "", bucket));
            return;
        }

        // Create on storage first; roll back if meta fails
        auto err = storage_->createBucket(bucket);
        if (err == S3Error::BucketAlreadyExists) {
            cb(xmlErr(drogon::k409Conflict, "BucketAlreadyOwnedByYou",
                      "Your previous request to create the named bucket succeeded "
                      "and you already own it.", "", bucket));
            return;
        }
        if (err != S3Error::None) {
            cb(xmlErr(drogon::k500InternalServerError, "InternalError",
                      "We encountered an internal error.", "", bucket));
            return;
        }

        const std::string region = "us-east-1"; // TODO: parse from CreateBucketConfiguration XML body
        err = meta_->createBucket(bucket, region);
        if (err != S3Error::None) {
            storage_->deleteBucket(bucket); // rollback
            cb(xmlErr(drogon::k500InternalServerError, "InternalError",
                      "We encountered an internal error.", "", bucket));
            return;
        }

        auto res = HttpResponse::newHttpResponse();
        res->setStatusCode(drogon::k200OK);
        addStdHeaders(res);
        cb(res);
        return;
    }

    // ── DELETE → DeleteBucket ─────────────────────────────────────────────────
    if (method == drogon::Delete) {
        auto err = meta_->deleteBucket(bucket);
        if (err == S3Error::BucketNotFound) {
            cb(xmlErr(drogon::k404NotFound, "NoSuchBucket",
                      "The specified bucket does not exist.", "", bucket));
            return;
        }
        if (err == S3Error::BucketNotEmpty) {
            cb(xmlErr(drogon::k409Conflict, "BucketNotEmpty",
                      "The bucket you tried to delete is not empty.", "", bucket));
            return;
        }
        if (err != S3Error::None) {
            cb(xmlErr(drogon::k500InternalServerError, "InternalError",
                      "We encountered an internal error.", "", bucket));
            return;
        }

        storage_->deleteBucket(bucket); // best-effort; meta already authoritative

        auto res = HttpResponse::newHttpResponse();
        res->setStatusCode(drogon::k204NoContent);
        addStdHeaders(res);
        cb(res);
        return;
    }

    // ── HEAD → HeadBucket ─────────────────────────────────────────────────────
    if (method == drogon::Head) {
        if (!meta_->getBucket(bucket)) {
            cb(xmlErr(drogon::k404NotFound, "NoSuchBucket",
                      "The specified bucket does not exist.", "", bucket));
            return;
        }
        auto res = HttpResponse::newHttpResponse();
        res->setStatusCode(drogon::k200OK);
        addStdHeaders(res);
        cb(res);
        return;
    }

    // ── GET → ListMultipartUploads or ListObjectsV2 ───────────────────────────
    if (method == drogon::Get) {
        const auto& params = req->getParameters();

        if (!meta_->getBucket(bucket)) {
            cb(xmlErr(drogon::k404NotFound, "NoSuchBucket",
                      "The specified bucket does not exist.", "", bucket));
            return;
        }

        // ?acl → return stub ACL (FULL_CONTROL for owner)
        if (params.count("acl")) {
            static const char* kBucketAclXml =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<AccessControlPolicy xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
                  "<Owner><ID>owner</ID><DisplayName>owner</DisplayName></Owner>"
                  "<AccessControlList>"
                    "<Grant>"
                      "<Grantee xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""
                               " xsi:type=\"CanonicalUser\">"
                        "<ID>owner</ID><DisplayName>owner</DisplayName>"
                      "</Grantee>"
                      "<Permission>FULL_CONTROL</Permission>"
                    "</Grant>"
                  "</AccessControlList>"
                "</AccessControlPolicy>\n";
            auto res = HttpResponse::newHttpResponse();
            res->setStatusCode(drogon::k200OK);
            res->setContentTypeCode(drogon::CT_APPLICATION_XML);
            res->setBody(kBucketAclXml);
            addStdHeaders(res);
            cb(res);
            return;
        }

        // Sub-resource stubs — features not implemented, return 404
        static const std::unordered_map<std::string, std::string> kSubResErrs{
            {"cors",         "NoSuchCORSConfiguration"},
            {"policy",       "NoSuchBucketPolicy"},
            {"lifecycle",    "NoSuchLifecycleConfiguration"},
            {"logging",      "NoSuchLoggingStatus"},
            {"notification", "NoSuchNotificationConfiguration"},
            {"versioning",   "NoSuchVersioningConfiguration"},
            {"website",      "NoSuchWebsiteConfiguration"},
            {"tagging",      "NoSuchTagSet"},
        };
        for (const auto& [param, errCode] : kSubResErrs) {
            if (params.count(param)) {
                cb(xmlErr(drogon::k404NotFound, errCode,
                          "This bucket configuration does not exist.", "", bucket));
                return;
            }
        }

        // ?uploads → ListMultipartUploads
        if (params.count("uploads")) {
            const auto uploads = meta_->listMultipartUploads(bucket);
            auto res = HttpResponse::newHttpResponse();
            res->setStatusCode(drogon::k200OK);
            res->setContentTypeCode(drogon::CT_APPLICATION_XML);
            res->setBody(S3XmlBuilder::listMultipartUploads(bucket, uploads));
            addStdHeaders(res);
            cb(res);
            return;
        }

        // Default: ListObjectsV2
        const std::string prefix    = req->getParameter("prefix");
        const std::string delimiter = req->getParameter("delimiter");
        const std::string token     = req->getParameter("continuation-token");
        int maxKeys = 1000;
        const auto& mk = req->getParameter("max-keys");
        if (!mk.empty()) {
            try { maxKeys = std::stoi(mk); } catch (...) {}
            maxKeys = std::clamp(maxKeys, 1, 1000);
        }

        const auto result = meta_->listObjects(bucket, prefix, delimiter, maxKeys, token);

        auto res = HttpResponse::newHttpResponse();
        res->setStatusCode(drogon::k200OK);
        res->setContentTypeCode(drogon::CT_APPLICATION_XML);
        res->setBody(S3XmlBuilder::listObjectsV2(bucket, prefix, delimiter, maxKeys, result));
        addStdHeaders(res);
        cb(res);
        return;
    }

    // ── POST ?delete → DeleteObjects ─────────────────────────────────────────
    if (method == drogon::Post) {
        const auto& params = req->getParameters();
        if (!params.count("delete")) {
            cb(xmlErr(drogon::k400BadRequest, "InvalidRequest",
                      "Unrecognised POST operation on bucket.", "", bucket));
            return;
        }

        // Parse <Delete><Object><Key>...</Key></Object>...</Delete>
        pugi::xml_document doc;
        const auto body = req->getBody();
        if (!doc.load_buffer(body.data(), body.size())) {
            cb(xmlErr(drogon::k400BadRequest, "MalformedXML",
                      "The XML you provided was not well-formed.", "", bucket));
            return;
        }

        std::vector<std::string> deleted, errors;
        for (auto obj : doc.child("Delete").children("Object")) {
            std::string key = obj.child("Key").text().as_string();
            if (key.empty()) continue;
            storage_->remove(bucket, key);
            meta_->deleteObject(bucket, key);
            deleted.push_back(key);
        }

        auto res = HttpResponse::newHttpResponse();
        res->setStatusCode(drogon::k200OK);
        res->setContentTypeCode(drogon::CT_APPLICATION_XML);
        res->setBody(S3XmlBuilder::deleteObjects(deleted, errors));
        addStdHeaders(res);
        cb(res);
        return;
    }

    // Unsupported method
    cb(xmlErr(drogon::k405MethodNotAllowed, "MethodNotAllowed",
              "The specified method is not allowed against this resource.", "", bucket));
}

} // namespace nanobucket
