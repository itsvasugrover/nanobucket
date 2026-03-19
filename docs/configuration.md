# Configuration & Server Entry Point

## Overview

`config/config.json` is the single source of truth for all runtime parameters. `main.cpp` loads it at startup and injects dependencies into all components before handing control to Drogon.

---

## File Map

```
config/
└── config.json          — runtime configuration

src/
└── main.cpp             — entry point: load config, wire deps, start Drogon
```

---

## `config.json` Schema

```json
{
  "server": {
    "host":        "0.0.0.0",
    "port":        9000,
    "threads":     0,
    "logLevel":    "info",
    "logFile":     ""
  },
  "auth": {
    "accessKey":   "nanobucketadmin",
    "secretKey":   "nanobucketadmin"
  },
  "storage": {
    "dataRoot":    "./data"
  },
  "meta": {
    "dbPath":      "./data/meta.db"
  }
}
```

### Field Reference

| Key | Type | Default | Notes |
|-----|------|---------|-------|
| `server.host` | string | `"0.0.0.0"` | Bind address |
| `server.port` | int | `9000` | Overridden by `NANOBUCKET_PORT` env var |
| `server.threads` | int | `0` | `0` = `hardware_concurrency()` |
| `server.logLevel` | string | `"info"` | `debug`, `info`, `warn`, `error` |
| `server.logFile` | string | `""` | Empty = console only (stderr) |
| `auth.accessKey` | string | `"nanobucketadmin"` | Access key ID |
| `auth.secretKey` | string | `"nanobucketadmin"` | Secret access key |
| `storage.dataRoot` | string | `"./data"` | Root directory for object files |
| `meta.dbPath` | string | `"./data/meta.db"` | SQLite database file path |

---

## `main.cpp` Startup Sequence

```
1. Install SIGINT / SIGTERM handlers
2. Load config.json
3. Apply NANOBUCKET_PORT env var override (if set)
4. Init SqliteMetaStore (open db, run migrations)
5. Init LocalFsStorageEngine (create data root dirs)
6. Construct SigV4Verifier with (accessKey, secretKey)
7. Register AwsSigV4Filter globally
8. Register BucketController, ObjectController, MultipartController
9. Configure Drogon (host, port, threads, log level, log file)
10. drogon::app().run()
11. On return: log shutdown complete
```

### Dependency Wiring

All components are injected via `std::shared_ptr` at startup. Controllers receive their dependencies through Drogon's plugin system or a simple singleton accessor — no global variables.

```cpp
// Construct components
auto metaStore    = std::make_shared<SqliteMetaStore>(config.meta.dbPath);
auto storage      = std::make_shared<LocalFsStorageEngine>(config.storage.dataRoot);
auto verifier     = std::make_shared<SigV4Verifier>(config.auth.accessKey,
                                                     config.auth.secretKey);

// Init
metaStore->init();
storage->init();

// Register filter (global — applies to all routes)
drogon::app().registerFilter(std::make_shared<AwsSigV4Filter>(verifier));

// Register controllers (inject deps)
drogon::app().registerController(
    std::make_shared<BucketController>(storage, metaStore));
drogon::app().registerController(
    std::make_shared<ObjectController>(storage, metaStore));
drogon::app().registerController(
    std::make_shared<MultipartController>(storage, metaStore));
```

---

## Log Level Mapping

| `config.json` value | Trantor level | spdlog level |
|--------------------|--------------|-------------|
| `"debug"` | `kDebug` | `debug` |
| `"info"` | `kInfo` | `info` |
| `"warn"` | `kWarn` | `warn` |
| `"error"` | `kError` | `err` |

Both trantor (Drogon's internal logger) and spdlog are configured to the same level. During development `"debug"` is recommended — it shows every incoming request, matched route, and response code.

---

## File Logging

When `server.logFile` is non-empty, Drogon writes trantor logs to that file path. The file is rotated by Drogon automatically based on size. Console logging (stderr) is suppressed when a log file is configured.

```cpp
if (!config.server.logFile.empty()) {
    drogon::app().setLogPath(config.server.logFile);
} // else: default stderr
```

---

## Environment Variable Overrides

Environment variables take precedence over `config.json` values:

| Env var | Config key | Notes |
|---------|-----------|-------|
| `NANOBUCKET_PORT` | `server.port` | Useful for container deployments |

---

## Graceful Shutdown

```cpp
static void onSignal(int sig) {
    spdlog::warn("caught signal {}, shutting down gracefully...", sig);
    drogon::app().quit();
}

std::signal(SIGINT,  onSignal);
std::signal(SIGTERM, onSignal);
```

`drogon::app().quit()` drains all in-flight requests and closes the listening socket before returning from `app().run()`. The `spdlog::info("nanobucket stopped")` line after `run()` confirms the shutdown completed cleanly.
