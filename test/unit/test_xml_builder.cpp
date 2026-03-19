#include "xml/S3XmlBuilder.h"

#include <gtest/gtest.h>
#include <pugixml.hpp>

#include <chrono>
#include <string>
#include <vector>

using namespace nanobucket;
using tp = std::chrono::system_clock::time_point;

// ── Helpers ───────────────────────────────────────────────────────────────────

// Parse XML and assert it is well-formed.
static pugi::xml_document parse(const std::string& xml)
{
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_string(xml.c_str());
    EXPECT_TRUE(result) << "XML parse error: " << result.description()
                        << "\nInput:\n" << xml;
    return doc;
}

// Return a fixed time point for deterministic timestamp tests.
// 2024-01-15 10:30:00.500 UTC
static tp fixedTime()
{
    // seconds since epoch for 2024-01-15 10:30:00 UTC
    std::tm t{};
    t.tm_year = 124; // 2024
    t.tm_mon  = 0;   // January
    t.tm_mday = 15;
    t.tm_hour = 10;
    t.tm_min  = 30;
    t.tm_sec  = 0;
    auto sec = static_cast<std::time_t>(timegm(&t));
    return std::chrono::system_clock::from_time_t(sec)
         + std::chrono::milliseconds(500);
}

static BucketInfo makeBucket(const std::string& name,
                              const std::string& region = "us-east-1")
{
    return BucketInfo{name, region, fixedTime()};
}

static ObjectRecord makeObject(const std::string& bucket,
                                const std::string& key,
                                uint64_t           size  = 1024,
                                const std::string& etag  = "d41d8cd98f00b204e9800998ecf8427e",
                                const std::string& ct    = "application/octet-stream")
{
    return ObjectRecord{bucket, key, size, etag, ct, fixedTime()};
}

// ── formatTimestamp ───────────────────────────────────────────────────────────

TEST(FormatTimestamp, ProducesIso8601WithMilliseconds)
{
    auto ts = S3XmlBuilder::formatTimestamp(fixedTime());
    EXPECT_EQ(ts, "2024-01-15T10:30:00.500Z");
}

TEST(FormatTimestamp, ZeroMilliseconds)
{
    std::tm t{};
    t.tm_year = 124;
    t.tm_mon  = 5;
    t.tm_mday = 1;
    t.tm_hour = 0;
    t.tm_min  = 0;
    t.tm_sec  = 0;
    auto pt = std::chrono::system_clock::from_time_t(
        static_cast<std::time_t>(timegm(&t)));
    EXPECT_EQ(S3XmlBuilder::formatTimestamp(pt), "2024-06-01T00:00:00.000Z");
}

// ── error ─────────────────────────────────────────────────────────────────────

TEST(Error, BasicCodeAndMessage)
{
    auto xml = S3XmlBuilder::error("NoSuchKey",
                                    "The specified key does not exist.");
    auto doc  = parse(xml);
    auto root = doc.child("Error");

    ASSERT_FALSE(root.empty());
    EXPECT_STREQ(root.child_value("Code"),    "NoSuchKey");
    EXPECT_STREQ(root.child_value("Message"), "The specified key does not exist.");
    EXPECT_STREQ(root.child_value("RequestId"), "nanobucket-request");
    EXPECT_STREQ(root.child_value("HostId"),    "nanobucket");
}

TEST(Error, NoNamespaceOnRoot)
{
    auto xml = S3XmlBuilder::error("InternalError", "Something went wrong.");
    auto doc  = parse(xml);
    // Error responses must not carry the S3 namespace
    EXPECT_TRUE(doc.child("Error").attribute("xmlns").empty());
}

TEST(Error, OptionalKeyIncludedWhenProvided)
{
    auto xml  = S3XmlBuilder::error("NoSuchKey", "msg", "photos/cat.jpg");
    auto root = parse(xml).child("Error");
    EXPECT_STREQ(root.child_value("Key"), "photos/cat.jpg");
}

TEST(Error, OptionalKeyOmittedWhenEmpty)
{
    auto xml  = S3XmlBuilder::error("NoSuchBucket", "msg");
    auto root = parse(xml).child("Error");
    EXPECT_TRUE(root.child("Key").empty());
}

TEST(Error, OptionalBucketNameIncluded)
{
    auto xml  = S3XmlBuilder::error("NoSuchBucket", "msg", "", "my-bucket");
    auto root = parse(xml).child("Error");
    EXPECT_STREQ(root.child_value("BucketName"), "my-bucket");
}

TEST(Error, BothKeyAndBucketName)
{
    auto xml  = S3XmlBuilder::error("NoSuchKey", "msg", "k.txt", "b");
    auto root = parse(xml).child("Error");
    EXPECT_STREQ(root.child_value("Key"),        "k.txt");
    EXPECT_STREQ(root.child_value("BucketName"), "b");
}

// ── listBuckets ───────────────────────────────────────────────────────────────

TEST(ListBuckets, Namespace)
{
    auto xml  = S3XmlBuilder::listBuckets("id", "name", {});
    auto root = parse(xml).child("ListAllMyBucketsResult");
    EXPECT_STREQ(root.attribute("xmlns").value(),
                 "http://s3.amazonaws.com/doc/2006-03-01/");
}

TEST(ListBuckets, OwnerFields)
{
    auto xml   = S3XmlBuilder::listBuckets("owner-id", "owner-name", {});
    auto owner = parse(xml).child("ListAllMyBucketsResult").child("Owner");
    EXPECT_STREQ(owner.child_value("ID"),          "owner-id");
    EXPECT_STREQ(owner.child_value("DisplayName"), "owner-name");
}

TEST(ListBuckets, EmptyBucketList)
{
    auto xml     = S3XmlBuilder::listBuckets("id", "name", {});
    auto buckets = parse(xml).child("ListAllMyBucketsResult").child("Buckets");
    EXPECT_TRUE(buckets.child("Bucket").empty());
}

TEST(ListBuckets, SingleBucket)
{
    auto xml    = S3XmlBuilder::listBuckets("id", "name", {makeBucket("my-bucket")});
    auto bucket = parse(xml)
                      .child("ListAllMyBucketsResult")
                      .child("Buckets")
                      .child("Bucket");
    EXPECT_STREQ(bucket.child_value("Name"),         "my-bucket");
    EXPECT_STREQ(bucket.child_value("CreationDate"), "2024-01-15T10:30:00.500Z");
}

TEST(ListBuckets, MultipleBucketsPreserveOrder)
{
    std::vector<BucketInfo> buckets = {
        makeBucket("alpha"), makeBucket("beta"), makeBucket("gamma")};
    auto xml  = S3XmlBuilder::listBuckets("id", "name", buckets);
    auto node = parse(xml)
                    .child("ListAllMyBucketsResult")
                    .child("Buckets")
                    .child("Bucket");
    EXPECT_STREQ(node.child_value("Name"), "alpha");
    node = node.next_sibling("Bucket");
    EXPECT_STREQ(node.child_value("Name"), "beta");
    node = node.next_sibling("Bucket");
    EXPECT_STREQ(node.child_value("Name"), "gamma");
}

// ── listObjectsV2 ─────────────────────────────────────────────────────────────

TEST(ListObjectsV2, Namespace)
{
    ListObjectsResult r;
    auto xml  = S3XmlBuilder::listObjectsV2("b", "", "", 1000, r);
    auto root = parse(xml).child("ListBucketResult");
    EXPECT_STREQ(root.attribute("xmlns").value(),
                 "http://s3.amazonaws.com/doc/2006-03-01/");
}

TEST(ListObjectsV2, EmptyResult)
{
    ListObjectsResult r;
    auto xml  = S3XmlBuilder::listObjectsV2("my-bucket", "", "", 1000, r);
    auto root = parse(xml).child("ListBucketResult");

    EXPECT_STREQ(root.child_value("Name"),        "my-bucket");
    EXPECT_STREQ(root.child_value("Prefix"),      "");
    EXPECT_STREQ(root.child_value("IsTruncated"), "false");
    EXPECT_STREQ(root.child_value("KeyCount"),    "0");
    EXPECT_STREQ(root.child_value("MaxKeys"),     "1000");
    EXPECT_TRUE(root.child("Contents").empty());
}

TEST(ListObjectsV2, ContentsFields)
{
    ListObjectsResult r;
    r.objects = {makeObject("b", "file.txt", 512,
                             "abc123", "text/plain")};
    auto xml      = S3XmlBuilder::listObjectsV2("b", "", "", 1000, r);
    auto contents = parse(xml).child("ListBucketResult").child("Contents");

    EXPECT_STREQ(contents.child_value("Key"),          "file.txt");
    EXPECT_STREQ(contents.child_value("Size"),         "512");
    EXPECT_STREQ(contents.child_value("StorageClass"), "STANDARD");
    EXPECT_STREQ(contents.child_value("LastModified"), "2024-01-15T10:30:00.500Z");
    // ETag must be double-quoted
    EXPECT_STREQ(contents.child_value("ETag"), "\"abc123\"");
}

TEST(ListObjectsV2, KeyCountIncludesCommonPrefixes)
{
    ListObjectsResult r;
    r.objects       = {makeObject("b", "a.txt"), makeObject("b", "b.txt")};
    r.commonPrefixes = {"photos/", "docs/"};
    auto root = parse(
        S3XmlBuilder::listObjectsV2("b", "", "/", 1000, r))
        .child("ListBucketResult");
    // 2 objects + 2 common prefixes = 4
    EXPECT_STREQ(root.child_value("KeyCount"), "4");
}

TEST(ListObjectsV2, CommonPrefixesEmitted)
{
    ListObjectsResult r;
    r.commonPrefixes = {"photos/", "videos/"};
    auto root = parse(
        S3XmlBuilder::listObjectsV2("b", "p/", "/", 1000, r))
        .child("ListBucketResult");

    auto cp = root.child("CommonPrefixes");
    EXPECT_STREQ(cp.child_value("Prefix"), "photos/");
    cp = cp.next_sibling("CommonPrefixes");
    EXPECT_STREQ(cp.child_value("Prefix"), "videos/");
}

TEST(ListObjectsV2, DelimiterEmittedWhenSet)
{
    ListObjectsResult r;
    auto root = parse(
        S3XmlBuilder::listObjectsV2("b", "", "/", 1000, r))
        .child("ListBucketResult");
    EXPECT_STREQ(root.child_value("Delimiter"), "/");
}

TEST(ListObjectsV2, DelimiterOmittedWhenEmpty)
{
    ListObjectsResult r;
    auto root = parse(
        S3XmlBuilder::listObjectsV2("b", "", "", 1000, r))
        .child("ListBucketResult");
    EXPECT_TRUE(root.child("Delimiter").empty());
}

TEST(ListObjectsV2, TruncatedWithToken)
{
    ListObjectsResult r;
    r.isTruncated          = true;
    r.nextContinuationToken = "next-page-token";
    auto root = parse(
        S3XmlBuilder::listObjectsV2("b", "", "", 10, r))
        .child("ListBucketResult");

    EXPECT_STREQ(root.child_value("IsTruncated"),          "true");
    EXPECT_STREQ(root.child_value("NextContinuationToken"), "next-page-token");
}

TEST(ListObjectsV2, TokenOmittedWhenNotTruncated)
{
    ListObjectsResult r;
    r.isTruncated          = false;
    r.nextContinuationToken = "should-not-appear";
    auto root = parse(
        S3XmlBuilder::listObjectsV2("b", "", "", 10, r))
        .child("ListBucketResult");
    EXPECT_TRUE(root.child("NextContinuationToken").empty());
}

// ── initiateMultipartUpload ───────────────────────────────────────────────────

TEST(InitiateMultipartUpload, Fields)
{
    auto xml  = S3XmlBuilder::initiateMultipartUpload("my-bucket", "key.dat", "upload-123");
    auto root = parse(xml).child("InitiateMultipartUploadResult");

    EXPECT_STREQ(root.attribute("xmlns").value(),
                 "http://s3.amazonaws.com/doc/2006-03-01/");
    EXPECT_STREQ(root.child_value("Bucket"),   "my-bucket");
    EXPECT_STREQ(root.child_value("Key"),      "key.dat");
    EXPECT_STREQ(root.child_value("UploadId"), "upload-123");
}

// ── completeMultipartUpload ───────────────────────────────────────────────────

TEST(CompleteMultipartUpload, Fields)
{
    auto xml  = S3XmlBuilder::completeMultipartUpload(
        "http://localhost:9000/b/k", "b", "k", "deadbeef-3");
    auto root = parse(xml).child("CompleteMultipartUploadResult");

    EXPECT_STREQ(root.attribute("xmlns").value(),
                 "http://s3.amazonaws.com/doc/2006-03-01/");
    EXPECT_STREQ(root.child_value("Location"), "http://localhost:9000/b/k");
    EXPECT_STREQ(root.child_value("Bucket"),   "b");
    EXPECT_STREQ(root.child_value("Key"),      "k");
    // ETag must be double-quoted; multipart format: "<digest>-<count>"
    EXPECT_STREQ(root.child_value("ETag"), "\"deadbeef-3\"");
}

// ── listParts ─────────────────────────────────────────────────────────────────

TEST(ListParts, EmptyParts)
{
    auto xml  = S3XmlBuilder::listParts("b", "k", "uid", {});
    auto root = parse(xml).child("ListPartsResult");

    EXPECT_STREQ(root.attribute("xmlns").value(),
                 "http://s3.amazonaws.com/doc/2006-03-01/");
    EXPECT_STREQ(root.child_value("Bucket"),       "b");
    EXPECT_STREQ(root.child_value("Key"),          "k");
    EXPECT_STREQ(root.child_value("UploadId"),     "uid");
    EXPECT_STREQ(root.child_value("StorageClass"), "STANDARD");
    EXPECT_STREQ(root.child_value("IsTruncated"),  "false");
    EXPECT_TRUE(root.child("Part").empty());
}

TEST(ListParts, PartFields)
{
    PartInfo p{1, "abc123", 5242880, fixedTime()};
    auto xml  = S3XmlBuilder::listParts("b", "k", "uid", {p});
    auto part = parse(xml).child("ListPartsResult").child("Part");

    EXPECT_STREQ(part.child_value("PartNumber"),   "1");
    EXPECT_STREQ(part.child_value("ETag"),         "\"abc123\"");
    EXPECT_STREQ(part.child_value("Size"),         "5242880");
    EXPECT_STREQ(part.child_value("LastModified"), "2024-01-15T10:30:00.500Z");
}

TEST(ListParts, MultiplePartsPreserveOrder)
{
    std::vector<PartInfo> parts = {
        {1, "etag1", 100, fixedTime()},
        {2, "etag2", 200, fixedTime()},
        {3, "etag3", 300, fixedTime()},
    };
    auto root = parse(
        S3XmlBuilder::listParts("b", "k", "uid", parts))
        .child("ListPartsResult");

    auto part = root.child("Part");
    EXPECT_STREQ(part.child_value("PartNumber"), "1");
    part = part.next_sibling("Part");
    EXPECT_STREQ(part.child_value("PartNumber"), "2");
    part = part.next_sibling("Part");
    EXPECT_STREQ(part.child_value("PartNumber"), "3");
}

TEST(ListParts, IsTruncatedTrue)
{
    auto root = parse(
        S3XmlBuilder::listParts("b", "k", "uid", {}, true))
        .child("ListPartsResult");
    EXPECT_STREQ(root.child_value("IsTruncated"), "true");
}

// ── listMultipartUploads ──────────────────────────────────────────────────────

TEST(ListMultipartUploads, EmptyUploads)
{
    auto xml  = S3XmlBuilder::listMultipartUploads("my-bucket", {});
    auto root = parse(xml).child("ListMultipartUploadsResult");

    EXPECT_STREQ(root.attribute("xmlns").value(),
                 "http://s3.amazonaws.com/doc/2006-03-01/");
    EXPECT_STREQ(root.child_value("Bucket"),      "my-bucket");
    EXPECT_STREQ(root.child_value("IsTruncated"), "false");
    EXPECT_TRUE(root.child("Upload").empty());
}

TEST(ListMultipartUploads, UploadFields)
{
    MultipartUploadInfo u{"uid-1", "b", "large.dat", "application/octet-stream",
                           fixedTime()};
    auto xml    = S3XmlBuilder::listMultipartUploads("b", {u});
    auto upload = parse(xml).child("ListMultipartUploadsResult").child("Upload");

    EXPECT_STREQ(upload.child_value("Key"),          "large.dat");
    EXPECT_STREQ(upload.child_value("UploadId"),     "uid-1");
    EXPECT_STREQ(upload.child_value("StorageClass"), "STANDARD");
    EXPECT_STREQ(upload.child_value("Initiated"),    "2024-01-15T10:30:00.500Z");
}

TEST(ListMultipartUploads, MultipleUploadsPreserveOrder)
{
    std::vector<MultipartUploadInfo> uploads = {
        {"uid-1", "b", "a.dat", "", fixedTime()},
        {"uid-2", "b", "b.dat", "", fixedTime()},
    };
    auto root   = parse(
        S3XmlBuilder::listMultipartUploads("b", uploads))
        .child("ListMultipartUploadsResult");

    auto upload = root.child("Upload");
    EXPECT_STREQ(upload.child_value("Key"), "a.dat");
    upload = upload.next_sibling("Upload");
    EXPECT_STREQ(upload.child_value("Key"), "b.dat");
}

// ── copyObjectResult ──────────────────────────────────────────────────────────

TEST(CopyObjectResult, Fields)
{
    auto xml  = S3XmlBuilder::copyObjectResult("cafebabe", fixedTime());
    auto root = parse(xml).child("CopyObjectResult");

    EXPECT_STREQ(root.attribute("xmlns").value(),
                 "http://s3.amazonaws.com/doc/2006-03-01/");
    EXPECT_STREQ(root.child_value("ETag"),         "\"cafebabe\"");
    EXPECT_STREQ(root.child_value("LastModified"), "2024-01-15T10:30:00.500Z");
}

// ── ETag quoting edge-cases ───────────────────────────────────────────────────

TEST(ETagQuoting, QuotesAddedInAllContexts)
{
    const std::string digest = "d41d8cd98f00b204e9800998ecf8427e";
    const std::string quoted = "\"" + digest + "\"";

    // ListObjectsV2 Contents
    {
        ListObjectsResult r;
        r.objects = {makeObject("b", "k", 0, digest)};
        auto xml = S3XmlBuilder::listObjectsV2("b", "", "", 1000, r);
        EXPECT_STREQ(
            parse(xml).child("ListBucketResult").child("Contents").child_value("ETag"),
            quoted.c_str());
    }

    // CompleteMultipartUpload
    {
        auto xml = S3XmlBuilder::completeMultipartUpload("loc", "b", "k", digest);
        EXPECT_STREQ(
            parse(xml).child("CompleteMultipartUploadResult").child_value("ETag"),
            quoted.c_str());
    }

    // CopyObjectResult
    {
        auto xml = S3XmlBuilder::copyObjectResult(digest, fixedTime());
        EXPECT_STREQ(
            parse(xml).child("CopyObjectResult").child_value("ETag"),
            quoted.c_str());
    }

    // ListParts Part
    {
        PartInfo p{1, digest, 100, fixedTime()};
        auto xml = S3XmlBuilder::listParts("b", "k", "u", {p});
        EXPECT_STREQ(
            parse(xml).child("ListPartsResult").child("Part").child_value("ETag"),
            quoted.c_str());
    }
}
