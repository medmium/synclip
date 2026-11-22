cmake_minimum_required(VERSION 3.24)
project(SynClip VERSION 0.3.0 LANGUAGES C CXX)

if(NOT WIN32 AND NOT ANDROID)
    message(FATAL_ERROR "SynClip builds for Windows or Android")
endif()

include(FetchContent)
set(ENABLE_PROGRAMS OFF CACHE BOOL "Build Mbed TLS tools" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "Build Mbed TLS tests" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "Build shared Mbed TLS" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "Build static Mbed TLS" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "Skip Mbed TLS installation" FORCE)

if(ANDROID)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

FetchContent_Declare(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.6/mbedtls-3.6.6.tar.bz2
    URL_HASH SHA256=8fb65fae8dcae5840f793c0a334860a411f884cc537ea290ce1c52bb64ca007a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(mbedtls)
target_include_directories(mbedcrypto PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/mbedtls")
target_compile_definitions(mbedcrypto PUBLIC MBEDTLS_USER_CONFIG_FILE="synclip_mbedtls_config.h")

find_package(Threads REQUIRED)
add_library(synclip_transport STATIC src/transport.cpp src/hub.cpp)
target_compile_features(synclip_transport PUBLIC cxx_std_17)
target_include_directories(synclip_transport PUBLIC include)
target_link_libraries(synclip_transport PRIVATE MbedTLS::mbedtls Threads::Threads)

if(ANDROID)
    add_subdirectory(android)
    add_library(synclip_pair_jni SHARED src/pair_jni.cpp)
    target_link_libraries(synclip_pair_jni PRIVATE synclip_transport)
else()
    FetchContent_Declare(qrcodegen
        URL https://codeload.github.com/nayuki/QR-Code-generator/tar.gz/refs/tags/v1.8.0
        URL_HASH SHA256=2ec0a4d33d6f521c942eeaf473d42d5fe139abcfa57d2beffe10c5cf7d34ae60
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(qrcodegen)
    add_library(synclip_qr STATIC "${qrcodegen_SOURCE_DIR}/cpp/qrcodegen.cpp")
    target_include_directories(synclip_qr PUBLIC "${qrcodegen_SOURCE_DIR}/cpp")
    target_compile_features(synclip_qr PUBLIC cxx_std_17)
    file(STRINGS "${qrcodegen_SOURCE_DIR}/cpp/qrcodegen.hpp" qr_license_lines LIMIT_COUNT 22)
    string(JOIN "\n" qr_license ${qr_license_lines})
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/qrcodegen.txt" "${qr_license}\n")

    add_executable(SynClip WIN32 windows/main.cpp windows/pairing_ui.cpp)
    target_compile_definitions(SynClip PRIVATE _WIN32_WINNT=0x0A00 UNICODE _UNICODE)
    target_compile_definitions(synclip_transport PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(synclip_transport PRIVATE ws2_32)
    target_link_libraries(SynClip PRIVATE synclip_transport synclip_qr user32 shell32 gdi32 iphlpapi ws2_32)
    if(MINGW)
        target_link_options(SynClip PRIVATE -municode -static)
    elseif(MSVC)
        target_compile_options(SynClip PRIVATE /utf-8)
        target_compile_options(synclip_transport PRIVATE /utf-8)
    endif()

    install(TARGETS SynClip RUNTIME DESTINATION . COMPONENT SynClipRuntime)
    install(FILES "${mbedtls_SOURCE_DIR}/LICENSE" DESTINATION licenses
            RENAME mbedtls.txt COMPONENT SynClipRuntime)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/qrcodegen.txt" DESTINATION licenses COMPONENT SynClipRuntime)
endif()
