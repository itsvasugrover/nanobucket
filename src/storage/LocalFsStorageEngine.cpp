#include "LocalFsStorageEngine.h"

#include <openssl/evp.h>
#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace nanobucket {

// ── Path helpers ──────────────────────────────────────────────────────────────

LocalFsStorageEngine::LocalFsStorageEngine(const std::string& dataRoot)
    : dataRoot_(dataRoot) {}

std::filesystem::path LocalFsStorageEngine::multipartRoot() const
{
    return dataRoot_ / ".multipart";
}

std::filesystem::path LocalFsStorageEngine::bucketDir(const std::string& bucket) const
{
    return dataRoot_ / bucket;
}

std::filesystem::path LocalFsStorageEngine::keyPath(const std::string& bucket,
                                                      const std::string& key) const
{
    return dataRoot_ / bucket / key;
}

std::filesystem::path LocalFsStorageEngine::partDir(const std::string& uploadId) const
{
    return multipartRoot() / uploadId;
}

std::filesystem::path LocalFsStorageEngine::partFilePath(const std::string& uploadId,
                                                          int                partNumber) const
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04d", partNumber);
    return partDir(uploadId) / buf;
}

// ── Validation ────────────────────────────────────────────────────────────────

bool LocalFsStorageEngine::isValidKey(const std::string& key)
{
    if (key.empty())     return false;
    if (key[0] == '/')   return false;
    // Walk each path component and reject ".."
    std::filesystem::path p(key);
    for (const auto& component : p) {
        if (component == "..") return false;
    }
    return true;
}

// ── Crypto ────────────────────────────────────────────────────────────────────

std::string LocalFsStorageEngine::md5Hex(const void* data, size_t len)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digestLen = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, digest, &digestLen);
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    for (unsigned int i = 0; i < digestLen; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    return oss.str();
}

// ── Initialisation ────────────────────────────────────────────────────────────

S3Error LocalFsStorageEngine::init()
{
    std::error_code ec;
    std::filesystem::create_directories(dataRoot_,        ec);
    if (ec) {
        spdlog::error("LocalFsStorageEngine: cannot create data root {}: {}",
                      dataRoot_.string(), ec.message());
        return S3Error::InternalError;
    }
    std::filesystem::create_directories(multipartRoot(), ec);
    if (ec) {
        spdlog::error("LocalFsStorageEngine: cannot create multipart root {}: {}",
                      multipartRoot().string(), ec.message());
        return S3Error::InternalError;
    }
    return S3Error::None;
}

// ── Bucket ────────────────────────────────────────────────────────────────────

S3Error LocalFsStorageEngine::createBucket(const std::string& bucket)
{
    const auto dir = bucketDir(bucket);
    if (std::filesystem::exists(dir)) return S3Error::BucketAlreadyExists;

    std::error_code ec;
    std::filesystem::create_directory(dir, ec);
    if (ec) {
        spdlog::error("createBucket {}: {}", bucket, ec.message());
        return S3Error::InternalError;
    }
    return S3Error::None;
}

S3Error LocalFsStorageEngine::deleteBucket(const std::string& bucket)
{
    const auto dir = bucketDir(bucket);
    if (!std::filesystem::exists(dir)) return S3Error::BucketNotFound;

    // Guard: reject if any regular files exist anywhere in the tree
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) return S3Error::BucketNotEmpty;
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (ec) {
        spdlog::error("deleteBucket {}: {}", bucket, ec.message());
        return S3Error::InternalError;
    }
    return S3Error::None;
}

// ── Object ────────────────────────────────────────────────────────────────────

S3Error LocalFsStorageEngine::put(const std::string& bucket,
                                   const std::string& key,
                                   std::string_view   data,
                                   std::string&       outEtag)
{
    if (!isValidKey(key)) return S3Error::InvalidArgument;
    if (!std::filesystem::exists(bucketDir(bucket))) return S3Error::BucketNotFound;

    const auto dest = keyPath(bucket, key);
    const auto tmp  = std::filesystem::path(dest.string() + ".tmp");

    // Create parent directories for nested keys (e.g. "photos/2024/jan.jpg")
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    if (ec) {
        spdlog::error("put {}/{}: create_directories: {}", bucket, key, ec.message());
        return S3Error::InternalError;
    }

    // Write data to temp file
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            spdlog::error("put {}/{}: cannot open tmp file", bucket, key);
            return S3Error::InternalError;
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) {
            spdlog::error("put {}/{}: write failed", bucket, key);
            return S3Error::InternalError;
        }
    }

    // Atomic rename into place
    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        spdlog::error("put {}/{}: rename: {}", bucket, key, ec.message());
        std::filesystem::remove(tmp, ec);
        return S3Error::InternalError;
    }

    outEtag = md5Hex(data.data(), data.size());
    return S3Error::None;
}

std::string LocalFsStorageEngine::objectPath(const std::string& bucket,
                                              const std::string& key) const
{
    if (!isValidKey(key)) return {};
    const auto p = keyPath(bucket, key);
    if (!std::filesystem::is_regular_file(p)) return {};
    return p.string();
}

S3Error LocalFsStorageEngine::remove(const std::string& bucket,
                                      const std::string& key)
{
    if (!isValidKey(key)) return S3Error::InvalidArgument;
    const auto p = keyPath(bucket, key);
    if (!std::filesystem::is_regular_file(p)) return S3Error::ObjectNotFound;

    std::error_code ec;
    std::filesystem::remove(p, ec);
    if (ec) {
        spdlog::error("remove {}/{}: {}", bucket, key, ec.message());
        return S3Error::InternalError;
    }
    return S3Error::None;
}

bool LocalFsStorageEngine::exists(const std::string& bucket,
                                   const std::string& key) const
{
    if (!isValidKey(key)) return false;
    return std::filesystem::is_regular_file(keyPath(bucket, key));
}

// ── Multipart ─────────────────────────────────────────────────────────────────

S3Error LocalFsStorageEngine::putPart(const std::string& uploadId,
                                       int                partNumber,
                                       std::string_view   data,
                                       std::string&       outEtag)
{
    // Create staging directory on first part (upload ID validated by meta store)
    const auto dir = partDir(uploadId);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        spdlog::error("putPart {}/{}: create_directories: {}", uploadId, partNumber, ec.message());
        return S3Error::InternalError;
    }

    const auto dest = partFilePath(uploadId, partNumber);
    const auto tmp  = std::filesystem::path(dest.string() + ".tmp");

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            spdlog::error("putPart {}/{}: cannot open tmp", uploadId, partNumber);
            return S3Error::InternalError;
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) {
            spdlog::error("putPart {}/{}: write failed", uploadId, partNumber);
            return S3Error::InternalError;
        }
    }

    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        spdlog::error("putPart {}/{}: rename: {}", uploadId, partNumber, ec.message());
        std::filesystem::remove(tmp, ec);
        return S3Error::InternalError;
    }

    outEtag = md5Hex(data.data(), data.size());
    return S3Error::None;
}

S3Error LocalFsStorageEngine::assembleParts(const std::string&      bucket,
                                             const std::string&      key,
                                             const std::string&      uploadId,
                                             const std::vector<int>& partNumbers)
{
    if (!isValidKey(key)) return S3Error::InvalidArgument;
    if (!std::filesystem::exists(bucketDir(bucket))) return S3Error::BucketNotFound;

    const auto dest = keyPath(bucket, key);
    const auto tmp  = std::filesystem::path(dest.string() + ".tmp");

    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    if (ec) {
        spdlog::error("assembleParts: create_directories: {}", ec.message());
        return S3Error::InternalError;
    }

    // Concatenate parts into the temp file.
    // EVP_MD_CTX accumulates MD5 across the raw MD5 bytes of each part
    // to produce the multipart ETag: md5(part1_md5_bytes || part2_md5_bytes…)-N
    EVP_MD_CTX* multiMd5Ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(multiMd5Ctx, EVP_md5(), nullptr);

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            EVP_MD_CTX_free(multiMd5Ctx);
            spdlog::error("assembleParts: cannot open tmp {}", tmp.string());
            return S3Error::InternalError;
        }

        static constexpr size_t kBufSize = 1 * 1024 * 1024; // 1 MB copy buffer
        std::vector<char> buf(kBufSize);

        for (int partNum : partNumbers) {
            const auto partPath = partFilePath(uploadId, partNum);
            if (!std::filesystem::is_regular_file(partPath)) {
                EVP_MD_CTX_free(multiMd5Ctx);
                spdlog::error("assembleParts: missing part {} for upload {}",
                              partNum, uploadId);
                return S3Error::InvalidPart;
            }

            // Compute per-part MD5 while copying bytes to the output file.
            EVP_MD_CTX* partCtx = EVP_MD_CTX_new();
            EVP_DigestInit_ex(partCtx, EVP_md5(), nullptr);

            std::ifstream in(partPath, std::ios::binary);
            if (!in) {
                EVP_MD_CTX_free(partCtx);
                EVP_MD_CTX_free(multiMd5Ctx);
                return S3Error::InvalidPart;
            }

            while (in) {
                in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
                const auto n = in.gcount();
                if (n == 0) break;
                out.write(buf.data(), n);
                EVP_DigestUpdate(partCtx, buf.data(), static_cast<size_t>(n));
            }
            if (!out) {
                EVP_MD_CTX_free(partCtx);
                EVP_MD_CTX_free(multiMd5Ctx);
                return S3Error::InternalError;
            }

            // Finalise part MD5 and feed its raw bytes into the multipart MD5
            unsigned char partDigest[EVP_MAX_MD_SIZE];
            unsigned int  partDigestLen = 0;
            EVP_DigestFinal_ex(partCtx, partDigest, &partDigestLen);
            EVP_MD_CTX_free(partCtx);
            EVP_DigestUpdate(multiMd5Ctx, partDigest, partDigestLen);
        }
    } // out closes here

    // Rename into place
    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        EVP_MD_CTX_free(multiMd5Ctx);
        spdlog::error("assembleParts: rename: {}", ec.message());
        std::filesystem::remove(tmp, ec);
        return S3Error::InternalError;
    }

    EVP_MD_CTX_free(multiMd5Ctx);
    removeParts(uploadId);
    return S3Error::None;
}

S3Error LocalFsStorageEngine::removeParts(const std::string& uploadId)
{
    std::error_code ec;
    std::filesystem::remove_all(partDir(uploadId), ec);
    if (ec) {
        spdlog::warn("removeParts {}: {}", uploadId, ec.message());
    }
    return S3Error::None; // best-effort; never fatal
}

} // namespace nanobucket
