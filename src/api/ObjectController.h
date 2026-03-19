#pragma once

#include "nanobucket/meta/IMetaStore.h"
#include "nanobucket/storage/IStorageEngine.h"
#include "auth/AwsSigV4Filter.h"

#include <drogon/HttpController.h>

#include <memory>
#include <string>
#include <vector>

namespace nanobucket {

// Handles all object-level S3 operations, including multipart upload lifecycle
// steps that share the /{bucket}/{key} URL space:
//
//   PUT    /{bucket}/{key}                         → PutObject
//   PUT    /{bucket}/{key}  x-amz-copy-source      → CopyObject
//   PUT    /{bucket}/{key}?partNumber=N&uploadId=X  → UploadPart
//   GET    /{bucket}/{key}                          → GetObject
//   GET    /{bucket}/{key}?uploadId=X               → ListParts
//   HEAD   /{bucket}/{key}                          → HeadObject
//   DELETE /{bucket}/{key}                          → DeleteObject
//   DELETE /{bucket}/{key}?uploadId=X               → AbortMultipartUpload
//   POST   /{bucket}/{key}?uploads                  → CreateMultipartUpload
//   POST   /{bucket}/{key}?uploadId=X               → CompleteMultipartUpload
//
// Route pattern "/{1}/(.+)" matches keys that contain slashes.
// The handler always parses bucket and key from req->getPath() directly.
class ObjectController : public drogon::HttpController<ObjectController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ObjectController::handleGet,    "/{1}/(.+)", drogon::Get,    "nanobucket::AwsSigV4Filter");
    ADD_METHOD_TO(ObjectController::handlePut,    "/{1}/(.+)", drogon::Put,    "nanobucket::AwsSigV4Filter");
    ADD_METHOD_TO(ObjectController::handlePost,   "/{1}/(.+)", drogon::Post,   "nanobucket::AwsSigV4Filter");
    ADD_METHOD_TO(ObjectController::handleDelete, "/{1}/(.+)", drogon::Delete, "nanobucket::AwsSigV4Filter");
    ADD_METHOD_TO(ObjectController::handleHead,   "/{1}/(.+)", drogon::Head,   "nanobucket::AwsSigV4Filter");
    METHOD_LIST_END

    void handleGet   (const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&&,
                      const std::string&, const std::string&);
    void handlePut   (const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&&,
                      const std::string&, const std::string&);
    void handlePost  (const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&&,
                      const std::string&, const std::string&);
    void handleDelete(const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&&,
                      const std::string&, const std::string&);
    void handleHead  (const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&&,
                      const std::string&, const std::string&);

    static void setDeps(std::shared_ptr<IMetaStore>    meta,
                        std::shared_ptr<IStorageEngine> storage);

private:
    static std::shared_ptr<IMetaStore>    meta_;
    static std::shared_ptr<IStorageEngine> storage_;

    // Individual operation implementations
    using Cb = std::function<void(const drogon::HttpResponsePtr&)>;

    void doGetObject    (const drogon::HttpRequestPtr&, Cb&&, const std::string& bucket, const std::string& key);
    void doHeadObject   (const drogon::HttpRequestPtr&, Cb&&, const std::string& bucket, const std::string& key);
    void doPutObject    (const drogon::HttpRequestPtr&, Cb&&, const std::string& bucket, const std::string& key);
    void doCopyObject   (const drogon::HttpRequestPtr&, Cb&&, const std::string& bucket, const std::string& key);
    void doDeleteObject (const drogon::HttpRequestPtr&, Cb&&, const std::string& bucket, const std::string& key);
    void doUploadPart   (const drogon::HttpRequestPtr&, Cb&&, const std::string& bucket, const std::string& key);
    void doCreateMpu    (const drogon::HttpRequestPtr&, Cb&&, const std::string& bucket, const std::string& key);
    void doCompleteMpu  (const drogon::HttpRequestPtr&, Cb&&, const std::string& bucket, const std::string& key);
    void doAbortMpu     (const drogon::HttpRequestPtr&, Cb&&, const std::string& bucket, const std::string& key);
    void doListParts    (const drogon::HttpRequestPtr&, Cb&&, const std::string& bucket, const std::string& key);
    void doGetAcl       (const drogon::HttpRequestPtr&, Cb&&, const std::string& bucket, const std::string& key);

    // Compute S3 multipart ETag from stored part ETags.
    // Each part etag is a hex MD5; raw bytes are concatenated then MD5'd: "<hex>-N"
    static std::string computeMultipartEtag(const std::vector<PartInfo>& parts);
};

} // namespace nanobucket
