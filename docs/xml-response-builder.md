# XML Response Builder

## Overview

Every non-trivial S3 response body is XML. AWS clients are strict about the schema — an unrecognised tag, wrong namespace, or missing element will cause the client to silently fail or raise a cryptic error. `S3XmlBuilder` centralises all XML generation using `pugixml` so there is a single place to fix schema issues and nothing in the controllers constructs raw strings.

---

## File Map

```
src/xml/
└── S3XmlBuilder.h / .cpp
```

---

## XML Namespace

All S3 list responses carry the namespace declaration on the root element:

```xml
xmlns="http://s3.amazonaws.com/doc/2006-03-01/"
```

Error responses do **not** carry a namespace. Both forms are handled by `S3XmlBuilder`.

---

## Response Schemas

### `ListBuckets`

Returned by `GET /` (list all buckets). Triggered by `aws s3 ls` and `s3cmd ls`.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<ListAllMyBucketsResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Owner>
    <ID>owner-id</ID>
    <DisplayName>owner-display-name</DisplayName>
  </Owner>
  <Buckets>
    <Bucket>
      <Name>my-bucket</Name>
      <CreationDate>2024-01-15T10:30:00.000Z</CreationDate>
    </Bucket>
  </Buckets>
</ListAllMyBucketsResult>
```

### `ListObjectsV2`

Returned by `GET /{bucket}?list-type=2`. Supports prefix, delimiter, and pagination.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Name>my-bucket</Name>
  <Prefix>photos/</Prefix>
  <KeyCount>2</KeyCount>
  <MaxKeys>1000</MaxKeys>
  <Delimiter>/</Delimiter>
  <IsTruncated>false</IsTruncated>
  <Contents>
    <Key>photos/cat.jpg</Key>
    <LastModified>2024-01-15T10:30:00.000Z</LastModified>
    <ETag>&quot;d41d8cd98f00b204e9800998ecf8427e&quot;</ETag>
    <Size>12345</Size>
    <StorageClass>STANDARD</StorageClass>
  </Contents>
  <CommonPrefixes>
    <Prefix>photos/archive/</Prefix>
  </CommonPrefixes>
  <!-- Only present when IsTruncated is true -->
  <NextContinuationToken>opaque-cursor-token</NextContinuationToken>
</ListBucketResult>
```

### `InitiateMultipartUploadResult`

Returned by `POST /{bucket}/{key}?uploads`.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<InitiateMultipartUploadResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Bucket>my-bucket</Bucket>
  <Key>large-file.dat</Key>
  <UploadId>VXBsb2FkIElEIGZvciA2aWWpbmcncyBteS1tb3ZpZS5tMnRzIHVwbG9hZA</UploadId>
</InitiateMultipartUploadResult>
```

### `CompleteMultipartUploadResult`

Returned by `POST /{bucket}/{key}?uploadId=X` after successful assembly.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<CompleteMultipartUploadResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Location>http://localhost:9000/my-bucket/large-file.dat</Location>
  <Bucket>my-bucket</Bucket>
  <Key>large-file.dat</Key>
  <ETag>&quot;d8e8fca2dc0f896fd7cb4cb0031ba249-3&quot;</ETag>
</CompleteMultipartUploadResult>
```

Note: the ETag for a completed multipart upload follows the format `"<md5>-<partCount>"`.

### `ListPartsResult`

Returned by `GET /{bucket}/{key}?uploadId=X`.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<ListPartsResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Bucket>my-bucket</Bucket>
  <Key>large-file.dat</Key>
  <UploadId>VXBsb2FkIElEIGZv...</UploadId>
  <StorageClass>STANDARD</StorageClass>
  <IsTruncated>false</IsTruncated>
  <Part>
    <PartNumber>1</PartNumber>
    <LastModified>2024-01-15T10:30:00.000Z</LastModified>
    <ETag>&quot;a54357aff0632cce46d942af68356b38&quot;</ETag>
    <Size>5242880</Size>
  </Part>
</ListPartsResult>
```

### `ListMultipartUploadsResult`

Returned by `GET /{bucket}?uploads`.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<ListMultipartUploadsResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Bucket>my-bucket</Bucket>
  <IsTruncated>false</IsTruncated>
  <Upload>
    <Key>large-file.dat</Key>
    <UploadId>VXBsb2FkIElEIGZv...</UploadId>
    <StorageClass>STANDARD</StorageClass>
    <Initiated>2024-01-15T10:30:00.000Z</Initiated>
  </Upload>
</ListMultipartUploadsResult>
```

### `CopyObjectResult`

Returned by `PUT /{bucket}/{key}` with `x-amz-copy-source` header.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<CopyObjectResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <LastModified>2024-01-15T10:30:00.000Z</LastModified>
  <ETag>&quot;d41d8cd98f00b204e9800998ecf8427e&quot;</ETag>
</CopyObjectResult>
```

### `Error`

Returned on any failure (4xx / 5xx). No namespace on this one.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Error>
  <Code>NoSuchKey</Code>
  <Message>The specified key does not exist.</Message>
  <Key>photos/missing.jpg</Key>
  <BucketName>my-bucket</BucketName>
  <RequestId>tx-placeholder</RequestId>
  <HostId>nanobucket</HostId>
</Error>
```

---

## `S3XmlBuilder` API Design

```cpp
namespace nanobucket {

class S3XmlBuilder {
public:
    // Each method returns the complete XML document as a std::string,
    // ready to set as the Drogon response body with CT_APPLICATION_XML.

    static std::string listBuckets(
        const std::string&           ownerId,
        const std::string&           ownerName,
        const std::vector<BucketInfo>& buckets);

    static std::string listObjectsV2(
        const std::string&     bucket,
        const std::string&     prefix,
        const std::string&     delimiter,
        int                    maxKeys,
        const ListObjectsResult& result);

    static std::string initiateMultipartUpload(
        const std::string& bucket,
        const std::string& key,
        const std::string& uploadId);

    static std::string completeMultipartUpload(
        const std::string& location,
        const std::string& bucket,
        const std::string& key,
        const std::string& etag);

    static std::string listParts(
        const std::string&          bucket,
        const std::string&          key,
        const std::string&          uploadId,
        const std::vector<PartInfo>& parts,
        bool                        isTruncated = false);

    static std::string listMultipartUploads(
        const std::string&                    bucket,
        const std::vector<MultipartUploadInfo>& uploads);

    static std::string copyObjectResult(
        const std::string& etag,
        const std::chrono::system_clock::time_point& lastModified);

    static std::string error(
        const std::string& code,
        const std::string& message,
        const std::string& key        = "",
        const std::string& bucketName = "");

private:
    // ISO 8601 format: 2024-01-15T10:30:00.000Z
    static std::string formatTimestamp(
        const std::chrono::system_clock::time_point& tp);
};

} // namespace nanobucket
```

---

## pugixml Usage Pattern

```cpp
#include <pugixml.hpp>
#include <sstream>

std::string S3XmlBuilder::error(const std::string& code,
                                const std::string& message,
                                const std::string& key,
                                const std::string& bucketName) {
    pugi::xml_document doc;
    auto decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version")  = "1.0";
    decl.append_attribute("encoding") = "UTF-8";

    auto root = doc.append_child("Error");
    root.append_child("Code").text().set(code.c_str());
    root.append_child("Message").text().set(message.c_str());
    if (!key.empty())        root.append_child("Key").text().set(key.c_str());
    if (!bucketName.empty()) root.append_child("BucketName").text().set(bucketName.c_str());
    root.append_child("RequestId").text().set("tx-placeholder");
    root.append_child("HostId").text().set("nanobucket");

    std::ostringstream oss;
    doc.save(oss, "  ");   // two-space indent
    return oss.str();
}
```

---

## ETag Quoting

S3 returns ETags as double-quoted strings in the response body XML:

```xml
<ETag>&quot;d41d8cd98f00b204e9800998ecf8427e&quot;</ETag>
```

But as unquoted in the `ETag` HTTP response header:

```
ETag: "d41d8cd98f00b204e9800998ecf8427e"
```

`S3XmlBuilder` stores the raw hex digest internally and handles the quoting itself — callers pass the bare digest.

---

## Timestamp Format

All S3 timestamps use ISO 8601 with milliseconds in UTC:

```
2024-01-15T10:30:00.000Z
```

`S3XmlBuilder::formatTimestamp()` handles this conversion from `std::chrono::system_clock::time_point`.
