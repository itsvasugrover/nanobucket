#pragma once

#include "nanobucket/storage/IStorageEngine.h"

#include <filesystem>
#include <string>

namespace nanobucket {

// Local-filesystem implementation of IStorageEngine.
//
// Layout:
//   <dataRoot>/
//   ├── <bucket>/           ← one directory per bucket
//   │   └── <key>           ← key maps directly to path; "/" creates sub-dirs
//   └── .multipart/
//       └── <uploadId>/
//           ├── 0001        ← part 1
//           └── 0002        ← part 2
//
// Atomic writes: every put() writes to <path>.tmp then rename()s to <path>.
// Thread safety: all syscalls operate on distinct paths; rename is atomic on POSIX.
//
// Usage:
//   auto engine = std::make_shared<LocalFsStorageEngine>("/data");
//   if (engine->init() != S3Error::None) { /* fatal */ }
class LocalFsStorageEngine : public IStorageEngine {
public:
    explicit LocalFsStorageEngine(const std::string& dataRoot);

    // Create dataRoot and dataRoot/.multipart/ if they do not exist.
    // Must be called once before any other method.
    S3Error init();

    // ── IStorageEngine ────────────────────────────────────────────────────────

    S3Error createBucket(const std::string& bucket) override;
    S3Error deleteBucket(const std::string& bucket) override;

    S3Error put(const std::string& bucket,
                const std::string& key,
                std::string_view   data,
                std::string&       outEtag) override;

    std::string objectPath(const std::string& bucket,
                           const std::string& key) const override;

    S3Error remove(const std::string& bucket,
                   const std::string& key) override;

    bool exists(const std::string& bucket,
                const std::string& key) const override;

    S3Error putPart(const std::string& uploadId,
                    int                partNumber,
                    std::string_view   data,
                    std::string&       outEtag) override;

    S3Error assembleParts(const std::string&      bucket,
                          const std::string&      key,
                          const std::string&      uploadId,
                          const std::vector<int>& partNumbers) override;

    S3Error removeParts(const std::string& uploadId) override;

private:
    std::filesystem::path dataRoot_;

    // ── Path helpers ──────────────────────────────────────────────────────────

    std::filesystem::path bucketDir(const std::string& bucket) const;
    std::filesystem::path keyPath(const std::string& bucket,
                                  const std::string& key) const;
    std::filesystem::path partDir(const std::string& uploadId) const;
    std::filesystem::path partFilePath(const std::string& uploadId,
                                       int                partNumber) const;
    std::filesystem::path multipartRoot() const;

    // ── Validation ────────────────────────────────────────────────────────────

    // Returns false if the key contains ".." components or starts with "/".
    static bool isValidKey(const std::string& key);

    // ── Crypto ────────────────────────────────────────────────────────────────

    // MD5 of raw bytes → lowercase hex string.
    static std::string md5Hex(const void* data, size_t len);
};

} // namespace nanobucket
