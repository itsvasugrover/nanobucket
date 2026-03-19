# AWS SigV4 Authentication

## Overview

Every request to S3 must carry a valid AWS Signature Version 4 credential. The filter runs before any controller — if verification fails, the request never reaches storage. Getting SigV4 right is the single most important compatibility requirement: a single byte difference in the canonical request causes a signature mismatch and breaks the entire AWS SDK or CLI.

---

## File Map

```
src/auth/
├── SigV4Verifier.h / .cpp    — implements ISigV4Verifier, contains all crypto math
└── AwsSigV4Filter.h / .cpp   — Drogon HttpFilter, calls verifier, returns XML errors
```

---

## How SigV4 Works

The algorithm has four sequential steps. Every step feeds into the next.

### Step 1 — Build the Canonical Request

A deterministic string representation of the request, used as input to the hash.

```
<HTTPMethod>\n
<CanonicalURI>\n
<CanonicalQueryString>\n
<CanonicalHeaders>\n
<SignedHeaders>\n
<HexEncode(SHA256(RequestPayload))>
```

**`CanonicalURI`** — URI-encode every path segment (not the slashes). For `PUT /my-bucket/folder/file.txt`, the canonical URI is `/my-bucket/folder/file.txt`.

**`CanonicalQueryString`** — Sort query parameters by name (then by value if names collide), URI-encode names and values, join with `&`. For `?list-type=2&prefix=photos/` the result is `list-type=2&prefix=photos%2F`.

**`CanonicalHeaders`** — Lowercase all header names. Sort by name. Each entry is `lowercasename:trimmedvalue\n`. Must include at minimum `host` and `x-amz-date`.

**`SignedHeaders`** — Semicolon-separated sorted list of the header names included in CanonicalHeaders, e.g. `host;x-amz-content-sha256;x-amz-date`.

**Payload hash** — `HexEncode(SHA256(body))`. If the client sends `x-amz-content-sha256: UNSIGNED-PAYLOAD` (presigned URLs, streaming), use the literal string `UNSIGNED-PAYLOAD`.

### Step 2 — Build the String to Sign

```
AWS4-HMAC-SHA256\n
<x-amz-date value>\n
<CredentialScope>\n
<HexEncode(SHA256(CanonicalRequest))>
```

**`CredentialScope`** — `<date>/<region>/<service>/aws4_request`, e.g. `20240115/us-east-1/s3/aws4_request`.

**`x-amz-date`** — ISO 8601 compact form: `20240115T103000Z`.

### Step 3 — Derive the Signing Key

A chain of four HMAC-SHA256 operations using the secret key. This is the key insight of SigV4: the signing key is derived fresh for each date + region + service combination, which limits the blast radius of a leaked key.

```
kDate    = HMAC-SHA256("AWS4" + SecretKey, Date)
kRegion  = HMAC-SHA256(kDate,   Region)
kService = HMAC-SHA256(kRegion, Service)
kSigning = HMAC-SHA256(kService, "aws4_request")
```

All HMAC operations use the **raw bytes** of the previous key as the key input. `Date` here is just the date portion: `20240115`.

### Step 4 — Calculate and Compare the Signature

```
Signature = HexEncode(HMAC-SHA256(kSigning, StringToSign))
```

Extract the `Signature` from the client's `Authorization` header and do a constant-time comparison.

---

## OpenSSL Implementation

```cpp
// HMAC-SHA256 with raw binary key
static std::vector<uint8_t> hmacSha256(std::span<const uint8_t> key,
                                        std::string_view          data) {
    unsigned int len = EVP_MAX_MD_SIZE;
    std::vector<uint8_t> result(len);
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const uint8_t*>(data.data()), data.size(),
         result.data(), &len);
    result.resize(len);
    return result;
}

// SHA256 → hex string
static std::string sha256Hex(std::string_view data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const uint8_t*>(data.data()), data.size(), hash);
    // hex encode ...
}

// Signing key derivation
static std::vector<uint8_t> deriveSigningKey(const std::string& secretKey,
                                              const std::string& date,
                                              const std::string& region,
                                              const std::string& service) {
    auto kDate    = hmacSha256(toBytes("AWS4" + secretKey), date);
    auto kRegion  = hmacSha256(kDate,    region);
    auto kService = hmacSha256(kRegion,  service);
    auto kSigning = hmacSha256(kService, "aws4_request");
    return kSigning;
}
```

---

## Parsing the `Authorization` Header

```
Authorization: AWS4-HMAC-SHA256
    Credential=AKIAIOSFODNN7EXAMPLE/20240115/us-east-1/s3/aws4_request,
    SignedHeaders=host;x-amz-content-sha256;x-amz-date,
    Signature=fe5f80f77d5fa3beca038a248ff027d0445342fe2855ddc963176630326f1024
```

Extract:
- `AccessKeyId` — first component of `Credential` before the first `/`
- `CredentialScope` — remainder of `Credential` after `AccessKeyId/`
- `SignedHeaders` — semicolon-separated list
- `Signature` — hex string to compare against

---

## Presigned URL (Query-String Auth)

Used by `s3cmd signurl` and `aws s3 presign`. Parameters arrive as query parameters instead of headers:

| Query param | Equivalent header |
|-------------|------------------|
| `X-Amz-Algorithm` | (always `AWS4-HMAC-SHA256`) |
| `X-Amz-Credential` | Credential in Authorization |
| `X-Amz-Date` | `x-amz-date` |
| `X-Amz-Expires` | seconds until expiry |
| `X-Amz-SignedHeaders` | SignedHeaders in Authorization |
| `X-Amz-Signature` | Signature in Authorization |

Additional checks for presigned requests:
1. Payload hash is always the literal string `UNSIGNED-PAYLOAD`
2. Parse `X-Amz-Date` and add `X-Amz-Expires` seconds — if the result is before `now()`, return `RequestExpired`
3. `X-Amz-SignedHeaders` must include `host` at minimum

---

## The Drogon Filter (`AwsSigV4Filter`)

```cpp
class AwsSigV4Filter : public drogon::HttpFilter<AwsSigV4Filter> {
public:
    void doFilter(const drogon::HttpRequestPtr& req,
                  drogon::FilterCallback&&      fcb,
                  drogon::FilterChainCallback&& fccb) override {

        auto result = verifier_->verify(req);

        if (result.valid) {
            fccb();  // pass to controller
            return;
        }

        auto res = drogon::HttpResponse::newHttpResponse();
        res->setStatusCode(drogon::k403Forbidden);
        res->setContentTypeCode(drogon::CT_APPLICATION_XML);
        res->setBody(S3XmlBuilder::error(result.errorCode, result.errorMessage));
        fcb(res);
    }

private:
    std::shared_ptr<ISigV4Verifier> verifier_;
};
```

The filter is registered globally in `main.cpp` so it intercepts every route.

---

## Error Responses

| Condition | S3 Code | HTTP |
|-----------|---------|------|
| Missing Authorization header | `AccessDenied` | 403 |
| Unknown access key | `InvalidAccessKeyId` | 403 |
| Signature does not match | `SignatureDoesNotMatch` | 403 |
| Timestamp outside ±15 min window | `RequestTimeTooSkewed` | 403 |
| Presigned URL has expired | `AccessDenied` | 403 |
| Malformed Authorization header | `AuthorizationHeaderMalformed` | 400 |

---

## AWS SigV4 Test Vectors

AWS publishes an official test suite at:
`https://docs.aws.amazon.com/general/latest/gr/sigv4-test-suite.html`

The unit tests for `SigV4Verifier` run against these vectors to verify correctness before any real client is involved. See [testing.md](testing.md) for details.

---

## Common Pitfalls

| Pitfall | Effect | Fix |
|---------|--------|-----|
| URI encoding path segments including `/` | Signature mismatch | Only encode characters inside each segment, never the slashes |
| Not sorting query params | Signature mismatch | Sort by name, then value; use byte-order sort not locale |
| Trimming header values incorrectly | Signature mismatch | Collapse multiple spaces to one, strip leading/trailing |
| Using string HMAC key on first step | Wrong signing key | Prefix with `"AWS4"` bytes, not the string `"AWS4"` alone |
| Comparing signatures with `==` | Timing attack | Use `CRYPTO_memcmp()` from OpenSSL for constant-time compare |
