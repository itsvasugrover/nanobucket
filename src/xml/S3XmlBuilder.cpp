#include "S3XmlBuilder.h"

#include <pugixml.hpp>

#include <ctime>
#include <iomanip>
#include <sstream>

namespace nanobucket {

// ── Private helpers ───────────────────────────────────────────────────────────

std::string S3XmlBuilder::formatTimestamp(
    const std::chrono::system_clock::time_point& tp)
{
    auto tt  = std::chrono::system_clock::to_time_t(tp);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   tp.time_since_epoch()).count() % 1000;

    std::tm utc{};
    gmtime_r(&tt, &utc);

    char buf[24];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &utc);

    std::ostringstream oss;
    oss << buf << '.' << std::setw(3) << std::setfill('0') << ms << 'Z';
    return oss.str();
}

std::string S3XmlBuilder::quotedEtag(const std::string& digest)
{
    return '"' + digest + '"';
}

// Add the XML declaration and return the document as a string.
static std::string serialize(const pugi::xml_document& doc)
{
    std::ostringstream oss;
    doc.save(oss, "  ");
    return oss.str();
}

// Append an XML declaration node to doc and return the root node name child.
static void addDeclaration(pugi::xml_document& doc)
{
    auto decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version")  = "1.0";
    decl.append_attribute("encoding") = "UTF-8";
}

// ── Public methods ────────────────────────────────────────────────────────────

std::string S3XmlBuilder::listBuckets(
    const std::string&             ownerId,
    const std::string&             ownerName,
    const std::vector<BucketInfo>& buckets)
{
    pugi::xml_document doc;
    addDeclaration(doc);

    auto root = doc.append_child("ListAllMyBucketsResult");
    root.append_attribute("xmlns") = kNamespace;

    auto owner = root.append_child("Owner");
    owner.append_child("ID").text().set(ownerId.c_str());
    owner.append_child("DisplayName").text().set(ownerName.c_str());

    auto bucketsNode = root.append_child("Buckets");
    for (const auto& b : buckets) {
        auto node = bucketsNode.append_child("Bucket");
        node.append_child("Name").text().set(b.name.c_str());
        node.append_child("CreationDate").text().set(
            formatTimestamp(b.createdAt).c_str());
    }

    return serialize(doc);
}

std::string S3XmlBuilder::listObjectsV2(
    const std::string&     bucket,
    const std::string&     prefix,
    const std::string&     delimiter,
    int                    maxKeys,
    const ListObjectsResult& result)
{
    pugi::xml_document doc;
    addDeclaration(doc);

    auto root = doc.append_child("ListBucketResult");
    root.append_attribute("xmlns") = kNamespace;

    root.append_child("Name").text().set(bucket.c_str());
    root.append_child("Prefix").text().set(prefix.c_str());
    root.append_child("KeyCount").text().set(
        static_cast<int>(result.objects.size() + result.commonPrefixes.size()));
    root.append_child("MaxKeys").text().set(maxKeys);
    root.append_child("IsTruncated").text().set(result.isTruncated ? "true" : "false");

    if (!delimiter.empty())
        root.append_child("Delimiter").text().set(delimiter.c_str());

    for (const auto& obj : result.objects) {
        auto contents = root.append_child("Contents");
        contents.append_child("Key").text().set(obj.key.c_str());
        contents.append_child("LastModified").text().set(
            formatTimestamp(obj.lastModified).c_str());
        contents.append_child("ETag").text().set(quotedEtag(obj.etag).c_str());
        contents.append_child("Size").text().set(static_cast<long long>(obj.size));
        contents.append_child("StorageClass").text().set("STANDARD");
    }

    for (const auto& cp : result.commonPrefixes) {
        auto node = root.append_child("CommonPrefixes");
        node.append_child("Prefix").text().set(cp.c_str());
    }

    if (result.isTruncated && !result.nextContinuationToken.empty())
        root.append_child("NextContinuationToken").text().set(
            result.nextContinuationToken.c_str());

    return serialize(doc);
}

std::string S3XmlBuilder::initiateMultipartUpload(
    const std::string& bucket,
    const std::string& key,
    const std::string& uploadId)
{
    pugi::xml_document doc;
    addDeclaration(doc);

    auto root = doc.append_child("InitiateMultipartUploadResult");
    root.append_attribute("xmlns") = kNamespace;

    root.append_child("Bucket").text().set(bucket.c_str());
    root.append_child("Key").text().set(key.c_str());
    root.append_child("UploadId").text().set(uploadId.c_str());

    return serialize(doc);
}

std::string S3XmlBuilder::completeMultipartUpload(
    const std::string& location,
    const std::string& bucket,
    const std::string& key,
    const std::string& etag)
{
    pugi::xml_document doc;
    addDeclaration(doc);

    auto root = doc.append_child("CompleteMultipartUploadResult");
    root.append_attribute("xmlns") = kNamespace;

    root.append_child("Location").text().set(location.c_str());
    root.append_child("Bucket").text().set(bucket.c_str());
    root.append_child("Key").text().set(key.c_str());
    root.append_child("ETag").text().set(quotedEtag(etag).c_str());

    return serialize(doc);
}

std::string S3XmlBuilder::listParts(
    const std::string&           bucket,
    const std::string&           key,
    const std::string&           uploadId,
    const std::vector<PartInfo>& parts,
    bool                         isTruncated)
{
    pugi::xml_document doc;
    addDeclaration(doc);

    auto root = doc.append_child("ListPartsResult");
    root.append_attribute("xmlns") = kNamespace;

    root.append_child("Bucket").text().set(bucket.c_str());
    root.append_child("Key").text().set(key.c_str());
    root.append_child("UploadId").text().set(uploadId.c_str());
    root.append_child("StorageClass").text().set("STANDARD");
    root.append_child("IsTruncated").text().set(isTruncated ? "true" : "false");

    for (const auto& p : parts) {
        auto node = root.append_child("Part");
        node.append_child("PartNumber").text().set(p.partNumber);
        node.append_child("LastModified").text().set(
            formatTimestamp(p.lastModified).c_str());
        node.append_child("ETag").text().set(quotedEtag(p.etag).c_str());
        node.append_child("Size").text().set(static_cast<long long>(p.size));
    }

    return serialize(doc);
}

std::string S3XmlBuilder::listMultipartUploads(
    const std::string&                     bucket,
    const std::vector<MultipartUploadInfo>& uploads)
{
    pugi::xml_document doc;
    addDeclaration(doc);

    auto root = doc.append_child("ListMultipartUploadsResult");
    root.append_attribute("xmlns") = kNamespace;

    root.append_child("Bucket").text().set(bucket.c_str());
    root.append_child("IsTruncated").text().set("false");

    for (const auto& u : uploads) {
        auto node = root.append_child("Upload");
        node.append_child("Key").text().set(u.key.c_str());
        node.append_child("UploadId").text().set(u.uploadId.c_str());
        node.append_child("StorageClass").text().set("STANDARD");
        node.append_child("Initiated").text().set(
            formatTimestamp(u.initiatedAt).c_str());
    }

    return serialize(doc);
}

std::string S3XmlBuilder::copyObjectResult(
    const std::string&                           etag,
    const std::chrono::system_clock::time_point& lastModified)
{
    pugi::xml_document doc;
    addDeclaration(doc);

    auto root = doc.append_child("CopyObjectResult");
    root.append_attribute("xmlns") = kNamespace;

    root.append_child("LastModified").text().set(formatTimestamp(lastModified).c_str());
    root.append_child("ETag").text().set(quotedEtag(etag).c_str());

    return serialize(doc);
}

std::string S3XmlBuilder::error(
    const std::string& code,
    const std::string& message,
    const std::string& key,
    const std::string& bucketName)
{
    pugi::xml_document doc;
    addDeclaration(doc);

    auto root = doc.append_child("Error");
    // No namespace on error responses — matches real S3 behaviour

    root.append_child("Code").text().set(code.c_str());
    root.append_child("Message").text().set(message.c_str());
    if (!key.empty())        root.append_child("Key").text().set(key.c_str());
    if (!bucketName.empty()) root.append_child("BucketName").text().set(bucketName.c_str());
    root.append_child("RequestId").text().set("nanobucket-request");
    root.append_child("HostId").text().set("nanobucket");

    return serialize(doc);
}

// POST /{bucket}?delete → DeleteObjects response
std::string S3XmlBuilder::deleteObjects(const std::vector<std::string>& deleted,
                                         const std::vector<std::string>& errors)
{
    pugi::xml_document doc;
    addDeclaration(doc);

    auto root = doc.append_child("DeleteResult");
    root.append_attribute("xmlns") = kNamespace;

    for (const auto& key : deleted) {
        auto d = root.append_child("Deleted");
        d.append_child("Key").text().set(key.c_str());
    }
    for (const auto& key : errors) {
        auto e = root.append_child("Error");
        e.append_child("Key").text().set(key.c_str());
        e.append_child("Code").text().set("InternalError");
        e.append_child("Message").text().set("We encountered an internal error.");
    }

    return serialize(doc);
}

} // namespace nanobucket
