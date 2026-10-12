# Tapa: a monophonic drum synth whose interface is a web page.
#
#   cmake -B build && cmake --build build     CLAP, VST3, AU (macOS), standalone
#   More formats, each with one extra flag (see README.md): WCLAP, the iOS app
#   (with its AUv3).
cmake_minimum_required(VERSION 3.28)

# macOS plugins here are universal (Apple silicon and Intel).
if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0")
endif()

project(Tapa VERSION 0.2.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJC OBJCXX)
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # build/compile_commands.json, where clangd finds the downloaded headers
if(MSVC) # clap-wrapper and the VST3 SDK use the DLL runtime (/MD)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux") # VST3 modules link static libraries
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# char-clap-template's libraries (libs/), at a release: read its CHANGELOG.md
# before changing GIT_TAG. To build against a local checkout of it instead:
#   cmake -B build -DFETCHCONTENT_SOURCE_DIR_CLAP_TEMPLATE=<checkout>
include(FetchContent)
FetchContent_Declare(clap_template
    GIT_REPOSITORY https://github.com/charCulbert/char-clap-template.git
    GIT_TAG v0.1.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR none) # only libs/: the template's own CMakeLists isn't for plugins
FetchContent_MakeAvailable(clap_template)
set(LIBS "${clap_template_SOURCE_DIR}/libs")
include(${LIBS}/core/core.cmake)
include(${LIBS}/webview/webview.cmake)

# chardsp is a git submodule; its Oversampler needs its own signalsmith-dsp submodule.
set(CHARDSP "${CMAKE_CURRENT_SOURCE_DIR}/libs/chardsp")
if(NOT EXISTS "${CHARDSP}/external/signalsmith-dsp/include/signalsmith-dsp/rates.h")
    message(FATAL_ERROR "libs/chardsp is empty: run `git submodule update --init --recursive`")
endif()

add_library(Tapa_impl STATIC plugin.cpp)
target_include_directories(Tapa_impl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CHARDSP}")
target_include_directories(Tapa_impl SYSTEM PUBLIC "${CHARDSP}/external/signalsmith-dsp/include")
core_add_to(Tapa_impl)
webview_add_to(Tapa_impl)

# resources/ (the page in page/) ships as ordinary files inside each plugin.
webview_resources(resources Tapa resources)

set(identity
    OUTPUT_NAME "Tapa"
    BUNDLE_IDENTIFIER "com.charlieculbert.tapa"
    BUNDLE_VERSION "${PROJECT_VERSION}")

# iOS: an AUv3 extension inside an app (configure with -G Xcode -DCMAKE_SYSTEM_NAME=iOS).
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(Tapa_auv3 entry.cpp)
    target_link_libraries(Tapa_auv3 PRIVATE Tapa_impl)
    target_add_auv3_wrapper(TARGET Tapa_auv3 ${identity}
        MANUFACTURER_NAME "Charlie Culbert" MANUFACTURER_CODE "ChAr"
        SUBTYPE_CODE "Tapa" INSTRUMENT_TYPE "aumu")
    add_executable(Tapa_ios entry.cpp)
    target_link_libraries(Tapa_ios PRIVATE Tapa_impl)
    target_add_auv3_standalone_ios_wrapper(TARGET Tapa_ios
        OUTPUT_NAME "Tapa" BUNDLE_IDENTIFIER "com.charlieculbert.tapa.ios"
        BUNDLE_VERSION "${PROJECT_VERSION}" AUV3_TARGET Tapa_auv3
        AU_TYPE "aumu" AU_SUBTYPE "Tapa" AU_MANUFACTURER "ChAr")
    return()
endif()

set(formats CLAP VST3)
if(APPLE)
    list(APPEND formats AUV2)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    set(formats WCLAP)
endif()

make_clapfirst_plugins(
    TARGET_NAME Tapa
    IMPL_TARGET Tapa_impl
    ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/entry.cpp"
    ${identity}
    AUV2_MANUFACTURER_NAME "Charlie Culbert"
    AUV2_MANUFACTURER_CODE "ChAr"
    AUV2_SUBTYPE_CODE "Tapa"
    AUV2_INSTRUMENT_TYPE "aumu"
    PLUGIN_FORMATS ${formats}
    WINDOWS_FOLDER_VST3 TRUE # a folder, so its resources can travel inside it
    STANDALONE_CONFIGURATIONS standalone "Tapa" "com.charlieculbert.tapa"
    COPY_AFTER_BUILD FALSE)

core_ship_resources(Tapa "${resources}" DEPENDS Tapa_resources)

enable_testing()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
    add_executable(tapa-tests tests/TapaTests.cpp entry.cpp)
    target_link_libraries(tapa-tests PRIVATE Tapa_impl)
    add_test(NAME tapa-tests COMMAND tapa-tests)
endif()
