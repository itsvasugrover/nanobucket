#pragma once

#include "nanobucket/meta/IMetaStore.h"
#include "nanobucket/storage/IStorageEngine.h"
#include "auth/AwsSigV4Filter.h"

#include <drogon/HttpController.h>

#include <memory>

namespace nanobucket {

// Handles all bucket-level S3 operations:
//
//   GET  /                         → ListBuckets
//   PUT  /{bucket}                 → CreateBucket
//   DELETE /{bucket}               → DeleteBucket
//   HEAD /{bucket}                 → HeadBucket
//   GET  /{bucket}?list-type=2     → ListObjectsV2
//   GET  /{bucket}?uploads         → ListMultipartUploads
//
// Dependencies are injected once before app().run() via setDeps().
class BucketController : public drogon::HttpController<BucketController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(BucketController::listBuckets,  "/",     drogon::Get,
                  "nanobucket::AwsSigV4Filter");
    // Both with and without trailing slash — s3cmd appends "/" to bucket paths
    ADD_METHOD_TO(BucketController::handleBucket, "/{1}",  drogon::Get, drogon::Put,
                                                           drogon::Delete, drogon::Head,
                                                           drogon::Post,
                  "nanobucket::AwsSigV4Filter");
    ADD_METHOD_TO(BucketController::handleBucket, "/{1}/", drogon::Get, drogon::Put,
                                                           drogon::Delete, drogon::Head,
                                                           drogon::Post,
                  "nanobucket::AwsSigV4Filter");
    METHOD_LIST_END

    // GET /  — ListBuckets
    void listBuckets(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    // Dispatches on method + query params:
    //   PUT     → CreateBucket
    //   DELETE  → DeleteBucket
    //   HEAD    → HeadBucket
    //   GET     → ListObjectsV2  (default)
    //   GET ?uploads → ListMultipartUploads
    void handleBucket(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                      const std::string& bucket);

    static void setDeps(std::shared_ptr<IMetaStore>    meta,
                        std::shared_ptr<IStorageEngine> storage);

private:
    static std::shared_ptr<IMetaStore>    meta_;
    static std::shared_ptr<IStorageEngine> storage_;
};

} // namespace nanobucket
