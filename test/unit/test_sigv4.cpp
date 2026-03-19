// Unit tests for SigV4Verifier using official AWS SigV4 test vectors.
//
// Test vector source:
//   https://docs.aws.amazon.com/general/latest/gr/sigv4-test-suite.html
//   ("get-vanilla" request from the 2015-08-30 test suite)
//
// Crypto primitives and building blocks are tested in isolation first;
// the full verify() path is exercised last with a live-signed request.

#include <gtest/gtest.h>

#include "auth/SigV4Verifier.h"

#include <drogon/HttpRequest.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace nanobucket;

// ─────────────────────────────────────────────────────────────────────────────
// Constants from the official AWS SigV4 test suite (get-vanilla, 20150830)
// ─────────────────────────────────────────────────────────────────────────────
namespace aws_tv {

static constexpr const char* kAccessKey = "AKIDEXAMPLE";
static constexpr const char* kSecretKey =
    "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
static constexpr const char* kRegion    = "us-east-1";
static constexpr const char* kService   = "service";
static constexpr const char* kDate      = "20150830";
static constexpr const char* kTimestamp = "20150830T123600Z";

// SHA-256 of the empty string
static constexpr const char* kEmptySha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

// Canonical request for "get-vanilla"
static const std::string kCanonicalRequest =
    "GET\n"
    "/\n"
    "\n"
    "host:example.amazonaws.com\n"
    "x-amz-date:20150830T123600Z\n"
    "\n"
    "host;x-amz-date\n"
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

// SHA-256 of the canonical request above
static constexpr const char* kCanonicalRequestHash =
    "816cd5b414d056048ba4f7c5386d6e0533120fb1fcfa93762cf0fc39e2cf19e0";

// Credential scope
static constexpr const char* kCredentialScope =
    "20150830/us-east-1/service/aws4_request";

// String-to-sign
static const std::string kStringToSign =
    "AWS4-HMAC-SHA256\n"
    "20150830T123600Z\n"
    "20150830/us-east-1/service/aws4_request\n"
    "816cd5b414d056048ba4f7c5386d6e0533120fb1fcfa93762cf0fc39e2cf19e0";

// Expected signature for "get-vanilla"
static constexpr const char* kSignature =
    "b97d918cfa904a5beff61c982a1b6f458b799221646efd99d3219ec94cdf2500";

} // namespace aws_tv

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Return the current UTC time as "YYYYMMDDTHHMMSSz" (compact ISO-8601) and
// the date portion "YYYYMMDD".  Used by the round-trip verify() test.
static void currentUtcStamps(std::string& timestamp, std::string& date)
{
    auto now = std::time(nullptr);
    std::tm t{};
    gmtime_r(&now, &t);
    char tsBuf[17], dBuf[9];
    std::strftime(tsBuf, sizeof(tsBuf), "%Y%m%dT%H%M%SZ", &t);
    std::strftime(dBuf,  sizeof(dBuf),  "%Y%m%d",          &t);
    timestamp = tsBuf;
    date      = dBuf;
}

// Build a minimal Drogon GET request for round-trip testing.
static drogon::HttpRequestPtr makeRequest(
    const std::string& path,
    const std::string& host,
    const std::string& timestamp,
    const std::string& body = "")
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath(path);
    req->addHeader("host",         host);
    req->addHeader("x-amz-date",  timestamp);
    if (!body.empty()) {
        req->setBody(body);
        req->addHeader("x-amz-content-sha256",
                       SigV4Verifier::sha256Hex(body));
    } else {
        req->addHeader("x-amz-content-sha256", aws_tv::kEmptySha256);
    }
    return req;
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite: Sha256Hex
// ─────────────────────────────────────────────────────────────────────────────
TEST(Sha256Hex, EmptyString)
{
    EXPECT_EQ(SigV4Verifier::sha256Hex(""), aws_tv::kEmptySha256);
}

TEST(Sha256Hex, KnownString)
{
    // SHA-256("The quick brown fox jumps over the lazy dog")
    EXPECT_EQ(
        SigV4Verifier::sha256Hex("The quick brown fox jumps over the lazy dog"),
        "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

TEST(Sha256Hex, OutputIsLowercase)
{
    const auto h = SigV4Verifier::sha256Hex("hello");
    for (char c : h) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "unexpected char: " << c;
    }
}

TEST(Sha256Hex, OutputLength)
{
    EXPECT_EQ(SigV4Verifier::sha256Hex("").size(), 64u);
    EXPECT_EQ(SigV4Verifier::sha256Hex("abc").size(), 64u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite: HexEncode
// ─────────────────────────────────────────────────────────────────────────────
TEST(HexEncode, ZeroBytes)
{
    uint8_t data[] = {0x00, 0xff, 0xab};
    EXPECT_EQ(SigV4Verifier::hexEncode(data, 3), "00ffab");
}

TEST(HexEncode, Empty)
{
    EXPECT_EQ(SigV4Verifier::hexEncode(nullptr, 0), "");
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite: UriEncode
// ─────────────────────────────────────────────────────────────────────────────
TEST(UriEncode, UnreservedCharsPassThrough)
{
    const std::string unreserved = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~";
    EXPECT_EQ(SigV4Verifier::uriEncode(unreserved, true), unreserved);
}

TEST(UriEncode, SpaceEncoded)
{
    EXPECT_EQ(SigV4Verifier::uriEncode("hello world", true), "hello%20world");
}

TEST(UriEncode, SlashEncodedWhenRequested)
{
    EXPECT_EQ(SigV4Verifier::uriEncode("a/b", true), "a%2Fb");
}

TEST(UriEncode, SlashPassThroughWhenNotEncoded)
{
    EXPECT_EQ(SigV4Verifier::uriEncode("a/b", false), "a/b");
}

TEST(UriEncode, UppercaseHex)
{
    // '%' itself is encoded; verify the hex digits are uppercase
    EXPECT_EQ(SigV4Verifier::uriEncode("%"), "%25");
    EXPECT_EQ(SigV4Verifier::uriEncode("\xff", true), "%FF");
}

TEST(UriEncode, SpecialS3Chars)
{
    EXPECT_EQ(SigV4Verifier::uriEncode("my key+name?", true), "my%20key%2Bname%3F");
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite: HmacSha256
// ─────────────────────────────────────────────────────────────────────────────
// RFC 4231 test vector #1:
//   Key    = 0x0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b (20 bytes)
//   Data   = "Hi There"
//   HMAC   = b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7
TEST(HmacSha256, Rfc4231Vector1)
{
    std::vector<uint8_t> key(20, 0x0b);
    auto mac = SigV4Verifier::hmacSha256(key, "Hi There");
    EXPECT_EQ(SigV4Verifier::hexEncode(mac.data(), mac.size()),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

// RFC 4231 test vector #2:
//   Key    = "Jefe"
//   Data   = "what do ya want for nothing?"
//   HMAC   = 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843
//   (verified with: echo -n "what do ya want for nothing?" |
//    openssl dgst -sha256 -mac HMAC -macopt key:Jefe)
TEST(HmacSha256, Rfc4231Vector2)
{
    std::string keyStr = "Jefe";
    std::vector<uint8_t> key(keyStr.begin(), keyStr.end());
    auto mac = SigV4Verifier::hmacSha256(key, "what do ya want for nothing?");
    EXPECT_EQ(SigV4Verifier::hexEncode(mac.data(), mac.size()),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(HmacSha256, OutputIs32Bytes)
{
    std::vector<uint8_t> key = {0x01};
    auto mac = SigV4Verifier::hmacSha256(key, "data");
    EXPECT_EQ(mac.size(), 32u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite: BuildStringToSign  (AWS test vector, no Drogon needed)
// ─────────────────────────────────────────────────────────────────────────────
TEST(BuildStringToSign, AwsGetVanilla)
{
    const std::string sts = SigV4Verifier::buildStringToSign(
        aws_tv::kTimestamp,
        aws_tv::kCredentialScope,
        aws_tv::kCanonicalRequestHash);

    EXPECT_EQ(sts, aws_tv::kStringToSign);
}

TEST(BuildStringToSign, AlgorithmLine)
{
    const std::string sts = SigV4Verifier::buildStringToSign(
        "20240101T000000Z", "20240101/us-east-1/s3/aws4_request", "aabbcc");
    EXPECT_EQ(sts.substr(0, 16), "AWS4-HMAC-SHA256");
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite: ComputeSignature  (AWS test vector, no Drogon needed)
// ─────────────────────────────────────────────────────────────────────────────
TEST(ComputeSignature, AwsGetVanilla)
{
    const std::string sig = SigV4Verifier::computeSignature(
        aws_tv::kSecretKey,
        aws_tv::kDate,
        aws_tv::kRegion,
        aws_tv::kService,
        aws_tv::kStringToSign);

    EXPECT_EQ(sig, aws_tv::kSignature);
}

TEST(ComputeSignature, OutputIs64HexChars)
{
    const std::string sig = SigV4Verifier::computeSignature(
        "secret", "20240101", "us-east-1", "s3", "dummy-string-to-sign");
    EXPECT_EQ(sig.size(), 64u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite: BuildCanonicalRequest  (requires Drogon HttpRequest)
// ─────────────────────────────────────────────────────────────────────────────

// Re-create the "get-vanilla" request and verify the canonical form matches
// the official test vector exactly.
TEST(BuildCanonicalRequest, AwsGetVanilla)
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/");
    req->addHeader("host",        "example.amazonaws.com");
    req->addHeader("x-amz-date", aws_tv::kTimestamp);

    SigV4Verifier verifier(aws_tv::kAccessKey, aws_tv::kSecretKey);

    const std::vector<std::string> signedHeaders = {"host", "x-amz-date"};
    const std::string cr =
        verifier.buildCanonicalRequest(req, signedHeaders, /*presigned=*/false);

    EXPECT_EQ(cr, aws_tv::kCanonicalRequest);
}

TEST(BuildCanonicalRequest, MethodAppears)
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setPath("/bucket/key");
    req->addHeader("host",        "localhost:9000");
    req->addHeader("x-amz-date", "20240101T000000Z");

    SigV4Verifier verifier("AKID", "SECRET");
    const std::string cr =
        verifier.buildCanonicalRequest(req, {"host", "x-amz-date"}, false);

    EXPECT_EQ(cr.substr(0, 4), "POST");
}

TEST(BuildCanonicalRequest, PresignedUsesUnsignedPayload)
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/b/k");
    req->addHeader("host", "localhost:9000");

    SigV4Verifier verifier("AKID", "SECRET");
    const std::string cr =
        verifier.buildCanonicalRequest(req, {"host"}, /*presigned=*/true);

    EXPECT_NE(cr.find("UNSIGNED-PAYLOAD"), std::string::npos);
}

TEST(BuildCanonicalRequest, QueryParamsSorted)
{
    // canonicalQueryStringFromRaw is public specifically to allow this test.
    // Drogon has no setQuery() setter, so we cannot inject a query string via
    // an HttpRequest object in unit tests — we test the logic directly instead.
    const std::string qs =
        SigV4Verifier::canonicalQueryStringFromRaw("z-param=1&a-param=2", false);

    // 'a-param' must come before 'z-param' (lexicographic sort)
    const auto posA = qs.find("a-param");
    const auto posZ = qs.find("z-param");
    EXPECT_NE(posA, std::string::npos);
    EXPECT_NE(posZ, std::string::npos);
    EXPECT_LT(posA, posZ);
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite: Verify  (full end-to-end, uses current timestamp)
// ─────────────────────────────────────────────────────────────────────────────

TEST(Verify, MissingAuthReturnsAccessDenied)
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/bucket");

    SigV4Verifier verifier("AKID", "SECRET");
    const auto result = verifier.verify(req);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.errorCode, "AccessDenied");
}

TEST(Verify, WrongAccessKey)
{
    std::string ts, date;
    currentUtcStamps(ts, date);

    auto req = makeRequest("/bucket/key", "localhost:9000", ts);
    req->addHeader("authorization",
        "AWS4-HMAC-SHA256 "
        "Credential=WRONGKEY/" + date + "/us-east-1/s3/aws4_request,"
        "SignedHeaders=host;x-amz-content-sha256;x-amz-date,"
        "Signature=0000000000000000000000000000000000000000000000000000000000000000");

    SigV4Verifier verifier("AKID", "SECRET");
    const auto result = verifier.verify(req);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.errorCode, "InvalidAccessKeyId");
}

TEST(Verify, MalformedAuthHeader)
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/");
    req->addHeader("authorization", "not-aws-sigv4 garbage");

    SigV4Verifier verifier("AKID", "SECRET");
    const auto result = verifier.verify(req);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.errorCode, "AccessDenied");
}

TEST(Verify, ExpiredTimestamp)
{
    // Build a request with a timestamp 20 minutes in the past (> 15-min window)
    auto pastTime = std::time(nullptr) - 20 * 60;
    std::tm t{};
    gmtime_r(&pastTime, &t);
    char tsBuf[17], dBuf[9];
    std::strftime(tsBuf, sizeof(tsBuf), "%Y%m%dT%H%M%SZ", &t);
    std::strftime(dBuf,  sizeof(dBuf),  "%Y%m%d",          &t);

    auto req = makeRequest("/bucket", "localhost:9000", tsBuf);
    req->addHeader("authorization",
        std::string("AWS4-HMAC-SHA256 ")
        + "Credential=AKID/" + dBuf + "/us-east-1/s3/aws4_request,"
        + "SignedHeaders=host;x-amz-content-sha256;x-amz-date,"
        + "Signature=0000000000000000000000000000000000000000000000000000000000000000");

    SigV4Verifier verifier("AKID", "SECRET");
    const auto result = verifier.verify(req);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.errorCode, "RequestTimeTooSkewed");
}

TEST(Verify, ValidSignature)
{
    const std::string accessKey = "TESTAKID";
    const std::string secretKey = "TESTSECRETKEY";
    const std::string region    = "us-east-1";
    const std::string service   = "s3";
    const std::string host      = "localhost:9000";
    const std::string path      = "/my-bucket/my-object";

    std::string ts, date;
    currentUtcStamps(ts, date);

    // Signed headers (sorted, lowercase)
    const std::vector<std::string> signedHeaders = {
        "host", "x-amz-content-sha256", "x-amz-date"};

    auto req = makeRequest(path, host, ts);

    // Compute canonical request hash and signature using the same verifier
    SigV4Verifier verifier(accessKey, secretKey);
    const std::string cr   = verifier.buildCanonicalRequest(req, signedHeaders, false);
    const std::string crHash = SigV4Verifier::sha256Hex(cr);
    const std::string credScope = date + "/" + region + "/" + service + "/aws4_request";
    const std::string sts  = SigV4Verifier::buildStringToSign(ts, credScope, crHash);
    const std::string sig  = SigV4Verifier::computeSignature(secretKey, date, region, service, sts);

    // Build Authorization header
    const std::string authHeader =
        "AWS4-HMAC-SHA256 "
        "Credential=" + accessKey + "/" + date + "/" + region + "/" + service + "/aws4_request,"
        "SignedHeaders=host;x-amz-content-sha256;x-amz-date,"
        "Signature=" + sig;

    req->addHeader("authorization", authHeader);

    const auto result = verifier.verify(req);
    EXPECT_TRUE(result.valid) << "errorCode=" << result.errorCode
                              << " errorMsg=" << result.errorMessage;
}

TEST(Verify, WrongSignatureValue)
{
    const std::string accessKey = "AKID";
    const std::string secretKey = "SECRET";

    std::string ts, date;
    currentUtcStamps(ts, date);

    auto req = makeRequest("/bucket", "localhost:9000", ts);
    req->addHeader("authorization",
        "AWS4-HMAC-SHA256 "
        "Credential=AKID/" + date + "/us-east-1/s3/aws4_request,"
        "SignedHeaders=host;x-amz-content-sha256;x-amz-date,"
        "Signature=deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef");

    SigV4Verifier verifier(accessKey, secretKey);
    const auto result = verifier.verify(req);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.errorCode, "SignatureDoesNotMatch");
}
