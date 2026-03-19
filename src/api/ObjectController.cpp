#include "ObjectController.h"
#include "ApiHelpers.h"
#include "xml/S3XmlBuilder.h"

#include <openssl/evp.h>
#include <pugixml.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace nanobucket {

using namespace api;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;
using drogon::HttpRequestPtr;
using Clock = std::chrono::system_clock;

// Static member definitions
std::shared_ptr<IMetaStore>    ObjectController::meta_;
std::shared_ptr<IStorageEngine> ObjectController::storage_;

void ObjectController::setDeps(std::shared_ptr<IMetaStore>    meta,
                                std::shared_ptr<IStorageEngine> storage)
{
    meta_    = std::move(meta);
    storage_ = std::move(storage);
}

// ── Multipart ETag ────────────────────────────────────────────────────────────

std::string ObjectController::computeMultipartEtag(const std::vector<PartInfo>& parts)
{
    // MD5(concat of raw bytes of each part's MD5 hex digest)
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);

    for (const auto& p : parts) {
        const std::string& hex = p.etag;
        // hex-decode two chars at a time into one byte
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            uint8_t byte = static_cast<uint8_t>(
                std::stoul(hex.substr(i, 2), nullptr, 16));
            EVP_DigestUpdate(ctx, &byte, 1);
        }
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  dlen = 0;
    EVP_DigestFinal_ex(ctx, digest, &dlen);
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    for (unsigned int i = 0; i < dlen; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    oss << '-' << parts.size();
    return oss.str();
}

// ── Top-level dispatchers ─────────────────────────────────────────────────────

void ObjectController::handleGet(const HttpRequestPtr& req,
                                  std::function<void(const HttpResponsePtr&)>&& cb,
                                  const std::string& /*p1*/, const std::string& /*p2*/)
{
    const auto [bucket, key] = parseBucketKey(req->getPath());
    const auto& params = req->getParameters();

    if (params.count("uploadId"))
        doListParts(req, std::move(cb), bucket, key);
    else if (params.count("acl"))
        doGetAcl(req, std::move(cb), bucket, key);
    else
        doGetObject(req, std::move(cb), bucket, key);
}

void ObjectController::handlePut(const HttpRequestPtr& req,
                                  std::function<void(const HttpResponsePtr&)>&& cb,
                                  const std::string& /*p1*/, const std::string& /*p2*/)
{
    const auto [bucket, key] = parseBucketKey(req->getPath());
    const auto& params = req->getParameters();

    if (params.count("acl")) {
        // PutObjectAcl stub — accept silently (we don't enforce ACLs)
        auto res = HttpResponse::newHttpResponse();
        res->setStatusCode(drogon::k200OK);
        addStdHeaders(res);
        cb(res);
    } else if (params.count("partNumber") && params.count("uploadId"))
        doUploadPart(req, std::move(cb), bucket, key);
    else if (!req->getHeader("x-amz-copy-source").empty())
        doCopyObject(req, std::move(cb), bucket, key);
    else
        doPutObject(req, std::move(cb), bucket, key);
}

void ObjectController::handlePost(const HttpRequestPtr& req,
                                   std::function<void(const HttpResponsePtr&)>&& cb,
                                   const std::string& /*p1*/, const std::string& /*p2*/)
{
    const auto [bucket, key] = parseBucketKey(req->getPath());
    const auto& params = req->getParameters();

    if (params.count("uploads"))
        doCreateMpu(req, std::move(cb), bucket, key);
    else if (params.count("uploadId"))
        doCompleteMpu(req, std::move(cb), bucket, key);
    else
        cb(xmlErr(drogon::k400BadRequest, "InvalidRequest",
                  "Unrecognised POST operation.", key, bucket));
}

void ObjectController::handleDelete(const HttpRequestPtr& req,
                                     std::function<void(const HttpResponsePtr&)>&& cb,
                                     const std::string& /*p1*/, const std::string& /*p2*/)
{
    const auto [bucket, key] = parseBucketKey(req->getPath());
    const auto& params = req->getParameters();

    if (params.count("uploadId"))
        doAbortMpu(req, std::move(cb), bucket, key);
    else
        doDeleteObject(req, std::move(cb), bucket, key);
}

void ObjectController::handleHead(const HttpRequestPtr& req,
                                   std::function<void(const HttpResponsePtr&)>&& cb,
                                   const std::string& /*p1*/, const std::string& /*p2*/)
{
    const auto [bucket, key] = parseBucketKey(req->getPath());
    doHeadObject(req, std::move(cb), bucket, key);
}

// ── GetObject ─────────────────────────────────────────────────────────────────

void ObjectController::doGetObject(const HttpRequestPtr& /*req*/,
                                    Cb&& cb,
                                    const std::string& bucket,
                                    const std::string& key)
{
    const auto record = meta_->getObject(bucket, key);
    if (!record) {
        cb(xmlErr(drogon::k404NotFound, "NoSuchKey",
                  "The specified key does not exist.", key, bucket));
        return;
    }

    const auto path = storage_->objectPath(bucket, key);
    if (path.empty()) {
        spdlog::error("GetObject: meta exists but file missing for {}/{}", bucket, key);
        cb(xmlErr(drogon::k500InternalServerError, "InternalError",
                  "We encountered an internal error.", key, bucket));
        return;
    }

    auto res = HttpResponse::newFileResponse(path, "", drogon::CT_NONE);
    res->addHeader("ETag",          "\"" + record->etag + "\"");
    res->addHeader("Content-Type",  record->contentType);
    res->addHeader("Last-Modified", rfc7231Date(record->lastModified));
    res->addHeader("Content-Length",std::to_string(record->size));
    addStdHeaders(res);
    cb(res);
}

// ── HeadObject ────────────────────────────────────────────────────────────────

void ObjectController::doHeadObject(const HttpRequestPtr& /*req*/,
                                     Cb&& cb,
                                     const std::string& bucket,
                                     const std::string& key)
{
    const auto record = meta_->getObject(bucket, key);
    if (!record) {
        // HEAD must not return a body but still uses 404
        auto res = HttpResponse::newHttpResponse();
        res->setStatusCode(drogon::k404NotFound);
        addStdHeaders(res);
        cb(res);
        return;
    }

    auto res = HttpResponse::newHttpResponse();
    res->setStatusCode(drogon::k200OK);
    res->addHeader("ETag",           "\"" + record->etag + "\"");
    res->addHeader("Content-Type",   record->contentType);
    res->addHeader("Content-Length", std::to_string(record->size));
    res->addHeader("Last-Modified",  rfc7231Date(record->lastModified));
    addStdHeaders(res);
    cb(res);
}

// ── PutObject ─────────────────────────────────────────────────────────────────

void ObjectController::doPutObject(const HttpRequestPtr& req,
                                    Cb&& cb,
                                    const std::string& bucket,
                                    const std::string& key)
{
    std::string etag;
    const auto err = storage_->put(bucket, key, req->getBody(), etag);

    if (err == S3Error::BucketNotFound) {
        cb(xmlErr(drogon::k404NotFound, "NoSuchBucket",
                  "The specified bucket does not exist.", key, bucket));
        return;
    }
    if (err == S3Error::InvalidArgument) {
        cb(xmlErr(drogon::k400BadRequest, "InvalidArgument",
                  "The specified object key is invalid.", key, bucket));
        return;
    }
    if (err != S3Error::None) {
        cb(xmlErr(drogon::k500InternalServerError, "InternalError",
                  "We encountered an internal error.", key, bucket));
        return;
    }

    ObjectRecord rec;
    rec.bucket       = bucket;
    rec.key          = key;
    rec.size         = req->getBody().size();
    rec.etag         = etag;
    rec.contentType  = req->getHeader("content-type").empty()
                           ? "application/octet-stream"
                           : req->getHeader("content-type");
    rec.lastModified = Clock::now();
    meta_->putObject(rec);

    auto res = HttpResponse::newHttpResponse();
    res->setStatusCode(drogon::k200OK);
    res->addHeader("ETag", "\"" + etag + "\"");
    addStdHeaders(res);
    cb(res);
}

// ── CopyObject ────────────────────────────────────────────────────────────────

void ObjectController::doCopyObject(const HttpRequestPtr& req,
                                     Cb&& cb,
                                     const std::string& destBucket,
                                     const std::string& destKey)
{
    // x-amz-copy-source: [/]<srcBucket>/<srcKey>
    std::string copySource = req->getHeader("x-amz-copy-source");
    if (!copySource.empty() && copySource[0] == '/') copySource = copySource.substr(1);

    const auto slash = copySource.find('/');
    if (slash == std::string::npos) {
        cb(xmlErr(drogon::k400BadRequest, "InvalidArgument",
                  "x-amz-copy-source is malformed.", destKey, destBucket));
        return;
    }
    const std::string srcBucket = copySource.substr(0, slash);
    const std::string srcKey    = copySource.substr(slash + 1);

    const auto srcRecord = meta_->getObject(srcBucket, srcKey);
    if (!srcRecord) {
        cb(xmlErr(drogon::k404NotFound, "NoSuchKey",
                  "The specified source key does not exist.", srcKey, srcBucket));
        return;
    }

    // Read source bytes via the storage engine and write to destination.
    // This correctly re-uses the existing put() interface and handles
    // nested keys and directory creation transparently.
    const auto srcPath = storage_->objectPath(srcBucket, srcKey);
    if (srcPath.empty()) {
        cb(xmlErr(drogon::k500InternalServerError, "InternalError",
                  "Source object file not found on disk.", srcKey, srcBucket));
        return;
    }

    std::ifstream in(srcPath, std::ios::binary);
    if (!in) {
        cb(xmlErr(drogon::k500InternalServerError, "InternalError",
                  "Cannot read source object.", srcKey, srcBucket));
        return;
    }
    std::string srcData((std::istreambuf_iterator<char>(in)), {});

    std::string etag;
    const auto err = storage_->put(destBucket, destKey, srcData, etag);
    if (err == S3Error::BucketNotFound) {
        cb(xmlErr(drogon::k404NotFound, "NoSuchBucket",
                  "The destination bucket does not exist.", destKey, destBucket));
        return;
    }
    if (err != S3Error::None) {
        cb(xmlErr(drogon::k500InternalServerError, "InternalError",
                  "We encountered an internal error during copy.", destKey, destBucket));
        return;
    }

    // Preserve the source ETag (multipart ETags would differ if we computed MD5)
    const auto now = Clock::now();
    ObjectRecord rec;
    rec.bucket       = destBucket;
    rec.key          = destKey;
    rec.size         = srcRecord->size;
    rec.etag         = srcRecord->etag;  // preserve source; covers multipart ETags
    rec.contentType  = srcRecord->contentType;
    rec.lastModified = now;
    meta_->putObject(rec);

    auto res = HttpResponse::newHttpResponse();
    res->setStatusCode(drogon::k200OK);
    res->setContentTypeCode(drogon::CT_APPLICATION_XML);
    res->setBody(S3XmlBuilder::copyObjectResult(srcRecord->etag, now));
    addStdHeaders(res);
    cb(res);
}

// ── DeleteObject ──────────────────────────────────────────────────────────────

void ObjectController::doDeleteObject(const HttpRequestPtr& /*req*/,
                                       Cb&& cb,
                                       const std::string& bucket,
                                       const std::string& key)
{
    // S3 spec: always return 204, even if the object did not exist
    storage_->remove(bucket, key);
    meta_->deleteObject(bucket, key);

    auto res = HttpResponse::newHttpResponse();
    res->setStatusCode(drogon::k204NoContent);
    addStdHeaders(res);
    cb(res);
}

// ── CreateMultipartUpload ─────────────────────────────────────────────────────

void ObjectController::doCreateMpu(const HttpRequestPtr& req,
                                    Cb&& cb,
                                    const std::string& bucket,
                                    const std::string& key)
{
    if (!meta_->getBucket(bucket)) {
        cb(xmlErr(drogon::k404NotFound, "NoSuchBucket",
                  "The specified bucket does not exist.", key, bucket));
        return;
    }

    const std::string contentType = req->getHeader("content-type").empty()
                                        ? "application/octet-stream"
                                        : req->getHeader("content-type");

    const auto uploadId = meta_->createMultipartUpload(bucket, key, contentType);
    if (uploadId.empty()) {
        cb(xmlErr(drogon::k500InternalServerError, "InternalError",
                  "We encountered an internal error.", key, bucket));
        return;
    }

    auto res = HttpResponse::newHttpResponse();
    res->setStatusCode(drogon::k200OK);
    res->setContentTypeCode(drogon::CT_APPLICATION_XML);
    res->setBody(S3XmlBuilder::initiateMultipartUpload(bucket, key, uploadId));
    addStdHeaders(res);
    cb(res);
}

// ── UploadPart ────────────────────────────────────────────────────────────────

void ObjectController::doUploadPart(const HttpRequestPtr& req,
                                     Cb&& cb,
                                     const std::string& bucket,
                                     const std::string& key)
{
    const std::string& uploadId   = req->getParameter("uploadId");
    const std::string& partNumStr = req->getParameter("partNumber");

    int partNumber = 0;
    try { partNumber = std::stoi(partNumStr); } catch (...) {}
    if (partNumber < 1 || partNumber > 10000) {
        cb(xmlErr(drogon::k400BadRequest, "InvalidArgument",
                  "Part number must be an integer between 1 and 10000.", key, bucket));
        return;
    }

    const auto upload = meta_->getMultipartUpload(uploadId);
    if (!upload || upload->bucket != bucket || upload->key != key) {
        cb(xmlErr(drogon::k404NotFound, "NoSuchUpload",
                  "The specified upload does not exist.", key, bucket));
        return;
    }

    std::string etag;
    const auto err = storage_->putPart(uploadId, partNumber, req->getBody(), etag);
    if (err != S3Error::None) {
        cb(xmlErr(drogon::k500InternalServerError, "InternalError",
                  "We encountered an internal error.", key, bucket));
        return;
    }

    PartInfo part;
    part.partNumber   = partNumber;
    part.etag         = etag;
    part.size         = req->getBody().size();
    part.lastModified = Clock::now();
    meta_->putPart(uploadId, part);

    auto res = HttpResponse::newHttpResponse();
    res->setStatusCode(drogon::k200OK);
    res->addHeader("ETag", "\"" + etag + "\"");
    addStdHeaders(res);
    cb(res);
}

// ── CompleteMultipartUpload ───────────────────────────────────────────────────

void ObjectController::doCompleteMpu(const HttpRequestPtr& req,
                                      Cb&& cb,
                                      const std::string& bucket,
                                      const std::string& key)
{
    const std::string& uploadId = req->getParameter("uploadId");

    const auto upload = meta_->getMultipartUpload(uploadId);
    if (!upload || upload->bucket != bucket || upload->key != key) {
        cb(xmlErr(drogon::k404NotFound, "NoSuchUpload",
                  "The specified upload does not exist.", key, bucket));
        return;
    }

    // Parse request body: <CompleteMultipartUpload><Part>...</Part>...</CompleteMultipartUpload>
    struct ClientPart { int num; std::string etag; };
    std::vector<ClientPart> clientParts;

    {
        pugi::xml_document doc;
        const auto body = req->getBody();
        const auto parseResult = doc.load_buffer(body.data(), body.size());
        if (!parseResult) {
            cb(xmlErr(drogon::k400BadRequest, "MalformedXML",
                      "The XML you provided was not well-formed.", key, bucket));
            return;
        }
        for (auto part : doc.child("CompleteMultipartUpload").children("Part")) {
            ClientPart cp;
            cp.num = part.child("PartNumber").text().as_int();
            std::string etag = part.child("ETag").text().as_string();
            // Strip surrounding quotes if present
            if (!etag.empty() && etag.front() == '"') etag = etag.substr(1);
            if (!etag.empty() && etag.back()  == '"') etag.pop_back();
            cp.etag = etag;
            clientParts.push_back(cp);
        }
    }

    if (clientParts.empty()) {
        cb(xmlErr(drogon::k400BadRequest, "MalformedXML",
                  "The XML did not contain any Part elements.", key, bucket));
        return;
    }

    // Sort client parts by part number (S3 requires ascending order)
    std::sort(clientParts.begin(), clientParts.end(),
              [](const ClientPart& a, const ClientPart& b) { return a.num < b.num; });

    // Validate against stored part records
    const auto storedParts = meta_->listParts(uploadId);
    for (const auto& cp : clientParts) {
        const auto it = std::find_if(storedParts.begin(), storedParts.end(),
            [&](const PartInfo& p) { return p.partNumber == cp.num; });
        if (it == storedParts.end() || it->etag != cp.etag) {
            cb(xmlErr(drogon::k400BadRequest, "InvalidPart",
                      "One or more of the specified parts could not be found or "
                      "the ETag does not match.", key, bucket));
            return;
        }
    }

    // Assemble in the client-specified order
    std::vector<int> partNumbers;
    std::vector<PartInfo> orderedParts;
    for (const auto& cp : clientParts) {
        partNumbers.push_back(cp.num);
        const auto it = std::find_if(storedParts.begin(), storedParts.end(),
            [&](const PartInfo& p) { return p.partNumber == cp.num; });
        orderedParts.push_back(*it);
    }

    const auto assembleErr = storage_->assembleParts(bucket, key, uploadId, partNumbers);
    if (assembleErr == S3Error::InvalidPart) {
        cb(xmlErr(drogon::k400BadRequest, "InvalidPart",
                  "One or more part files are missing on disk.", key, bucket));
        return;
    }
    if (assembleErr != S3Error::None) {
        cb(xmlErr(drogon::k500InternalServerError, "InternalError",
                  "We encountered an internal error assembling parts.", key, bucket));
        return;
    }

    // Compute multipart ETag and store the completed object
    const std::string multiEtag = computeMultipartEtag(orderedParts);

    ObjectRecord rec;
    rec.bucket       = bucket;
    rec.key          = key;
    rec.etag         = multiEtag;
    rec.contentType  = upload->contentType;
    rec.lastModified = Clock::now();
    // Compute total size from part sizes
    rec.size = 0;
    for (const auto& p : orderedParts) rec.size += p.size;
    meta_->putObject(rec);
    meta_->deleteMultipartUpload(uploadId);

    const std::string host     = req->getHeader("host");
    const std::string location = "http://" + host + "/" + bucket + "/" + key;

    auto res = HttpResponse::newHttpResponse();
    res->setStatusCode(drogon::k200OK);
    res->setContentTypeCode(drogon::CT_APPLICATION_XML);
    res->setBody(S3XmlBuilder::completeMultipartUpload(location, bucket, key, multiEtag));
    addStdHeaders(res);
    cb(res);
}

// ── AbortMultipartUpload ──────────────────────────────────────────────────────

void ObjectController::doAbortMpu(const HttpRequestPtr& req,
                                   Cb&& cb,
                                   const std::string& bucket,
                                   const std::string& key)
{
    const std::string& uploadId = req->getParameter("uploadId");

    const auto upload = meta_->getMultipartUpload(uploadId);
    if (!upload || upload->bucket != bucket || upload->key != key) {
        cb(xmlErr(drogon::k404NotFound, "NoSuchUpload",
                  "The specified upload does not exist.", key, bucket));
        return;
    }

    storage_->removeParts(uploadId);
    meta_->deleteMultipartUpload(uploadId);

    auto res = HttpResponse::newHttpResponse();
    res->setStatusCode(drogon::k204NoContent);
    addStdHeaders(res);
    cb(res);
}

// ── ListParts ─────────────────────────────────────────────────────────────────

void ObjectController::doListParts(const HttpRequestPtr& req,
                                    Cb&& cb,
                                    const std::string& bucket,
                                    const std::string& key)
{
    const std::string& uploadId = req->getParameter("uploadId");

    const auto upload = meta_->getMultipartUpload(uploadId);
    if (!upload || upload->bucket != bucket || upload->key != key) {
        cb(xmlErr(drogon::k404NotFound, "NoSuchUpload",
                  "The specified upload does not exist.", key, bucket));
        return;
    }

    const auto parts = meta_->listParts(uploadId);

    auto res = HttpResponse::newHttpResponse();
    res->setStatusCode(drogon::k200OK);
    res->setContentTypeCode(drogon::CT_APPLICATION_XML);
    res->setBody(S3XmlBuilder::listParts(bucket, key, uploadId, parts));
    addStdHeaders(res);
    cb(res);
}

// ── GetObjectAcl (stub — always returns FULL_CONTROL for the owner) ───────────

void ObjectController::doGetAcl(const HttpRequestPtr& /*req*/,
                                  Cb&& cb,
                                  const std::string& bucket,
                                  const std::string& key)
{
    if (!meta_->getObject(bucket, key)) {
        cb(xmlErr(drogon::k404NotFound, "NoSuchKey",
                  "The specified key does not exist.", key, bucket));
        return;
    }

    static const char* kAclXml =
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
    res->setBody(kAclXml);
    addStdHeaders(res);
    cb(res);
}

} // namespace nanobucket
