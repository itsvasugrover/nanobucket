# NanoBucket

**A lightweight, self-hosted S3-compatible object storage server written in C++.**

Drop it into your local dev stack, home lab, or CI pipeline and get a real S3 endpoint — no AWS account, no Docker image with 500 MB of JVM overhead, no configuration files to wrestle with.

```bash
NANOBUCKET_ACCESS_KEY=mykey NANOBUCKET_SECRET_KEY=mysecret ./nanobucket
# → Listening on http://0.0.0.0:9000
```

Point any S3 client at `localhost:9000` and everything just works.

---

## Why NanoBucket?

Most self-hosted S3-compatible servers are either **too heavy** (MinIO ships a full distributed system), **too limited** (fake-s3 / localstack-lite miss auth or multipart), or **not something you can actually read and learn from**.

NanoBucket is none of those things:

- **Single binary** — one executable, no runtime dependencies, no JVM, no Python interpreter
- **Zero config files** — everything is an environment variable with a sane default
- **Full SigV4 auth** — not a stub; real HMAC-SHA256 verification, same as AWS
- **All 16 core S3 operations** — including multipart upload, bulk delete, CopyObject
- **Verified against real clients** — 47 end-to-end tests using `s3cmd`; passes every one
- **~2,500 lines of readable C++17** — no macro soup, no generated code, no black boxes
- **92 unit tests** across three isolated suites

---

## Supported Operations

| Operation | HTTP | Status |
|-----------|------|--------|
| ListBuckets | `GET /` | ✅ |
| CreateBucket | `PUT /{bucket}` | ✅ |
| DeleteBucket | `DELETE /{bucket}` | ✅ |
| HeadBucket | `HEAD /{bucket}` | ✅ |
| ListObjectsV2 | `GET /{bucket}?list-type=2` | ✅ |
| DeleteObjects (bulk) | `POST /{bucket}?delete` | ✅ |
| PutObject | `PUT /{bucket}/{key}` | ✅ |
| GetObject | `GET /{bucket}/{key}` | ✅ |
| HeadObject | `HEAD /{bucket}/{key}` | ✅ |
| DeleteObject | `DELETE /{bucket}/{key}` | ✅ |
| CopyObject | `PUT /{bucket}/{key}` + `x-amz-copy-source` | ✅ |
| CreateMultipartUpload | `POST /{bucket}/{key}?uploads` | ✅ |
| UploadPart | `PUT /{bucket}/{key}?partNumber=N&uploadId=X` | ✅ |
| CompleteMultipartUpload | `POST /{bucket}/{key}?uploadId=X` | ✅ |
| AbortMultipartUpload | `DELETE /{bucket}/{key}?uploadId=X` | ✅ |
| ListParts | `GET /{bucket}/{key}?uploadId=X` | ✅ |

ACL endpoints return stubs (FULL_CONTROL for all authenticated requests). CORS, Policy, and Lifecycle return proper 404 error codes so SDK clients don't choke.

---

## Quick Start

### Prerequisites

- CMake ≥ 3.15
- Conan v2
- A C++17 compiler (GCC 10+, Clang 12+)

### Build

```bash
git clone https://github.com/itsvasugrover/nanobucket.git
cd nanobucket

# Install dependencies and configure
bash scripts/setup.sh

# Compile
bash scripts/build.sh
```

### Run

```bash
bash scripts/start.sh
```

Or with custom settings:

```bash
NANOBUCKET_PORT=9000 \
NANOBUCKET_DATA_ROOT=./data \
NANOBUCKET_ACCESS_KEY=myaccesskey \
NANOBUCKET_SECRET_KEY=mysecretkey \
./build/Release/src/nanobucket
```

### Test (unit)

```bash
bash scripts/test.sh
```

### Test (end-to-end with s3cmd)

```bash
# Server must be running first
bash scripts/api-test.sh
```

---

## Configuration

Everything is an environment variable. No config files needed.

| Variable | Default | Description |
|----------|---------|-------------|
| `NANOBUCKET_PORT` | `9000` | TCP port to listen on |
| `NANOBUCKET_DATA_ROOT` | `./data` | Root directory for stored objects |
| `NANOBUCKET_DB_PATH` | `<dataRoot>/meta.db` | SQLite database path |
| `NANOBUCKET_ACCESS_KEY` | `s3liteadmin` | AWS access key |
| `NANOBUCKET_SECRET_KEY` | `s3liteadmin` | AWS secret key |

---

## Client Compatibility

### s3cmd

```ini
# ~/.s3cfg
[default]
access_key = s3liteadmin
secret_key = s3liteadmin
host_base = localhost:9000
host_bucket = localhost:9000/%(bucket)s
use_https = False
signature_v2 = False
```

```bash
s3cmd mb s3://my-bucket
s3cmd put file.txt s3://my-bucket/file.txt
s3cmd get s3://my-bucket/file.txt ./file-copy.txt
s3cmd ls s3://my-bucket/
```

### AWS SDK / boto3

```python
import boto3

s3 = boto3.client(
    "s3",
    endpoint_url="http://localhost:9000",
    aws_access_key_id="s3liteadmin",
    aws_secret_access_key="s3liteadmin",
    region_name="us-east-1",
)

s3.create_bucket(Bucket="my-bucket")
s3.put_object(Bucket="my-bucket", Key="hello.txt", Body=b"Hello, world!")
```

### AWS CLI v2

```bash
aws --endpoint-url http://localhost:9000 \
    --profile nanobucket \
    s3 cp file.txt s3://my-bucket/file.txt
```

---

## Architecture

```
src/
├── main.cpp                        — entry point, dependency wiring, Drogon bootstrap
├── api/
│   ├── BucketController.h/.cpp     — bucket ops + ListObjectsV2 + DeleteObjects
│   ├── ObjectController.h/.cpp     — object ops + full multipart lifecycle
│   └── ApiHelpers.h                — shared header helpers
├── auth/
│   ├── SigV4Verifier.h/.cpp        — AWS SigV4 HMAC-SHA256 verification
│   └── AwsSigV4Filter.h/.cpp       — Drogon HttpFilter, applied to every route
├── meta/
│   ├── IMetaStore.h                — interface
│   └── SqliteMetaStore.h/.cpp      — SQLite + WAL, 14 prepared statements
├── storage/
│   ├── IStorageEngine.h            — interface
│   └── LocalFsStorageEngine.h/.cpp — filesystem backend, atomic writes
└── xml/
    └── S3XmlBuilder.h/.cpp         — pugixml-based S3 response builder
```

Storage and metadata are decoupled behind interfaces — swapping the SQLite backend for Postgres or the filesystem for object-level encryption requires only a new implementation of `IMetaStore` or `IStorageEngine`.

---

## Tech Stack

| Component | Library |
|-----------|---------|
| HTTP server | [Drogon](https://github.com/drogonframework/drogon) 1.9 |
| XML | [pugixml](https://pugixml.org/) 1.15 |
| Auth crypto | OpenSSL 3.6 (HMAC-SHA256, MD5) |
| Metadata | SQLite 3.51 (WAL mode) |
| Logging | [spdlog](https://github.com/gabime/spdlog) 1.17 |
| Build | CMake 3.15 + Conan v2 |
| Tests | GoogleTest 1.14 |

---

## Known Limitations

| Area | Limitation |
|------|-----------|
| TLS | HTTP only. Run behind nginx or Caddy for HTTPS. |
| Auth | Single static credential pair. No IAM, no multi-user. |
| Versioning | Not implemented. Last write wins. |
| ACL / Policy | Stubs only. All authenticated requests are treated as owner. |
| Concurrency | SQLite single-mutex connection. Fine for personal use; not horizontally scalable. |
| AWS CLI | Untested. s3cmd is the verified client. |

---

## Roadmap

- [ ] `aws s3` CLI compatibility testing
- [ ] Presigned URL end-to-end test
- [ ] `s3cmd sync` verification
- [ ] Multiple credential pairs
- [ ] TLS via Drogon config (or nginx reverse-proxy guide)
- [ ] Docker image
- [ ] Metrics / health endpoint (`GET /health`)
- [ ] Streaming write path (avoid full body buffering for large objects)

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for full detail on what's done and what's next.

---

## Contributing

Contributions are welcome — bug reports, feature requests, compatibility fixes, and documentation improvements all count.

- [Bug report](.github/ISSUE_TEMPLATE/bug_report.md)
- [Feature request](.github/ISSUE_TEMPLATE/feature_request.md)
- [Contributing guide](CONTRIBUTING.md)

If you test against a client that isn't s3cmd (aws CLI, boto3, s3fs, rclone, etc.) and find a gap, please open an issue with the exact command and error. Compatibility fixes are the highest-priority contributions right now.

---

## License

[MIT](LICENSE) — © 2025 Vasu Grover (NotCoderGuy)
