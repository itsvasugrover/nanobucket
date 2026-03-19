include(GoogleTest)

# ── Helper: shared include paths used by all unit test targets ────────────────
# Tests compile the production .cpp files they exercise directly so there is no
# dependency on the nanobucket executable target.
set(NANOBUCKET_INCLUDES
    ${PROJECT_SOURCE_DIR}/include   # nanobucket/common/Types.h etc.
    ${PROJECT_SOURCE_DIR}/src       # xml/S3XmlBuilder.h, auth/..., etc.
)

# ── test_xml_builder ──────────────────────────────────────────────────────────
add_executable(test_xml_builder
    test_xml_builder.cpp
    ${PROJECT_SOURCE_DIR}/src/xml/S3XmlBuilder.cpp
)

target_include_directories(test_xml_builder PRIVATE ${NANOBUCKET_INCLUDES})

target_link_libraries(test_xml_builder PRIVATE
    GTest::gtest
    GTest::gtest_main
    pugixml::pugixml
)

gtest_discover_tests(test_xml_builder)

# ── test_sigv4 ────────────────────────────────────────────────────────────────
add_executable(test_sigv4
    test_sigv4.cpp
    ${PROJECT_SOURCE_DIR}/src/auth/SigV4Verifier.cpp
)

target_include_directories(test_sigv4 PRIVATE ${NANOBUCKET_INCLUDES})

target_link_libraries(test_sigv4 PRIVATE
    GTest::gtest
    GTest::gtest_main
    Drogon::Drogon
    OpenSSL::SSL
    OpenSSL::Crypto
)

gtest_discover_tests(test_sigv4)

# ── test_meta_store ───────────────────────────────────────────────────────────
add_executable(test_meta_store
    test_meta_store.cpp
    ${PROJECT_SOURCE_DIR}/src/meta/SqliteMetaStore.cpp
)

target_include_directories(test_meta_store PRIVATE ${NANOBUCKET_INCLUDES})

target_link_libraries(test_meta_store PRIVATE
    GTest::gtest
    GTest::gtest_main
    SQLite::SQLite3
    OpenSSL::SSL
    OpenSSL::Crypto
    spdlog::spdlog
)

gtest_discover_tests(test_meta_store)

# ── test_storage_engine ───────────────────────────────────────────────────────
add_executable(test_storage_engine
    test_storage_engine.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/LocalFsStorageEngine.cpp
)

target_include_directories(test_storage_engine PRIVATE ${NANOBUCKET_INCLUDES})

target_link_libraries(test_storage_engine PRIVATE
    GTest::gtest
    GTest::gtest_main
    OpenSSL::SSL
    OpenSSL::Crypto
    spdlog::spdlog
)

gtest_discover_tests(test_storage_engine)
