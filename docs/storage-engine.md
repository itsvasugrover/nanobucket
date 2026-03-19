# Local Filesystem Storage Engine

## Overview

`LocalFsStorageEngine` implements `IStorageEngine` using the local filesystem. Each bucket is a directory; each object is a file whose path mirrors the object key. Multipart upload parts are staged in a temporary directory and assembled on completion.

---

## File Map

```
src/storage/
└── LocalFsStorageEngine.h / .cpp
```

---

## Filesystem Layout

```
<data_root>/
├── <bucket-name>/
│   ├── file.txt
│   ├── photos/
│   │   └── cat.jpg          ← key "photos/cat.jpg" maps to this path
│   └── archive/
│       └── backup.tar.gz
└── .multipart/
    └── <uploadId>/
        ├── 0001             ← part 1
        ├── 0002             ← part 2
        └── 0003             ← part 3
```

### Key → Path Mapping

An object key like `photos/2024/jan.jpg` maps directly to `<data_root>/<bucket>/photos/2024/jan.jpg`. Intermediate directories are created as needed on `put()`. This means:
- No encoding or hashing of keys
- Directory listings are human-readable
- `ls <data_root>/<bucket>/` mirrors the logical bucket contents

### Security Consideration

Keys must be sanitised before mapping to paths to prevent directory traversal:
- Reject keys containing `..` components
- Reject keys starting with `/`
- Normalise multiple consecutive `/` to a single `/`

---

## Zero-Copy GET

This is the primary performance feature of the storage engine. Instead of reading file bytes into a user-space buffer and then writing them to the socket, the kernel handles the entire transfer:

```
Disk → Kernel page cache → Socket
            (no copy into user space)
```

The controller calls:
```cpp
auto path = storageEngine_->objectPath(bucket, key);
auto res  = drogon::HttpResponse::newFileResponse(path);
res->addHeader("ETag", record.etag);
res->addHeader("Content-Type", record.contentType);
callback(res);
```

`newFileResponse()` uses `sendfile(2)` on Linux. For a 1 GB object, this means:
- **Without zero-copy**: 1 GB read from disk into a `std::string`, then 1 GB written to socket
- **With zero-copy**: kernel transfers 1 GB directly — user-space memory usage stays flat

---

## ETag Computation

S3 uses the MD5 hex digest of the object bytes as the ETag for single-part uploads. This is computed during `put()` using OpenSSL:

```cpp
S3Error LocalFsStorageEngine::put(const std::string& bucket,
                                   const std::string& key,
                                   std::string_view   data,
                                   std::string&       outEtag) {
    // 1. Create intermediate directories
    // 2. Write data to a temp file, then rename (atomic on POSIX)
    // 3. Compute MD5
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5(reinterpret_cast<const uint8_t*>(data.data()), data.size(), digest);
    outEtag = hexEncode(digest, MD5_DIGEST_LENGTH);
    return S3Error::None;
}
```

**Atomic write**: Write to `<path>.tmp`, then `rename()` to `<path>`. This prevents a crashed write leaving a truncated object readable by concurrent GET requests.

---

## Multipart Upload Flow

### `putPart()` — stage a part

```
.multipart/<uploadId>/0001   ← part 1 bytes written here
.multipart/<uploadId>/0002   ← part 2 bytes written here
```

Part files are named with zero-padded 4-digit part numbers for natural sort order.

### `assembleParts()` — concatenate on complete

```cpp
S3Error LocalFsStorageEngine::assembleParts(
    const std::string&      bucket,
    const std::string&      key,
    const std::string&      uploadId,
    const std::vector<int>& partNumbers) {

    auto destPath = objectPath(bucket, key);
    // open destPath for writing
    for (int partNum : partNumbers) {
        auto partPath = partPath(uploadId, partNum);
        // read partPath, append to destPath
        // or use splice(2) for in-kernel copy between file descriptors
    }
    removeParts(uploadId);
    return S3Error::None;
}
```

For large files, use `splice(2)` (Linux) to copy part data between file descriptors entirely in kernel space, avoiding user-space buffering.

### ETag for Multipart Objects

The ETag of a completed multipart object follows a special S3 format:

```
"<md5-of-concatenated-part-etags-bytes>-<part-count>"
```

Example: `"d8e8fca2dc0f896fd7cb4cb0031ba249-3"` means 3 parts.

The MD5 is computed over the binary concatenation of each part's raw MD5 bytes (not their hex strings). This is computed in `assembleParts()` and returned to the controller to store in the metadata record.

---

## Error Handling

| Operation | Failure condition | `S3Error` returned |
|-----------|------------------|--------------------|
| `createBucket` | Directory already exists | `BucketAlreadyExists` |
| `deleteBucket` | Directory has files | `BucketNotEmpty` |
| `deleteBucket` | Directory missing | `BucketNotFound` |
| `put` | Parent bucket directory missing | `BucketNotFound` |
| `put` | Disk write fails | `InternalError` |
| `remove` | File does not exist | `ObjectNotFound` |
| `putPart` | Staging dir does not exist | `UploadNotFound` |
| `assembleParts` | A part file is missing | `InvalidPart` |

---

## Configuration

The data root directory is read from `config.json` at startup:

```json
{
  "storage": {
    "dataRoot": "./data"
  }
}
```

`LocalFsStorageEngine` creates `<dataRoot>` and `<dataRoot>/.multipart/` on `init()` if they do not exist.

---

## Thread Safety

All filesystem operations (`open`, `write`, `rename`, `unlink`, `mkdir`) are thread-safe at the OS level when operating on different paths. Concurrent writes to the same `(bucket, key)` are serialised by the atomic rename — the last writer wins, matching S3 last-write-wins semantics.
