// Unit tests for LocalFsStorageEngine.
//
// Each test fixture creates a fresh temporary directory and removes it on
// teardown, so tests are fully isolated with no shared state.

#include <gtest/gtest.h>

#include "storage/LocalFsStorageEngine.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace nanobucket;
namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// Fixture
// ─────────────────────────────────────────────────────────────────────────────

class StorageEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        // mkdtemp requires a writable template ending in XXXXXX
        char tmpl[] = "/tmp/s3lite_storage_XXXXXX";
        char* dir   = mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr);
        root_ = dir;

        engine_ = std::make_unique<LocalFsStorageEngine>(root_);
        ASSERT_EQ(engine_->init(), S3Error::None);
    }

    void TearDown() override {
        fs::remove_all(root_);
    }

    // Create a bucket via the engine (asserts success)
    void mkBucket(const std::string& name) {
        ASSERT_EQ(engine_->createBucket(name), S3Error::None);
    }

    // Put a small object and return its ETag
    std::string putObj(const std::string& bucket,
                       const std::string& key,
                       const std::string& data = "hello")
    {
        std::string etag;
        EXPECT_EQ(engine_->put(bucket, key, data, etag), S3Error::None);
        return etag;
    }

    std::string         root_;
    std::unique_ptr<LocalFsStorageEngine> engine_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Suite: init
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(StorageEngineTest, Init_CreatesMultipartDir)
{
    EXPECT_TRUE(fs::is_directory(root_ + "/.multipart"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite: Bucket
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(StorageEngineTest, CreateBucket_CreatesDirectory)
{
    ASSERT_EQ(engine_->createBucket("photos"), S3Error::None);
    EXPECT_TRUE(fs::is_directory(root_ + "/photos"));
}

TEST_F(StorageEngineTest, CreateBucket_Duplicate)
{
    mkBucket("photos");
    EXPECT_EQ(engine_->createBucket("photos"), S3Error::BucketAlreadyExists);
}

TEST_F(StorageEngineTest, DeleteBucket_Basic)
{
    mkBucket("empty");
    EXPECT_EQ(engine_->deleteBucket("empty"), S3Error::None);
    EXPECT_FALSE(fs::exists(root_ + "/empty"));
}

TEST_F(StorageEngineTest, DeleteBucket_NotFound)
{
    EXPECT_EQ(engine_->deleteBucket("ghost"), S3Error::BucketNotFound);
}

TEST_F(StorageEngineTest, DeleteBucket_NotEmpty)
{
    mkBucket("photos");
    putObj("photos", "cat.jpg");
    EXPECT_EQ(engine_->deleteBucket("photos"), S3Error::BucketNotEmpty);
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite: Put / Get / Remove
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(StorageEngineTest, Put_CreatesFile)
{
    mkBucket("b");
    std::string etag;
    ASSERT_EQ(engine_->put("b", "hello.txt", "world", etag), S3Error::None);
    EXPECT_TRUE(fs::is_regular_file(root_ + "/b/hello.txt"));
}

TEST_F(StorageEngineTest, Put_ReturnsCorrectMd5Etag)
{
    mkBucket("b");
    std::string etag;
    // MD5("") = d41d8cd98f00b204e9800998ecf8427e
    ASSERT_EQ(engine_->put("b", "empty.bin", "", etag), S3Error::None);
    EXPECT_EQ(etag, "d41d8cd98f00b204e9800998ecf8427e");
}

TEST_F(StorageEngineTest, Put_KnownMd5)
{
    mkBucket("b");
    std::string etag;
    // MD5("hello world") = 5eb63bbbe01eeed093cb22bb8f5acdc3
    ASSERT_EQ(engine_->put("b", "k", "hello world", etag), S3Error::None);
    EXPECT_EQ(etag, "5eb63bbbe01eeed093cb22bb8f5acdc3");
}

TEST_F(StorageEngineTest, Put_BucketNotFound)
{
    std::string etag;
    EXPECT_EQ(engine_->put("nosuchbucket", "k", "data", etag), S3Error::BucketNotFound);
}

TEST_F(StorageEngineTest, Put_InvalidKey_DotDot)
{
    mkBucket("b");
    std::string etag;
    EXPECT_EQ(engine_->put("b", "../escape", "data", etag), S3Error::InvalidArgument);
}

TEST_F(StorageEngineTest, Put_InvalidKey_AbsolutePath)
{
    mkBucket("b");
    std::string etag;
    EXPECT_EQ(engine_->put("b", "/abs/path", "data", etag), S3Error::InvalidArgument);
}

TEST_F(StorageEngineTest, Put_NestedKey_CreatesSubdirectories)
{
    mkBucket("b");
    std::string etag;
    ASSERT_EQ(engine_->put("b", "photos/2024/jan.jpg", "bytes", etag), S3Error::None);
    EXPECT_TRUE(fs::is_regular_file(root_ + "/b/photos/2024/jan.jpg"));
}

TEST_F(StorageEngineTest, Put_Overwrite)
{
    mkBucket("b");
    std::string etag1, etag2;
    ASSERT_EQ(engine_->put("b", "k", "v1", etag1), S3Error::None);
    ASSERT_EQ(engine_->put("b", "k", "v2", etag2), S3Error::None);
    EXPECT_NE(etag1, etag2);
    // File should contain the new content
    std::ifstream f(root_ + "/b/k");
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "v2");
}

TEST_F(StorageEngineTest, ObjectPath_Exists)
{
    mkBucket("b");
    putObj("b", "k", "data");
    const auto p = engine_->objectPath("b", "k");
    EXPECT_FALSE(p.empty());
    EXPECT_TRUE(fs::is_regular_file(p));
}

TEST_F(StorageEngineTest, ObjectPath_NotExists)
{
    mkBucket("b");
    EXPECT_TRUE(engine_->objectPath("b", "missing").empty());
}

TEST_F(StorageEngineTest, Exists_True)
{
    mkBucket("b");
    putObj("b", "k");
    EXPECT_TRUE(engine_->exists("b", "k"));
}

TEST_F(StorageEngineTest, Exists_False)
{
    mkBucket("b");
    EXPECT_FALSE(engine_->exists("b", "ghost"));
}

TEST_F(StorageEngineTest, Remove_Basic)
{
    mkBucket("b");
    putObj("b", "k");
    EXPECT_EQ(engine_->remove("b", "k"), S3Error::None);
    EXPECT_FALSE(engine_->exists("b", "k"));
}

TEST_F(StorageEngineTest, Remove_NotFound)
{
    mkBucket("b");
    EXPECT_EQ(engine_->remove("b", "ghost"), S3Error::ObjectNotFound);
}

// After removing the last object the bucket directory should be deletable
TEST_F(StorageEngineTest, DeleteBucket_AfterObjectDeleted)
{
    mkBucket("b");
    putObj("b", "k");
    ASSERT_EQ(engine_->remove("b", "k"), S3Error::None);
    EXPECT_EQ(engine_->deleteBucket("b"), S3Error::None);
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite: Multipart
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(StorageEngineTest, PutPart_CreatesFile)
{
    std::string etag;
    ASSERT_EQ(engine_->putPart("upload-1", 1, "part data", etag), S3Error::None);
    EXPECT_TRUE(fs::is_regular_file(root_ + "/.multipart/upload-1/0001"));
}

TEST_F(StorageEngineTest, PutPart_ReturnsEtag)
{
    std::string etag;
    ASSERT_EQ(engine_->putPart("uid", 1, "hello world", etag), S3Error::None);
    EXPECT_EQ(etag, "5eb63bbbe01eeed093cb22bb8f5acdc3");
}

TEST_F(StorageEngineTest, PutPart_ZeroPaddedName)
{
    std::string etag;
    engine_->putPart("uid", 42, "x", etag);
    EXPECT_TRUE(fs::is_regular_file(root_ + "/.multipart/uid/0042"));
}

TEST_F(StorageEngineTest, AssembleParts_ProducesCorrectContent)
{
    mkBucket("b");
    std::string e1, e2, e3;
    ASSERT_EQ(engine_->putPart("uid", 1, "Hello, ",  e1), S3Error::None);
    ASSERT_EQ(engine_->putPart("uid", 2, "world",    e2), S3Error::None);
    ASSERT_EQ(engine_->putPart("uid", 3, "!",        e3), S3Error::None);

    ASSERT_EQ(engine_->assembleParts("b", "greeting.txt", "uid", {1, 2, 3}),
              S3Error::None);

    std::ifstream f(root_ + "/b/greeting.txt");
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "Hello, world!");
}

TEST_F(StorageEngineTest, AssembleParts_RemovesStaging)
{
    mkBucket("b");
    std::string e;
    engine_->putPart("uid", 1, "data", e);

    ASSERT_EQ(engine_->assembleParts("b", "k", "uid", {1}), S3Error::None);
    EXPECT_FALSE(fs::exists(root_ + "/.multipart/uid"));
}

TEST_F(StorageEngineTest, AssembleParts_MissingPartReturnsError)
{
    mkBucket("b");
    std::string e;
    engine_->putPart("uid", 1, "part1", e);
    // part 2 is missing
    EXPECT_EQ(engine_->assembleParts("b", "k", "uid", {1, 2}), S3Error::InvalidPart);
}

TEST_F(StorageEngineTest, AssembleParts_NestedKey)
{
    mkBucket("b");
    std::string e;
    engine_->putPart("uid", 1, "bytes", e);
    ASSERT_EQ(engine_->assembleParts("b", "a/b/c.bin", "uid", {1}), S3Error::None);
    EXPECT_TRUE(fs::is_regular_file(root_ + "/b/a/b/c.bin"));
}

TEST_F(StorageEngineTest, RemoveParts_Basic)
{
    std::string e;
    engine_->putPart("uid", 1, "x", e);
    engine_->putPart("uid", 2, "y", e);
    EXPECT_EQ(engine_->removeParts("uid"), S3Error::None);
    EXPECT_FALSE(fs::exists(root_ + "/.multipart/uid"));
}

TEST_F(StorageEngineTest, RemoveParts_NonexistentIsOk)
{
    // best-effort: never fails even if the dir doesn't exist
    EXPECT_EQ(engine_->removeParts("ghost"), S3Error::None);
}
