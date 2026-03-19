// Unit tests for SqliteMetaStore.
//
// Each test creates an in-memory SQLite database (:memory:) so there is no
// filesystem dependency and tests can run in any order without cleanup.

#include <gtest/gtest.h>

#include "meta/SqliteMetaStore.h"

#include <chrono>
#include <string>

using namespace nanobucket;
using Clock = std::chrono::system_clock;

// ─────────────────────────────────────────────────────────────────────────────
// Fixture: a freshly initialised in-memory store for every test
// ─────────────────────────────────────────────────────────────────────────────

class MetaStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<SqliteMetaStore>(":memory:");
        ASSERT_EQ(store_->init(), S3Error::None);
    }

    // Helper: create a bucket and assert it succeeds
    void mkBucket(const std::string& name, const std::string& region = "us-east-1") {
        ASSERT_EQ(store_->createBucket(name, region), S3Error::None);
    }

    // Helper: build a minimal ObjectRecord
    static ObjectRecord makeObj(const std::string& bucket,
                                const std::string& key,
                                uint64_t           size = 42,
                                const std::string& etag = "abc123")
    {
        ObjectRecord r;
        r.bucket       = bucket;
        r.key          = key;
        r.size         = size;
        r.etag         = etag;
        r.contentType  = "application/octet-stream";
        r.lastModified = Clock::now();
        return r;
    }

    std::unique_ptr<SqliteMetaStore> store_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Suite: Bucket CRUD
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(MetaStoreTest, CreateBucket_Basic)
{
    EXPECT_EQ(store_->createBucket("photos", "us-east-1"), S3Error::None);
}

TEST_F(MetaStoreTest, CreateBucket_Duplicate)
{
    mkBucket("photos");
    EXPECT_EQ(store_->createBucket("photos", "us-east-1"), S3Error::BucketAlreadyExists);
}

TEST_F(MetaStoreTest, GetBucket_Exists)
{
    mkBucket("photos", "eu-west-1");
    auto info = store_->getBucket("photos");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->name,   "photos");
    EXPECT_EQ(info->region, "eu-west-1");
}

TEST_F(MetaStoreTest, GetBucket_NotFound)
{
    EXPECT_FALSE(store_->getBucket("nonexistent").has_value());
}

TEST_F(MetaStoreTest, ListBuckets_Empty)
{
    EXPECT_TRUE(store_->listBuckets().empty());
}

TEST_F(MetaStoreTest, ListBuckets_Multiple)
{
    mkBucket("alpha");
    mkBucket("beta");
    mkBucket("gamma");
    const auto buckets = store_->listBuckets();
    EXPECT_EQ(buckets.size(), 3u);
    // Names should all appear
    auto hasName = [&](const std::string& n) {
        return std::any_of(buckets.begin(), buckets.end(),
                           [&](const BucketInfo& b) { return b.name == n; });
    };
    EXPECT_TRUE(hasName("alpha"));
    EXPECT_TRUE(hasName("beta"));
    EXPECT_TRUE(hasName("gamma"));
}

TEST_F(MetaStoreTest, DeleteBucket_Basic)
{
    mkBucket("empty");
    EXPECT_EQ(store_->deleteBucket("empty"), S3Error::None);
    EXPECT_FALSE(store_->getBucket("empty").has_value());
}

TEST_F(MetaStoreTest, DeleteBucket_NotFound)
{
    EXPECT_EQ(store_->deleteBucket("ghost"), S3Error::BucketNotFound);
}

TEST_F(MetaStoreTest, DeleteBucket_NotEmpty)
{
    mkBucket("photos");
    ASSERT_EQ(store_->putObject(makeObj("photos", "cat.jpg")), S3Error::None);
    EXPECT_EQ(store_->deleteBucket("photos"), S3Error::BucketNotEmpty);
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite: Object CRUD
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(MetaStoreTest, PutObject_Basic)
{
    mkBucket("b");
    EXPECT_EQ(store_->putObject(makeObj("b", "key1")), S3Error::None);
}

TEST_F(MetaStoreTest, GetObject_Exists)
{
    mkBucket("b");
    auto obj = makeObj("b", "hello.txt", 1234, "deadbeef");
    obj.contentType = "text/plain";
    ASSERT_EQ(store_->putObject(obj), S3Error::None);

    auto got = store_->getObject("b", "hello.txt");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bucket,      "b");
    EXPECT_EQ(got->key,         "hello.txt");
    EXPECT_EQ(got->size,        1234u);
    EXPECT_EQ(got->etag,        "deadbeef");
    EXPECT_EQ(got->contentType, "text/plain");
}

TEST_F(MetaStoreTest, GetObject_NotFound)
{
    mkBucket("b");
    EXPECT_FALSE(store_->getObject("b", "missing").has_value());
}

TEST_F(MetaStoreTest, PutObject_Upsert)
{
    mkBucket("b");
    ASSERT_EQ(store_->putObject(makeObj("b", "k", 10, "old")), S3Error::None);
    ASSERT_EQ(store_->putObject(makeObj("b", "k", 99, "new")), S3Error::None);
    auto got = store_->getObject("b", "k");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->size, 99u);
    EXPECT_EQ(got->etag, "new");
}

TEST_F(MetaStoreTest, DeleteObject_Basic)
{
    mkBucket("b");
    ASSERT_EQ(store_->putObject(makeObj("b", "k")), S3Error::None);
    EXPECT_EQ(store_->deleteObject("b", "k"), S3Error::None);
    EXPECT_FALSE(store_->getObject("b", "k").has_value());
}

TEST_F(MetaStoreTest, DeleteObject_NotFound)
{
    mkBucket("b");
    EXPECT_EQ(store_->deleteObject("b", "missing"), S3Error::ObjectNotFound);
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite: ListObjects
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(MetaStoreTest, ListObjects_Empty)
{
    mkBucket("b");
    auto res = store_->listObjects("b", "", "", 1000, "");
    EXPECT_TRUE(res.objects.empty());
    EXPECT_FALSE(res.isTruncated);
}

TEST_F(MetaStoreTest, ListObjects_AllKeys)
{
    mkBucket("b");
    ASSERT_EQ(store_->putObject(makeObj("b", "a")), S3Error::None);
    ASSERT_EQ(store_->putObject(makeObj("b", "b")), S3Error::None);
    ASSERT_EQ(store_->putObject(makeObj("b", "c")), S3Error::None);

    auto res = store_->listObjects("b", "", "", 1000, "");
    EXPECT_EQ(res.objects.size(), 3u);
    EXPECT_FALSE(res.isTruncated);
}

TEST_F(MetaStoreTest, ListObjects_Prefix)
{
    mkBucket("b");
    ASSERT_EQ(store_->putObject(makeObj("b", "photos/a.jpg")), S3Error::None);
    ASSERT_EQ(store_->putObject(makeObj("b", "photos/b.jpg")), S3Error::None);
    ASSERT_EQ(store_->putObject(makeObj("b", "docs/readme.txt")), S3Error::None);

    auto res = store_->listObjects("b", "photos/", "", 1000, "");
    EXPECT_EQ(res.objects.size(), 2u);
    for (const auto& obj : res.objects)
        EXPECT_EQ(obj.key.substr(0, 7), "photos/");
}

TEST_F(MetaStoreTest, ListObjects_Delimiter)
{
    mkBucket("b");
    ASSERT_EQ(store_->putObject(makeObj("b", "photos/2024/jan.jpg")), S3Error::None);
    ASSERT_EQ(store_->putObject(makeObj("b", "photos/2024/feb.jpg")), S3Error::None);
    ASSERT_EQ(store_->putObject(makeObj("b", "photos/cat.jpg")),      S3Error::None);

    auto res = store_->listObjects("b", "photos/", "/", 1000, "");
    // cat.jpg → Contents; 2024/jan and 2024/feb → CommonPrefix "photos/2024/"
    EXPECT_EQ(res.objects.size(),       1u);
    EXPECT_EQ(res.commonPrefixes.size(), 1u);
    EXPECT_EQ(res.objects[0].key,        "photos/cat.jpg");
    EXPECT_EQ(res.commonPrefixes[0],     "photos/2024/");
}

TEST_F(MetaStoreTest, ListObjects_Pagination)
{
    mkBucket("b");
    for (int i = 0; i < 5; ++i)
        ASSERT_EQ(store_->putObject(makeObj("b", "k" + std::to_string(i))), S3Error::None);

    // Page 1
    auto page1 = store_->listObjects("b", "", "", 3, "");
    EXPECT_EQ(page1.objects.size(), 3u);
    EXPECT_TRUE(page1.isTruncated);
    EXPECT_FALSE(page1.nextContinuationToken.empty());

    // Page 2
    auto page2 = store_->listObjects("b", "", "", 3, page1.nextContinuationToken);
    EXPECT_EQ(page2.objects.size(), 2u);
    EXPECT_FALSE(page2.isTruncated);

    // All keys across both pages must be distinct
    std::vector<std::string> all;
    for (const auto& o : page1.objects) all.push_back(o.key);
    for (const auto& o : page2.objects) all.push_back(o.key);
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    EXPECT_EQ(all.size(), 5u);
}

TEST_F(MetaStoreTest, ListObjects_KeysAreSorted)
{
    mkBucket("b");
    // Insert out-of-order
    ASSERT_EQ(store_->putObject(makeObj("b", "z")), S3Error::None);
    ASSERT_EQ(store_->putObject(makeObj("b", "a")), S3Error::None);
    ASSERT_EQ(store_->putObject(makeObj("b", "m")), S3Error::None);

    auto res = store_->listObjects("b", "", "", 1000, "");
    ASSERT_EQ(res.objects.size(), 3u);
    EXPECT_EQ(res.objects[0].key, "a");
    EXPECT_EQ(res.objects[1].key, "m");
    EXPECT_EQ(res.objects[2].key, "z");
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite: Multipart Upload
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(MetaStoreTest, CreateMultipartUpload_Basic)
{
    mkBucket("b");
    const auto id = store_->createMultipartUpload("b", "big.bin", "application/octet-stream");
    EXPECT_FALSE(id.empty());
    EXPECT_EQ(id.size(), 32u); // 16 bytes → 32 hex chars
}

TEST_F(MetaStoreTest, CreateMultipartUpload_IdsAreUnique)
{
    mkBucket("b");
    const auto id1 = store_->createMultipartUpload("b", "k", "application/octet-stream");
    const auto id2 = store_->createMultipartUpload("b", "k", "application/octet-stream");
    EXPECT_NE(id1, id2);
}

TEST_F(MetaStoreTest, GetMultipartUpload_Exists)
{
    mkBucket("b");
    const auto id = store_->createMultipartUpload("b", "k", "video/mp4");
    auto info = store_->getMultipartUpload(id);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->uploadId,    id);
    EXPECT_EQ(info->bucket,      "b");
    EXPECT_EQ(info->key,         "k");
    EXPECT_EQ(info->contentType, "video/mp4");
}

TEST_F(MetaStoreTest, GetMultipartUpload_NotFound)
{
    EXPECT_FALSE(store_->getMultipartUpload("nonexistent").has_value());
}

TEST_F(MetaStoreTest, DeleteMultipartUpload_Basic)
{
    mkBucket("b");
    const auto id = store_->createMultipartUpload("b", "k", "application/octet-stream");
    EXPECT_EQ(store_->deleteMultipartUpload(id), S3Error::None);
    EXPECT_FALSE(store_->getMultipartUpload(id).has_value());
}

TEST_F(MetaStoreTest, DeleteMultipartUpload_NotFound)
{
    EXPECT_EQ(store_->deleteMultipartUpload("ghost"), S3Error::UploadNotFound);
}

TEST_F(MetaStoreTest, ListMultipartUploads_Basic)
{
    mkBucket("b");
    store_->createMultipartUpload("b", "k1", "application/octet-stream");
    store_->createMultipartUpload("b", "k2", "application/octet-stream");
    const auto uploads = store_->listMultipartUploads("b");
    EXPECT_EQ(uploads.size(), 2u);
}

TEST_F(MetaStoreTest, ListMultipartUploads_OtherBucketNotIncluded)
{
    mkBucket("b1");
    mkBucket("b2");
    store_->createMultipartUpload("b1", "k", "application/octet-stream");
    EXPECT_TRUE(store_->listMultipartUploads("b2").empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite: Parts
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(MetaStoreTest, PutPart_Basic)
{
    mkBucket("b");
    const auto id = store_->createMultipartUpload("b", "k", "application/octet-stream");
    PartInfo p;
    p.partNumber   = 1;
    p.etag         = "etag1";
    p.size         = 5 * 1024 * 1024;
    p.lastModified = Clock::now();
    EXPECT_EQ(store_->putPart(id, p), S3Error::None);
}

TEST_F(MetaStoreTest, PutPart_Upsert)
{
    mkBucket("b");
    const auto id = store_->createMultipartUpload("b", "k", "application/octet-stream");
    PartInfo p;
    p.partNumber = 1; p.etag = "old"; p.size = 10; p.lastModified = Clock::now();
    ASSERT_EQ(store_->putPart(id, p), S3Error::None);
    p.etag = "new"; p.size = 20;
    ASSERT_EQ(store_->putPart(id, p), S3Error::None);

    const auto parts = store_->listParts(id);
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(parts[0].etag, "new");
    EXPECT_EQ(parts[0].size, 20u);
}

TEST_F(MetaStoreTest, ListParts_OrderedByPartNumber)
{
    mkBucket("b");
    const auto id = store_->createMultipartUpload("b", "k", "application/octet-stream");
    for (int n : {3, 1, 2}) {
        PartInfo p;
        p.partNumber = n; p.etag = "e" + std::to_string(n);
        p.size = 0; p.lastModified = Clock::now();
        store_->putPart(id, p);
    }
    const auto parts = store_->listParts(id);
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0].partNumber, 1);
    EXPECT_EQ(parts[1].partNumber, 2);
    EXPECT_EQ(parts[2].partNumber, 3);
}

TEST_F(MetaStoreTest, DeleteMultipartUpload_CascadeDeletesParts)
{
    mkBucket("b");
    const auto id = store_->createMultipartUpload("b", "k", "application/octet-stream");
    PartInfo p; p.partNumber = 1; p.etag = "e"; p.size = 0; p.lastModified = Clock::now();
    ASSERT_EQ(store_->putPart(id, p), S3Error::None);
    ASSERT_EQ(store_->deleteMultipartUpload(id), S3Error::None);
    // Parts must have been cascade-deleted
    EXPECT_TRUE(store_->listParts(id).empty());
}
