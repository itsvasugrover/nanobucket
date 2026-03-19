#include "SigV4Verifier.h"

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <map>
#include <sstream>
#include <stdexcept>

namespace nanobucket {

// ── Constructor ───────────────────────────────────────────────────────────────

SigV4Verifier::SigV4Verifier(std::string accessKey, std::string secretKey)
    : accessKey_(std::move(accessKey)), secretKey_(std::move(secretKey)) {}

// ── Crypto primitives ─────────────────────────────────────────────────────────

std::vector<uint8_t> SigV4Verifier::hmacSha256(const std::vector<uint8_t>& key,
                                                 std::string_view             data)
{
    unsigned int len = EVP_MAX_MD_SIZE;
    std::vector<uint8_t> result(len);
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const uint8_t*>(data.data()), data.size(),
         result.data(), &len);
    result.resize(len);
    return result;
}

std::string SigV4Verifier::sha256Hex(std::string_view data)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const uint8_t*>(data.data()), data.size(), hash);
    return hexEncode(hash, SHA256_DIGEST_LENGTH);
}

std::string SigV4Verifier::hexEncode(const uint8_t* data, size_t len)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out += kHex[(data[i] >> 4) & 0xf];
        out += kHex[data[i] & 0xf];
    }
    return out;
}

std::string SigV4Verifier::uriEncode(std::string_view input, bool encodeSlash)
{
    // Uppercase hex per RFC 3986 §2.1
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 3);
    for (unsigned char c : input) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else if (c == '/' && !encodeSlash) {
            out += '/';
        } else {
            out += '%';
            out += kHex[(c >> 4) & 0xf];
            out += kHex[c & 0xf];
        }
    }
    return out;
}

// ── Signing key derivation ────────────────────────────────────────────────────

std::vector<uint8_t> SigV4Verifier::deriveSigningKey(const std::string& secretKey,
                                                       const std::string& date,
                                                       const std::string& region,
                                                       const std::string& service)
{
    // First key uses "AWS4" + secretKey as a byte string (not as a hex string)
    const std::string seed = "AWS4" + secretKey;
    const std::vector<uint8_t> seedBytes(seed.begin(), seed.end());

    auto kDate    = hmacSha256(seedBytes, date);
    auto kRegion  = hmacSha256(kDate,    region);
    auto kService = hmacSha256(kRegion,  service);
    auto kSigning = hmacSha256(kService, "aws4_request");
    return kSigning;
}

bool SigV4Verifier::constantTimeEqual(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

// ── Helper utilities ──────────────────────────────────────────────────────────

std::string SigV4Verifier::normalizeHeaderValue(const std::string& v)
{
    // Strip leading/trailing whitespace, collapse internal runs to single space
    std::string out;
    bool inSpace = false;
    bool started = false;
    for (char c : v) {
        if (c == ' ' || c == '\t') {
            if (started) inSpace = true;
        } else {
            if (started && inSpace) out += ' ';
            out += c;
            started = true;
            inSpace = false;
        }
    }
    return out;
}

std::time_t SigV4Verifier::parseAmzTimestamp(const std::string& ts)
{
    // Format: 20240115T103000Z  (length 16)
    if (ts.size() != 16 || ts[8] != 'T' || ts[15] != 'Z') return -1;

    std::tm t{};
    auto parse = [&](int off, int len) {
        return std::stoi(ts.substr(static_cast<size_t>(off),
                                   static_cast<size_t>(len)));
    };
    t.tm_year  = parse(0, 4) - 1900;
    t.tm_mon   = parse(4, 2) - 1;
    t.tm_mday  = parse(6, 2);
    t.tm_hour  = parse(9, 2);
    t.tm_min   = parse(11, 2);
    t.tm_sec   = parse(13, 2);
    t.tm_isdst = 0;
    return timegm(&t);
}

// ── Parsing helpers ───────────────────────────────────────────────────────────

std::optional<AuthContext> SigV4Verifier::parseAuthHeader(
    const drogon::HttpRequestPtr& req)
{
    const auto& auth = req->getHeader("authorization");
    if (auth.empty()) return std::nullopt;

    // Must start with "AWS4-HMAC-SHA256 "
    const std::string prefix = "AWS4-HMAC-SHA256 ";
    if (auth.rfind(prefix, 0) != 0) return std::nullopt;

    // Split the rest by ", " into key=value pairs
    std::string rest = auth.substr(prefix.size());
    std::map<std::string, std::string> parts;
    std::istringstream ss(rest);
    std::string token;
    while (std::getline(ss, token, ',')) {
        // trim whitespace
        size_t s = token.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        token = token.substr(s);
        auto eq = token.find('=');
        if (eq == std::string::npos) continue;
        parts[token.substr(0, eq)] = token.substr(eq + 1);
    }

    auto it_cred = parts.find("Credential");
    auto it_sh   = parts.find("SignedHeaders");
    auto it_sig  = parts.find("Signature");
    if (it_cred == parts.end() || it_sh == parts.end() || it_sig == parts.end())
        return std::nullopt;

    // Credential = accessKeyId/date/region/service/aws4_request
    std::vector<std::string> credParts;
    {
        std::istringstream cs(it_cred->second);
        std::string seg;
        while (std::getline(cs, seg, '/')) credParts.push_back(seg);
    }
    if (credParts.size() < 5) return std::nullopt;

    AuthContext ctx;
    ctx.accessKeyId   = credParts[0];
    ctx.date          = credParts[1];
    ctx.region        = credParts[2];
    ctx.service       = credParts[3];
    ctx.signedHeaders = it_sh->second;
    ctx.signature     = it_sig->second;
    ctx.timestamp     = req->getHeader("x-amz-date");
    ctx.presigned     = false;
    return ctx;
}

std::optional<AuthContext> SigV4Verifier::parsePresignedParams(
    const drogon::HttpRequestPtr& req)
{
    const auto& algo = req->getParameter("X-Amz-Algorithm");
    if (algo.empty()) return std::nullopt;
    if (algo != "AWS4-HMAC-SHA256") return std::nullopt;

    const auto& cred    = req->getParameter("X-Amz-Credential");
    const auto& date    = req->getParameter("X-Amz-Date");
    const auto& sh      = req->getParameter("X-Amz-SignedHeaders");
    const auto& sig     = req->getParameter("X-Amz-Signature");
    if (cred.empty() || date.empty() || sh.empty() || sig.empty())
        return std::nullopt;

    std::vector<std::string> credParts;
    {
        std::istringstream cs(cred);
        std::string seg;
        while (std::getline(cs, seg, '/')) credParts.push_back(seg);
    }
    if (credParts.size() < 5) return std::nullopt;

    AuthContext ctx;
    ctx.accessKeyId   = credParts[0];
    ctx.date          = credParts[1];
    ctx.region        = credParts[2];
    ctx.service       = credParts[3];
    ctx.signedHeaders = sh;
    ctx.signature     = sig;
    ctx.timestamp     = date;
    ctx.presigned     = true;
    return ctx;
}

// ── Canonical request components ──────────────────────────────────────────────

std::string SigV4Verifier::canonicalUri(const std::string& path)
{
    if (path.empty()) return "/";

    std::string out;
    std::string segment;
    for (char c : path) {
        if (c == '/') {
            out += uriEncode(segment, true); // encode the segment
            out += '/';
            segment.clear();
        } else {
            segment += c;
        }
    }
    out += uriEncode(segment, true); // trailing segment (no trailing slash)
    return out;
}

// Decode a single percent-encoded character sequence.
// E.g. "%20" → " ", "+" is NOT decoded (query strings use %20 for space).
static std::string pctDecode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const char hi = s[i + 1], lo = s[i + 2];
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int h = hexVal(hi), l = hexVal(lo);
            if (h >= 0 && l >= 0) {
                out += static_cast<char>((h << 4) | l);
                i += 3;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

std::string SigV4Verifier::canonicalQueryStringFromRaw(
    const std::string& rawQuery, bool presigned)
{
    // Split on '&', decode each key=value pair, re-encode with strict RFC 3986.
    // Sorting is implicit: std::map orders keys lexicographically.
    std::map<std::string, std::string> encoded;
    std::istringstream ss(rawQuery);
    std::string token;
    while (std::getline(ss, token, '&')) {
        if (token.empty()) continue;
        auto eq = token.find('=');
        std::string k, v;
        if (eq == std::string::npos) {
            k = token;
        } else {
            k = token.substr(0, eq);
            v = token.substr(eq + 1);
        }
        k = pctDecode(k);
        v = pctDecode(v);
        if (presigned && k == "X-Amz-Signature") continue;
        encoded[uriEncode(k)] = uriEncode(v);
    }

    std::string out;
    for (const auto& [k, v] : encoded) {
        if (!out.empty()) out += '&';
        out += k + '=' + v;
    }
    return out;
}

std::string SigV4Verifier::canonicalQueryString(
    const drogon::HttpRequestPtr& req, bool presigned)
{
    // Parse the raw query string directly instead of using getParameters(),
    // which in Drogon can include form-body parameters for PUT/POST requests
    // and corrupt the canonical query string for binary body uploads.
    return canonicalQueryStringFromRaw(req->getQuery(), presigned);
}

std::string SigV4Verifier::canonicalHeaders(
    const drogon::HttpRequestPtr&   req,
    const std::vector<std::string>& headerNames)
{
    std::string out;
    for (const auto& name : headerNames) {
        std::string value;
        if (name == "host") {
            // Prefer the Host header; fall back to the local address.
            value = req->getHeader("host");
        } else {
            value = req->getHeader(name);
        }
        out += name + ':' + normalizeHeaderValue(value) + '\n';
    }
    return out;
}

std::string SigV4Verifier::payloadHash(const drogon::HttpRequestPtr& req,
                                        bool presigned)
{
    if (presigned) return "UNSIGNED-PAYLOAD";

    const auto& declared = req->getHeader("x-amz-content-sha256");
    if (!declared.empty()) return declared;

    // Compute SHA256 of the request body
    return sha256Hex(req->getBody());
}

// ── Core building blocks ──────────────────────────────────────────────────────

std::string SigV4Verifier::buildCanonicalRequest(
    const drogon::HttpRequestPtr&   req,
    const std::vector<std::string>& signedHeaderNames,
    bool                            presigned) const
{
    // signedHeaderNames must be sorted and lowercased (caller's responsibility).
    std::string signedHeadersStr;
    for (size_t i = 0; i < signedHeaderNames.size(); ++i) {
        if (i) signedHeadersStr += ';';
        signedHeadersStr += signedHeaderNames[i];
    }

    std::string cr;
    // Drogon converts HEAD to GET before calling filters/controllers.
    // isHead() detects the original HEAD even after that conversion.
    const std::string method = req->isHead() ? "HEAD" : req->methodString();
    cr += method;                                        cr += '\n';
    cr += canonicalUri(req->getPath());                  cr += '\n';
    cr += canonicalQueryString(req, presigned);          cr += '\n';
    cr += canonicalHeaders(req, signedHeaderNames);      cr += '\n';
    cr += signedHeadersStr;                              cr += '\n';
    cr += payloadHash(req, presigned);
    return cr;
}

std::string SigV4Verifier::buildStringToSign(
    const std::string& timestamp,
    const std::string& credentialScope,
    const std::string& canonicalRequestHash)
{
    return "AWS4-HMAC-SHA256\n"
         + timestamp + '\n'
         + credentialScope + '\n'
         + canonicalRequestHash;
}

std::string SigV4Verifier::computeSignature(
    const std::string& secretKey,
    const std::string& date,
    const std::string& region,
    const std::string& service,
    const std::string& stringToSign)
{
    auto kSigning = deriveSigningKey(secretKey, date, region, service);
    auto mac      = hmacSha256(kSigning, stringToSign);
    return hexEncode(mac.data(), mac.size());
}

// ── Main verification entry point ─────────────────────────────────────────────

VerifyResult SigV4Verifier::verify(const drogon::HttpRequestPtr& req) const
{
    // 1. Extract auth context (header-based or presigned URL)
    std::optional<AuthContext> ctx = parsePresignedParams(req);
    if (!ctx) ctx = parseAuthHeader(req);

    if (!ctx) {
        return {false, "AccessDenied",
                "Request is missing authentication information."};
    }

    // 2. Validate access key ID
    if (ctx->accessKeyId != accessKey_) {
        return {false, "InvalidAccessKeyId",
                "The AWS access key ID you provided does not exist in our records."};
    }

    // 3. Validate timestamp (±15 minute window)
    if (!ctx->timestamp.empty()) {
        std::time_t reqTime = parseAmzTimestamp(ctx->timestamp);
        if (reqTime < 0) {
            return {false, "AuthorizationHeaderMalformed",
                    "The date value in the request is invalid."};
        }
        auto now   = std::time(nullptr);
        auto delta = std::abs(static_cast<long>(now) - static_cast<long>(reqTime));
        if (delta > 15 * 60) {
            return {false, "RequestTimeTooSkewed",
                    "The difference between the request time and the current time is too large."};
        }
    }

    // 4. Check presigned expiry
    if (ctx->presigned) {
        const auto& expiresStr = req->getParameter("X-Amz-Expires");
        if (!expiresStr.empty()) {
            long expires = std::stol(expiresStr);
            std::time_t reqTime = parseAmzTimestamp(ctx->timestamp);
            auto now = static_cast<long>(std::time(nullptr));
            if (now > static_cast<long>(reqTime) + expires) {
                return {false, "AccessDenied", "Request has expired."};
            }
        }
    }

    // 5. Parse and sort signed header names
    std::vector<std::string> signedHeaderNames;
    {
        std::istringstream ss(ctx->signedHeaders);
        std::string name;
        while (std::getline(ss, name, ';')) {
            if (!name.empty()) signedHeaderNames.push_back(name);
        }
    }
    std::sort(signedHeaderNames.begin(), signedHeaderNames.end());

    // 6. Build canonical request → hash it
    const std::string canonReq = buildCanonicalRequest(req, signedHeaderNames,
                                                        ctx->presigned);
    const std::string canonHash = sha256Hex(canonReq);

    // 7. Build credential scope and string-to-sign
    const std::string credScope = ctx->date + '/' + ctx->region + '/'
                                + ctx->service + "/aws4_request";
    const std::string sts = buildStringToSign(ctx->timestamp, credScope, canonHash);

    // 8. Compute expected signature
    const std::string expected = computeSignature(secretKey_, ctx->date,
                                                   ctx->region, ctx->service, sts);

    // 9. Constant-time compare
    if (!constantTimeEqual(expected, ctx->signature)) {
        return {false, "SignatureDoesNotMatch",
                "The request signature we calculated does not match the signature you provided."};
    }

    return {true, "", ""};
}

} // namespace nanobucket
