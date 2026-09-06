cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0 CACHE STRING "Minimum iOS version")
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "Minimum macOS version")
    endif()
endif()

project(tapa VERSION 0.1.0 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^wasm")
    set(ANY_WASM_TOOLCHAIN TRUE)
endif()

# Reuse local checkouts; override these cache paths for another workspace.
set(external "${CMAKE_CURRENT_SOURCE_DIR}/external")
set(CLAP_SDK_ROOT "${external}/clap" CACHE PATH "CLAP SDK")
set(CLAP_HELPERS_ROOT "${external}/clap-helpers" CACHE PATH "clap-helpers")
set(CLAP_WRAPPER_ROOT "${external}/clap-wrapper" CACHE PATH "clap-wrapper (charCulbert fork)")
set(CLAP_WRAPPER_CHOC_ROOT "${external}/choc" CACHE PATH "CHOC")
set(CHAR_CLAP_UTILS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../char-clap-utils" CACHE PATH "char-clap-utils")
set(CHARDSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../chardsp" CACHE PATH "chardsp")
set(COMPOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../compost" CACHE PATH "compost web components")
foreach(root IN ITEMS CLAP_SDK_ROOT CLAP_HELPERS_ROOT CLAP_WRAPPER_ROOT CLAP_WRAPPER_CHOC_ROOT
                      CHAR_CLAP_UTILS_ROOT CHARDSP_ROOT COMPOST_ROOT)
    if(NOT EXISTS "${${root}}/CMakeLists.txt" AND NOT EXISTS "${${root}}/package.json"
       AND NOT EXISTS "${${root}}/include/char_clap_utils/WebUI.h")
        message(FATAL_ERROR
            "${root} is empty or missing: ${${root}}\n"
            "Point ${root} at an existing checkout.")
    endif()
endforeach()

# Guarded so this file also works when this project is added as a subdirectory of a
# larger build that already provides these targets.
if(NOT TARGET clap)
    add_subdirectory("${CLAP_SDK_ROOT}" clap-sdk EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET clap-helpers)
    add_subdirectory("${CLAP_HELPERS_ROOT}" clap-helpers EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET clap-wrapper-extensions)
    add_subdirectory("${CLAP_WRAPPER_ROOT}" clap-wrapper EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET chardsp)
    add_subdirectory("${CHARDSP_ROOT}" chardsp EXCLUDE_FROM_ALL)
endif()

set(PLUGIN_OUTPUT_NAME "tapa")
set(PLUGIN_ID "com.charlieculbert.tapa")

include("${CHAR_CLAP_UTILS_ROOT}/cmake/CharClapWebUI.cmake")
char_clap_stage_web_ui(tapa
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ui"
    COMPOST_ROOT "${COMPOST_ROOT}"
    COMPONENTS slider)

add_library(tapa-impl STATIC Plugin.cpp)
target_include_directories(tapa-impl PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CHAR_CLAP_UTILS_ROOT}/include")
target_link_libraries(tapa-impl PUBLIC
    clap
    clap-helpers
    clap-wrapper-extensions
    chardsp::chardsp)
char_clap_add_native_webview(tapa-impl "${CLAP_WRAPPER_CHOC_ROOT}")
add_dependencies(tapa-impl tapa-resources)

if(ANY_WASM_TOOLCHAIN)
    set(formats WCLAP)
else()
    set(formats CLAP)
    if(APPLE AND CMAKE_GENERATOR STREQUAL "Xcode")
        list(APPEND formats AUV3)
    endif()
endif()

set(standalone)
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT ANY_WASM_TOOLCHAIN)
    set(standalone STANDALONE_CONFIGURATIONS standalone "${PLUGIN_OUTPUT_NAME}" "${PLUGIN_ID}")
endif()

make_clapfirst_plugins(
    TARGET_NAME tapa
    IMPL_TARGET tapa-impl
    OUTPUT_NAME "${PLUGIN_OUTPUT_NAME}"
    ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Entry.cpp"
    RESOURCE_DIRECTORY "${tapa_RESOURCE_DIRECTORY}"
    BUNDLE_IDENTIFIER "${PLUGIN_ID}"
    BUNDLE_VERSION "${PROJECT_VERSION}"
    PLUGIN_FORMATS ${formats}
    ${standalone}
    # No WCLAP_MEMORY_MINIMUM/INITIAL/MAXIMUM: the heap stays small, so a browser
    # host's default reservation is enough and no memory.json is packaged.
    AUV3_BUILD_STANDALONE TRUE
    AUV2_MANUFACTURER_NAME "Charlie Culbert"
    AUV2_MANUFACTURER_CODE "ChAr"
    AUV2_SUBTYPE_CODE "Tapa"
    AUV2_INSTRUMENT_TYPE "aumu"
    ASSET_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/artifacts"
    COPY_AFTER_BUILD FALSE)

# The wrapper copies resources after linking; UI-only edits must trigger that step.
foreach(bundle IN ITEMS tapa_clap tapa_standalone tapa_auv3)
    if(TARGET ${bundle})
        set_property(TARGET ${bundle} APPEND PROPERTY LINK_DEPENDS
            "${CMAKE_CURRENT_BINARY_DIR}/tapa-resources.stamp")
    endif()
endforeach()

if(ANY_WASM_TOOLCHAIN)
    add_custom_target(tapa-stage-wclap-resources ALL
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "${CMAKE_CURRENT_BINARY_DIR}/artifacts/${PLUGIN_OUTPUT_NAME}.wclap/ui"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${tapa_RESOURCE_DIRECTORY}"
            "${CMAKE_CURRENT_BINARY_DIR}/artifacts/${PLUGIN_OUTPUT_NAME}.wclap"
        COMMAND ${CMAKE_COMMAND} -E tar cfz
            "${CMAKE_CURRENT_BINARY_DIR}/artifacts/${PLUGIN_OUTPUT_NAME}.wclap.tar.gz"
            --format=gnutar module.wasm ui
        DEPENDS tapa_wclap
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/artifacts/${PLUGIN_OUTPUT_NAME}.wclap"
        VERBATIM)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(tapa_ios_host Entry.cpp)
    target_link_libraries(tapa_ios_host PRIVATE tapa-impl)
    target_add_auv3_standalone_ios_wrapper(
        TARGET tapa_ios_host
        OUTPUT_NAME "${PLUGIN_OUTPUT_NAME}"
        BUNDLE_IDENTIFIER "${PLUGIN_ID}"
        BUNDLE_VERSION "${PROJECT_VERSION}"
        AUV3_TARGET tapa_auv3
        RESOURCE_DIRECTORY "${tapa_RESOURCE_DIRECTORY}"
        AU_TYPE "aumu"
        AU_SUBTYPE "Tapa"
        AU_MANUFACTURER "ChAr")
    set_target_properties(tapa_ios_host PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/artifacts")
    add_dependencies(tapa_all tapa_ios_host)
endif()

include(CTest)
if(BUILD_TESTING AND NOT ANY_WASM_TOOLCHAIN AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(tapa-tests tests/TapaTests.cpp)
    target_link_libraries(tapa-tests PRIVATE tapa-impl)
    add_test(NAME tapa-tests COMMAND tapa-tests)
endif()
