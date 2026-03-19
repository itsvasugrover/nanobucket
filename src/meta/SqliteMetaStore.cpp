#include "SqliteMetaStore.h"

#include <openssl/rand.h>
#include <spdlog/spdlog.h>

#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace nanobucket {

// ── Time helpers ──────────────────────────────────────────────────────────────

int64_t SqliteMetaStore::toMs(std::chrono::system_clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               tp.time_since_epoch()).count();
}

std::chrono::system_clock::time_point SqliteMetaStore::fromMs(int64_t ms)
{
    return std::chrono::system_clock::time_point{std::chrono::milliseconds{ms}};
}

// ── Base64 ────────────────────────────────────────────────────────────────────

static constexpr const char kB64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string SqliteMetaStore::base64Encode(const std::string& s)
{
    std::string out;
    out.reserve(((s.size() + 2) / 3) * 4);
    unsigned int val = 0;
    int valb = -6;
    for (unsigned char c : s) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out += kB64Table[(val >> valb) & 0x3f];
            valb -= 6;
        }
    }
    if (valb > -6) out += kB64Table[((val << 8) >> (valb + 8)) & 0x3f];
    while (out.size() % 4) out += '=';
    return out;
}

std::string SqliteMetaStore::base64Decode(const std::string& s)
{
    // Build reverse lookup
    static const auto rev = []() {
        std::array<int, 256> t;
        t.fill(-1);
        for (int i = 0; i < 64; ++i) t[static_cast<uint8_t>(kB64Table[i])] = i;
        return t;
    }();

    std::string out;
    out.reserve(s.size() * 3 / 4);
    unsigned int val = 0;
    int valb = -8;
    for (unsigned char c : s) {
        if (rev[c] == -1) break; // padding or end
        val = (val << 6) + static_cast<unsigned>(rev[c]);
        valb += 6;
        if (valb >= 0) {
            out += static_cast<char>((val >> valb) & 0xff);
            valb -= 8;
        }
    }
    return out;
}

// ── Upload ID ─────────────────────────────────────────────────────────────────

std::string SqliteMetaStore::generateUploadId()
{
    unsigned char buf[16];
    RAND_bytes(buf, sizeof(buf));
    std::ostringstream oss;
    for (auto b : buf) oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    return oss.str();
}

// ── Construction / destruction ────────────────────────────────────────────────

SqliteMetaStore::SqliteMetaStore(const std::string& dbPath)
    : dbPath_(dbPath) {}

SqliteMetaStore::~SqliteMetaStore()
{
    finalizeAll();
    if (db_) sqlite3_close(db_);
}

// ── Initialisation ────────────────────────────────────────────────────────────

S3Error SqliteMetaStore::exec(const char* sql)
{
    char* errmsg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        spdlog::error("SqliteMetaStore::exec failed: {} — SQL: {}", errmsg, sql);
        sqlite3_free(errmsg);
        return S3Error::InternalError;
    }
    return S3Error::None;
}

S3Error SqliteMetaStore::init()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (sqlite3_open(dbPath_.c_str(), &db_) != SQLITE_OK) {
        spdlog::error("SqliteMetaStore: cannot open {}: {}",
                      dbPath_, sqlite3_errmsg(db_));
        return S3Error::InternalError;
    }

    // PRAGMAs
    if (exec("PRAGMA journal_mode = WAL")    != S3Error::None) return S3Error::InternalError;
    if (exec("PRAGMA synchronous  = NORMAL") != S3Error::None) return S3Error::InternalError;
    if (exec("PRAGMA foreign_keys = ON")     != S3Error::None) return S3Error::InternalError;
    if (exec("PRAGMA temp_store   = MEMORY") != S3Error::None) return S3Error::InternalError;

    // Schema
    static constexpr const char* kSchema = R"sql(
        CREATE TABLE IF NOT EXISTS buckets (
            name        TEXT    PRIMARY KEY,
            region      TEXT    NOT NULL DEFAULT 'us-east-1',
            created_at  INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS objects (
            bucket        TEXT    NOT NULL,
            key           TEXT    NOT NULL,
            size          INTEGER NOT NULL DEFAULT 0,
            etag          TEXT    NOT NULL,
            content_type  TEXT    NOT NULL DEFAULT 'application/octet-stream',
            last_modified INTEGER NOT NULL,
            PRIMARY KEY (bucket, key),
            FOREIGN KEY (bucket) REFERENCES buckets(name)
        );

        CREATE INDEX IF NOT EXISTS idx_objects_bucket_key
            ON objects (bucket, key);

        CREATE TABLE IF NOT EXISTS multipart_uploads (
            upload_id    TEXT    PRIMARY KEY,
            bucket       TEXT    NOT NULL,
            key          TEXT    NOT NULL,
            content_type TEXT    NOT NULL DEFAULT 'application/octet-stream',
            initiated_at INTEGER NOT NULL,
            FOREIGN KEY (bucket) REFERENCES buckets(name)
        );

        CREATE INDEX IF NOT EXISTS idx_mpu_bucket
            ON multipart_uploads (bucket);

        CREATE TABLE IF NOT EXISTS parts (
            upload_id     TEXT    NOT NULL,
            part_number   INTEGER NOT NULL,
            etag          TEXT    NOT NULL,
            size          INTEGER NOT NULL DEFAULT 0,
            last_modified INTEGER NOT NULL,
            PRIMARY KEY (upload_id, part_number),
            FOREIGN KEY (upload_id) REFERENCES multipart_uploads(upload_id)
                ON DELETE CASCADE
        );
    )sql";

    if (exec(kSchema) != S3Error::None) return S3Error::InternalError;
    return prepareAll();
}

S3Error SqliteMetaStore::prepareAll()
{
    // Helper macro: prepare one statement, bail on error
    auto prep = [&](const char* sql, sqlite3_stmt*& out) -> bool {
        if (sqlite3_prepare_v2(db_, sql, -1, &out, nullptr) != SQLITE_OK) {
            spdlog::error("SqliteMetaStore: prepare failed: {} — {}",
                          sqlite3_errmsg(db_), sql);
            return false;
        }
        return true;
    };

    return (
        prep("SELECT name, region, created_at FROM buckets WHERE name = ?1",
             stmtGetBucket_) &&
        prep("INSERT INTO buckets (name, region, created_at) VALUES (?1, ?2, ?3)",
             stmtInsertBucket_) &&
        prep("SELECT COUNT(*) FROM objects WHERE bucket = ?1",
             stmtCountObjects_) &&
        prep("DELETE FROM buckets WHERE name = ?1",
             stmtDeleteBucket_) &&
        prep("SELECT name, region, created_at FROM buckets ORDER BY created_at ASC",
             stmtListBuckets_) &&
        prep("SELECT bucket, key, size, etag, content_type, last_modified "
             "FROM objects WHERE bucket = ?1 AND key = ?2",
             stmtGetObject_) &&
        prep("INSERT INTO objects (bucket, key, size, etag, content_type, last_modified) "
             "VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
             "ON CONFLICT(bucket, key) DO UPDATE SET "
             "  size = excluded.size, etag = excluded.etag, "
             "  content_type = excluded.content_type, "
             "  last_modified = excluded.last_modified",
             stmtUpsertObject_) &&
        prep("DELETE FROM objects WHERE bucket = ?1 AND key = ?2",
             stmtDeleteObject_) &&
        prep("INSERT INTO multipart_uploads (upload_id, bucket, key, content_type, initiated_at) "
             "VALUES (?1, ?2, ?3, ?4, ?5)",
             stmtInsertMpu_) &&
        prep("SELECT upload_id, bucket, key, content_type, initiated_at "
             "FROM multipart_uploads WHERE upload_id = ?1",
             stmtGetMpu_) &&
        prep("DELETE FROM multipart_uploads WHERE upload_id = ?1",
             stmtDeleteMpu_) &&
        prep("SELECT upload_id, bucket, key, content_type, initiated_at "
             "FROM multipart_uploads WHERE bucket = ?1 ORDER BY initiated_at ASC",
             stmtListMpus_) &&
        prep("INSERT INTO parts (upload_id, part_number, etag, size, last_modified) "
             "VALUES (?1, ?2, ?3, ?4, ?5) "
             "ON CONFLICT(upload_id, part_number) DO UPDATE SET "
             "  etag = excluded.etag, size = excluded.size, "
             "  last_modified = excluded.last_modified",
             stmtUpsertPart_) &&
        prep("SELECT part_number, etag, size, last_modified "
             "FROM parts WHERE upload_id = ?1 ORDER BY part_number ASC",
             stmtListParts_)
    ) ? S3Error::None : S3Error::InternalError;
}

void SqliteMetaStore::finalizeAll()
{
    sqlite3_finalize(stmtGetBucket_);     stmtGetBucket_   = nullptr;
    sqlite3_finalize(stmtInsertBucket_);  stmtInsertBucket_ = nullptr;
    sqlite3_finalize(stmtCountObjects_);  stmtCountObjects_ = nullptr;
    sqlite3_finalize(stmtDeleteBucket_);  stmtDeleteBucket_ = nullptr;
    sqlite3_finalize(stmtListBuckets_);   stmtListBuckets_  = nullptr;
    sqlite3_finalize(stmtGetObject_);     stmtGetObject_    = nullptr;
    sqlite3_finalize(stmtUpsertObject_);  stmtUpsertObject_ = nullptr;
    sqlite3_finalize(stmtDeleteObject_);  stmtDeleteObject_ = nullptr;
    sqlite3_finalize(stmtInsertMpu_);     stmtInsertMpu_    = nullptr;
    sqlite3_finalize(stmtGetMpu_);        stmtGetMpu_       = nullptr;
    sqlite3_finalize(stmtDeleteMpu_);     stmtDeleteMpu_    = nullptr;
    sqlite3_finalize(stmtListMpus_);      stmtListMpus_     = nullptr;
    sqlite3_finalize(stmtUpsertPart_);    stmtUpsertPart_   = nullptr;
    sqlite3_finalize(stmtListParts_);     stmtListParts_    = nullptr;
}

// ── Bucket ────────────────────────────────────────────────────────────────────

S3Error SqliteMetaStore::createBucket(const std::string& name,
                                       const std::string& region)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_reset(stmtInsertBucket_);
    sqlite3_bind_text(stmtInsertBucket_, 1, name.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtInsertBucket_, 2, region.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmtInsertBucket_, 3, toMs(std::chrono::system_clock::now()));

    const int rc = sqlite3_step(stmtInsertBucket_);
    if (rc == SQLITE_CONSTRAINT) return S3Error::BucketAlreadyExists;
    if (rc != SQLITE_DONE) {
        spdlog::error("createBucket: {}", sqlite3_errmsg(db_));
        return S3Error::InternalError;
    }
    return S3Error::None;
}

S3Error SqliteMetaStore::deleteBucket(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Check existence first
    sqlite3_reset(stmtGetBucket_);
    sqlite3_bind_text(stmtGetBucket_, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmtGetBucket_) != SQLITE_ROW) return S3Error::BucketNotFound;

    // Reject if non-empty
    sqlite3_reset(stmtCountObjects_);
    sqlite3_bind_text(stmtCountObjects_, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmtCountObjects_);
    if (sqlite3_column_int64(stmtCountObjects_, 0) > 0) return S3Error::BucketNotEmpty;

    sqlite3_reset(stmtDeleteBucket_);
    sqlite3_bind_text(stmtDeleteBucket_, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmtDeleteBucket_) != SQLITE_DONE) {
        spdlog::error("deleteBucket: {}", sqlite3_errmsg(db_));
        return S3Error::InternalError;
    }
    return S3Error::None;
}

std::optional<BucketInfo> SqliteMetaStore::getBucket(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_reset(stmtGetBucket_);
    sqlite3_bind_text(stmtGetBucket_, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmtGetBucket_) != SQLITE_ROW) return std::nullopt;

    BucketInfo info;
    info.name      = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetBucket_, 0));
    info.region    = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetBucket_, 1));
    info.createdAt = fromMs(sqlite3_column_int64(stmtGetBucket_, 2));
    return info;
}

std::vector<BucketInfo> SqliteMetaStore::listBuckets()
{
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_reset(stmtListBuckets_);
    std::vector<BucketInfo> result;
    while (sqlite3_step(stmtListBuckets_) == SQLITE_ROW) {
        BucketInfo info;
        info.name      = reinterpret_cast<const char*>(sqlite3_column_text(stmtListBuckets_, 0));
        info.region    = reinterpret_cast<const char*>(sqlite3_column_text(stmtListBuckets_, 1));
        info.createdAt = fromMs(sqlite3_column_int64(stmtListBuckets_, 2));
        result.push_back(std::move(info));
    }
    return result;
}

// ── Object ────────────────────────────────────────────────────────────────────

S3Error SqliteMetaStore::putObject(const ObjectRecord& rec)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_reset(stmtUpsertObject_);
    sqlite3_bind_text(stmtUpsertObject_,  1, rec.bucket.c_str(),      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtUpsertObject_,  2, rec.key.c_str(),         -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmtUpsertObject_, 3, static_cast<int64_t>(rec.size));
    sqlite3_bind_text(stmtUpsertObject_,  4, rec.etag.c_str(),        -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtUpsertObject_,  5, rec.contentType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmtUpsertObject_, 6, toMs(rec.lastModified));

    if (sqlite3_step(stmtUpsertObject_) != SQLITE_DONE) {
        spdlog::error("putObject: {}", sqlite3_errmsg(db_));
        return S3Error::InternalError;
    }
    return S3Error::None;
}

S3Error SqliteMetaStore::deleteObject(const std::string& bucket,
                                       const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_reset(stmtDeleteObject_);
    sqlite3_bind_text(stmtDeleteObject_, 1, bucket.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtDeleteObject_, 2, key.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_step(stmtDeleteObject_);
    return sqlite3_changes(db_) > 0 ? S3Error::None : S3Error::ObjectNotFound;
}

std::optional<ObjectRecord> SqliteMetaStore::getObject(const std::string& bucket,
                                                        const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_reset(stmtGetObject_);
    sqlite3_bind_text(stmtGetObject_, 1, bucket.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtGetObject_, 2, key.c_str(),    -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmtGetObject_) != SQLITE_ROW) return std::nullopt;

    ObjectRecord rec;
    rec.bucket      = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetObject_, 0));
    rec.key         = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetObject_, 1));
    rec.size        = static_cast<uint64_t>(sqlite3_column_int64(stmtGetObject_, 2));
    rec.etag        = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetObject_, 3));
    rec.contentType = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetObject_, 4));
    rec.lastModified = fromMs(sqlite3_column_int64(stmtGetObject_, 5));
    return rec;
}

ListObjectsResult SqliteMetaStore::listObjects(const std::string& bucket,
                                                const std::string& prefix,
                                                const std::string& delimiter,
                                                int                maxKeys,
                                                const std::string& continuationToken)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ListObjectsResult result;

    // Decode continuation token → the last key of the previous page.
    // We use key > lastKey to start the next page.
    const bool hasCont = !continuationToken.empty();
    const std::string startKey = hasCont ? base64Decode(continuationToken) : prefix;

    // Upper sentinel: append '\xff' so the range covers all keys with this prefix.
    const std::string upperBound = prefix + '\xff';

    // Both page types use key >= lowerBound:
    //   - First page:   lowerBound = prefix (include everything from prefix onwards)
    //   - Continuation: lowerBound = token  (token encodes the first excluded key of
    //                                        the previous page, so >= resumes correctly)
    const char* sql =
        "SELECT key, size, etag, content_type, last_modified "
        "FROM objects WHERE bucket = ?1 AND key >= ?2 AND key < ?3 "
        "ORDER BY key LIMIT ?4";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("listObjects prepare: {}", sqlite3_errmsg(db_));
        return result;
    }

    // Fetch a generous batch: delimiter folding may collapse many rows into a
    // single CommonPrefix, so we need more rows than maxKeys in the worst case.
    // Cap at 10 000 to avoid runaway memory use.
    const int fetchLimit = std::min(maxKeys * 100 + 1000, 10000);
    sqlite3_bind_text(stmt, 1, bucket.c_str(),     -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, startKey.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, upperBound.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt,  4, fetchLimit);

    std::set<std::string> seenPrefixes;
    int count = 0;
    bool truncated = false;
    std::string lastProcessedKey;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const std::string key =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));

        if (!delimiter.empty()) {
            // Does the key contain the delimiter after the prefix?
            const std::string afterPrefix = key.substr(prefix.size());
            const auto delimPos = afterPrefix.find(delimiter);
            if (delimPos != std::string::npos) {
                const std::string cp =
                    prefix + afterPrefix.substr(0, delimPos + delimiter.size());
                if (seenPrefixes.count(cp) == 0) {
                    // New common prefix — counts as one entry
                    if (count >= maxKeys) {
                        truncated = true;
                        result.nextContinuationToken = base64Encode(key);
                        break;
                    }
                    seenPrefixes.insert(cp);
                    result.commonPrefixes.push_back(cp);
                    ++count;
                    lastProcessedKey = key;
                }
                // Duplicate common prefix — skip without counting
                continue;
            }
        }

        // Regular object entry
        if (count >= maxKeys) {
            truncated = true;
            result.nextContinuationToken = base64Encode(key);
            break;
        }

        ObjectRecord rec;
        rec.bucket       = bucket;
        rec.key          = key;
        rec.size         = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
        rec.etag         = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        rec.contentType  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        rec.lastModified = fromMs(sqlite3_column_int64(stmt, 4));
        result.objects.push_back(std::move(rec));
        ++count;
        lastProcessedKey = key;
    }

    sqlite3_finalize(stmt);
    result.isTruncated = truncated;
    return result;
}

// ── Multipart uploads ─────────────────────────────────────────────────────────

std::string SqliteMetaStore::createMultipartUpload(const std::string& bucket,
                                                    const std::string& key,
                                                    const std::string& contentType)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string uploadId = generateUploadId();

    sqlite3_reset(stmtInsertMpu_);
    sqlite3_bind_text(stmtInsertMpu_,   1, uploadId.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtInsertMpu_,   2, bucket.c_str(),      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtInsertMpu_,   3, key.c_str(),         -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtInsertMpu_,   4, contentType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmtInsertMpu_,  5, toMs(std::chrono::system_clock::now()));

    if (sqlite3_step(stmtInsertMpu_) != SQLITE_DONE) {
        spdlog::error("createMultipartUpload: {}", sqlite3_errmsg(db_));
        return {};
    }
    return uploadId;
}

std::optional<MultipartUploadInfo>
SqliteMetaStore::getMultipartUpload(const std::string& uploadId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_reset(stmtGetMpu_);
    sqlite3_bind_text(stmtGetMpu_, 1, uploadId.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmtGetMpu_) != SQLITE_ROW) return std::nullopt;

    MultipartUploadInfo info;
    info.uploadId    = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetMpu_, 0));
    info.bucket      = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetMpu_, 1));
    info.key         = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetMpu_, 2));
    info.contentType = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetMpu_, 3));
    info.initiatedAt = fromMs(sqlite3_column_int64(stmtGetMpu_, 4));
    return info;
}

S3Error SqliteMetaStore::deleteMultipartUpload(const std::string& uploadId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_reset(stmtDeleteMpu_);
    sqlite3_bind_text(stmtDeleteMpu_, 1, uploadId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmtDeleteMpu_);
    // ON DELETE CASCADE removes all associated parts automatically
    return sqlite3_changes(db_) > 0 ? S3Error::None : S3Error::UploadNotFound;
}

std::vector<MultipartUploadInfo>
SqliteMetaStore::listMultipartUploads(const std::string& bucket)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_reset(stmtListMpus_);
    sqlite3_bind_text(stmtListMpus_, 1, bucket.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<MultipartUploadInfo> result;
    while (sqlite3_step(stmtListMpus_) == SQLITE_ROW) {
        MultipartUploadInfo info;
        info.uploadId    = reinterpret_cast<const char*>(sqlite3_column_text(stmtListMpus_, 0));
        info.bucket      = reinterpret_cast<const char*>(sqlite3_column_text(stmtListMpus_, 1));
        info.key         = reinterpret_cast<const char*>(sqlite3_column_text(stmtListMpus_, 2));
        info.contentType = reinterpret_cast<const char*>(sqlite3_column_text(stmtListMpus_, 3));
        info.initiatedAt = fromMs(sqlite3_column_int64(stmtListMpus_, 4));
        result.push_back(std::move(info));
    }
    return result;
}

// ── Parts ─────────────────────────────────────────────────────────────────────

S3Error SqliteMetaStore::putPart(const std::string& uploadId, const PartInfo& part)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_reset(stmtUpsertPart_);
    sqlite3_bind_text(stmtUpsertPart_,   1, uploadId.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmtUpsertPart_,    2, part.partNumber);
    sqlite3_bind_text(stmtUpsertPart_,   3, part.etag.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmtUpsertPart_,  4, static_cast<int64_t>(part.size));
    sqlite3_bind_int64(stmtUpsertPart_,  5, toMs(part.lastModified));

    if (sqlite3_step(stmtUpsertPart_) != SQLITE_DONE) {
        spdlog::error("putPart: {}", sqlite3_errmsg(db_));
        return S3Error::InternalError;
    }
    return S3Error::None;
}

std::vector<PartInfo> SqliteMetaStore::listParts(const std::string& uploadId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_reset(stmtListParts_);
    sqlite3_bind_text(stmtListParts_, 1, uploadId.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<PartInfo> result;
    while (sqlite3_step(stmtListParts_) == SQLITE_ROW) {
        PartInfo p;
        p.partNumber   = sqlite3_column_int(stmtListParts_, 0);
        p.etag         = reinterpret_cast<const char*>(sqlite3_column_text(stmtListParts_, 1));
        p.size         = static_cast<uint64_t>(sqlite3_column_int64(stmtListParts_, 2));
        p.lastModified = fromMs(sqlite3_column_int64(stmtListParts_, 3));
        result.push_back(std::move(p));
    }
    return result;
}

} // namespace nanobucket
