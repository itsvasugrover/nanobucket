add_executable(nanobucket
    main.cpp
    # XML
    xml/S3XmlBuilder.cpp
    # Auth
    auth/SigV4Verifier.cpp
    auth/AwsSigV4Filter.cpp
    # Metadata
    meta/SqliteMetaStore.cpp
    # Storage
    storage/LocalFsStorageEngine.cpp
    # Controllers
    api/BucketController.cpp
    api/ObjectController.cpp
)

target_include_directories(nanobucket PRIVATE
    ${PROJECT_SOURCE_DIR}/include   # nanobucket/common/Types.h etc.
    ${PROJECT_SOURCE_DIR}/src       # xml/, auth/, meta/, storage/ cross-includes
)

target_link_libraries(nanobucket PRIVATE
    Drogon::Drogon
    spdlog::spdlog
    OpenSSL::SSL
    OpenSSL::Crypto
    pugixml::pugixml
    SQLite::SQLite3
)
