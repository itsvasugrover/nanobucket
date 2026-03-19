#include <drogon/drogon.h>
#include <spdlog/spdlog.h>

#include "nanobucket/auth/ISigV4Verifier.h"
#include "nanobucket/meta/IMetaStore.h"
#include "nanobucket/storage/IStorageEngine.h"

#include "xml/S3XmlBuilder.h"
#include "auth/SigV4Verifier.h"
#include "auth/AwsSigV4Filter.h"
#include "meta/SqliteMetaStore.h"
#include "storage/LocalFsStorageEngine.h"
#include "api/BucketController.h"
#include "api/ObjectController.h"

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <thread>

static void onSignal(int sig) {
    spdlog::warn("caught signal {}, shutting down gracefully...", sig);
    drogon::app().quit();
}

// Read a required environment variable; return defaultVal if unset.
static std::string getEnvOr(const char* name, std::string defaultVal) {
    const char* v = std::getenv(name);
    return v ? v : std::move(defaultVal);
}

int main() {
    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    // ── Configuration (env-var driven) ────────────────────────────────────────
    const uint16_t    port      = static_cast<uint16_t>(
                                    std::atoi(getEnvOr("NANOBUCKET_PORT",      "9000").c_str()));
    const std::string dataRoot  = getEnvOr("NANOBUCKET_DATA_ROOT",  "./data");
    const std::string dbPath    = getEnvOr("NANOBUCKET_DB_PATH",    dataRoot + "/meta.db");
    const std::string accessKey = getEnvOr("NANOBUCKET_ACCESS_KEY", "nanobucketadmin");
    const std::string secretKey = getEnvOr("NANOBUCKET_SECRET_KEY", "nanobucketadmin");

    spdlog::info("nanobucket starting on port {}", port);
    spdlog::info("  data root : {}", dataRoot);
    spdlog::info("  db path   : {}", dbPath);
    spdlog::info("  access key: {}", accessKey);

    // ── Storage engine ────────────────────────────────────────────────────────
    auto storage = std::make_shared<nanobucket::LocalFsStorageEngine>(dataRoot);
    if (storage->init() != nanobucket::S3Error::None) {
        spdlog::error("Failed to initialise storage engine at {}", dataRoot);
        return 1;
    }

    // ── Metadata store ────────────────────────────────────────────────────────
    auto meta = std::make_shared<nanobucket::SqliteMetaStore>(dbPath);
    if (meta->init() != nanobucket::S3Error::None) {
        spdlog::error("Failed to initialise metadata store at {}", dbPath);
        return 1;
    }

    // ── Auth filter ───────────────────────────────────────────────────────────
    nanobucket::AwsSigV4Filter::setVerifier(
        std::make_shared<nanobucket::SigV4Verifier>(accessKey, secretKey));

    // ── Controller dependencies ───────────────────────────────────────────────
    nanobucket::BucketController::setDeps(meta, storage);
    nanobucket::ObjectController::setDeps(meta, storage);

    // ── Drogon application ────────────────────────────────────────────────────
    drogon::app()
        .setLogLevel(trantor::Logger::kInfo)
        .addListener("0.0.0.0", port)
        .setThreadNum(std::thread::hardware_concurrency())
        .setClientMaxBodySize(256UL * 1024 * 1024)   // 256 MB per part
        .setClientMaxMemoryBodySize(256UL * 1024 * 1024)
        .run();

    spdlog::info("nanobucket stopped");
    return 0;
}
