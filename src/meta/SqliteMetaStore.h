#pragma once

#include "nanobucket/meta/IMetaStore.h"

#include <mutex>
#include <sqlite3.h>
#include <string>

namespace nanobucket {

// SQLite-backed implementation of IMetaStore.
//
// A single database connection is guarded by a mutex.  SQLite in WAL mode
// permits one concurrent writer plus unlimited readers, but the same
// sqlite3* handle must not be accessed from multiple threads simultaneously.
// The mutex serialises all calls.  This is acceptable because metadata
// operations are fast and the real I/O bottleneck is object storage.
//
// Usage:
//   auto store = std::make_shared<SqliteMetaStore>("/data/meta.db");
//   if (store->init() != S3Error::None) { /* fatal */ }
class SqliteMetaStore : public IMetaStore {
public:
    explicit SqliteMetaStore(const std::string& dbPath);
    ~SqliteMetaStore() override;

    // Open / create the database, apply PRAGMAs, create tables, prepare stmts.
    // Must be called once before any other method.
    S3Error init();

    // ── IMetaStore ────────────────────────────────────────────────────────────

    S3Error createBucket(const std::string& name,
                         const std::string& region) override;

    S3Error deleteBucket(const std::string& name) override;

    std::optional<BucketInfo>   getBucket(const std::string& name) override;
    std::vector<BucketInfo>     listBuckets()                       override;

    S3Error                     putObject(const ObjectRecord& record) override;
    S3Error                     deleteObject(const std::string& bucket,
                                             const std::string& key)   override;
    std::optional<ObjectRecord> getObject(const std::string& bucket,
                                          const std::string& key)      override;
    ListObjectsResult           listObjects(const std::string& bucket,
                                            const std::string& prefix,
                                            const std::string& delimiter,
                                            int                maxKeys,
                                            const std::string& continuationToken) override;

    std::string createMultipartUpload(const std::string& bucket,
                                      const std::string& key,
                                      const std::string& contentType) override;

    std::optional<MultipartUploadInfo>
    getMultipartUpload(const std::string& uploadId) override;

    S3Error               putPart(const std::string& uploadId,
                                  const PartInfo&    part) override;
    std::vector<PartInfo> listParts(const std::string& uploadId) override;
    S3Error               deleteMultipartUpload(const std::string& uploadId) override;
    std::vector<MultipartUploadInfo>
    listMultipartUploads(const std::string& bucket) override;

private:
    std::string        dbPath_;
    sqlite3*           db_{nullptr};
    mutable std::mutex mutex_;

    // Prepared statements (lifetime = db connection lifetime)
    sqlite3_stmt* stmtGetBucket_{nullptr};
    sqlite3_stmt* stmtInsertBucket_{nullptr};
    sqlite3_stmt* stmtCountObjects_{nullptr};
    sqlite3_stmt* stmtDeleteBucket_{nullptr};
    sqlite3_stmt* stmtListBuckets_{nullptr};

    sqlite3_stmt* stmtGetObject_{nullptr};
    sqlite3_stmt* stmtUpsertObject_{nullptr};
    sqlite3_stmt* stmtDeleteObject_{nullptr};

    sqlite3_stmt* stmtInsertMpu_{nullptr};
    sqlite3_stmt* stmtGetMpu_{nullptr};
    sqlite3_stmt* stmtDeleteMpu_{nullptr};
    sqlite3_stmt* stmtListMpus_{nullptr};

    sqlite3_stmt* stmtUpsertPart_{nullptr};
    sqlite3_stmt* stmtListParts_{nullptr};

    // ── Helpers ───────────────────────────────────────────────────────────────

    // Execute a raw DDL/PRAGMA statement (no results expected).
    S3Error exec(const char* sql);

    // Prepare all cached statements after the connection is open.
    S3Error prepareAll();
    void    finalizeAll();

    // Encode/decode the continuation token (opaque base64 of the last key).
    static std::string base64Encode(const std::string& s);
    static std::string base64Decode(const std::string& s);

    // UUID v4 for upload IDs (32 hex chars, no dashes).
    static std::string generateUploadId();

    // Conversions between time_point and SQLite INTEGER (Unix ms).
    static int64_t toMs(std::chrono::system_clock::time_point tp);
    static std::chrono::system_clock::time_point fromMs(int64_t ms);
};

} // namespace nanobucket
