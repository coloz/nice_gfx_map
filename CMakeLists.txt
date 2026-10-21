set(nice_gfx_map_sources
    src/Geometry.cpp
    src/Image.cpp
    src/Map.cpp
    src/Providers.cpp
    src/TiandituWmts.cpp
    src/TiandituServices.cpp
    src/Pipeline.cpp
    src/Navigation.cpp
    src/Nmea.cpp
    src/adapters/Esp32.cpp
    src/adapters/TiandituParser.cpp)

# ESP-IDF component: put this directory beside components/nice_gfx.
# PNGdec, NICE_GFX rendering and UI publication are opt-in header adapters.
if(COMMAND idf_component_register)
    # IDF 5 ships cJSON in `json`; IDF 6 commonly uses the managed
    # espressif/cjson component. Explicit dependencies expose both headers and
    # linking, rather than silently compiling JSON services as Unsupported.
    set(nice_gfx_map_requires nice_gfx esp_http_client esp-tls esp_psram esp_timer heap)
    option(NICE_GFX_MAP_ENABLE_TDT_JSON "Enable TianDiTu JSON services" ON)
    if(NICE_GFX_MAP_ENABLE_TDT_JSON)
        if(NOT NICE_GFX_MAP_CJSON_COMPONENT)
            if(EXISTS "${PROJECT_DIR}/managed_components/espressif__cjson/CMakeLists.txt"
               OR EXISTS "${CMAKE_SOURCE_DIR}/managed_components/espressif__cjson/CMakeLists.txt"
               OR EXISTS "${PROJECT_DIR}/components/espressif__cjson/CMakeLists.txt")
                set(NICE_GFX_MAP_CJSON_COMPONENT espressif__cjson)
            elseif(EXISTS "${IDF_PATH}/components/json/CMakeLists.txt"
                   OR EXISTS "$ENV{IDF_PATH}/components/json/CMakeLists.txt")
                set(NICE_GFX_MAP_CJSON_COMPONENT json)
            else()
                message(FATAL_ERROR "TianDiTu JSON needs IDF json or managed espressif/cjson; set NICE_GFX_MAP_CJSON_COMPONENT to your component name, or explicitly disable NICE_GFX_MAP_ENABLE_TDT_JSON.")
            endif()
        endif()
        list(APPEND nice_gfx_map_requires ${NICE_GFX_MAP_CJSON_COMPONENT})
    endif()
    idf_component_register(
        SRCS ${nice_gfx_map_sources}
        INCLUDE_DIRS src
        REQUIRES ${nice_gfx_map_requires})
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
    if(NOT NICE_GFX_MAP_ENABLE_TDT_JSON)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE NICE_GFX_TDT_DISABLE_CJSON=1)
    endif()
    return()
endif()

cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(nice_gfx_map VERSION 0.2.0 LANGUAGES CXX)
endif()

# Portable host core and ESP32 allocator/transport fallbacks need no platform SDK.
add_library(nice_gfx_map STATIC ${nice_gfx_map_sources})
target_include_directories(nice_gfx_map PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(nice_gfx_map PUBLIC cxx_std_17)

include(CTest)
foreach(suite map geometry navigation overlays fs ride recorder performance_touch wmts projection pin wmts_example)
    if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/${suite}_tests.cpp")
        add_executable(nice_gfx_map_${suite}_tests tests/${suite}_tests.cpp)
        target_link_libraries(nice_gfx_map_${suite}_tests PRIVATE nice_gfx_map)
        if(suite STREQUAL "fs" OR suite STREQUAL "recorder")
            target_include_directories(nice_gfx_map_${suite}_tests PRIVATE tests/support)
        endif()
        add_test(NAME nice_gfx_map_${suite}_tests COMMAND nice_gfx_map_${suite}_tests)
        if(suite STREQUAL "wmts" OR suite STREQUAL "wmts_example")
            target_compile_definitions(nice_gfx_map_${suite}_tests PRIVATE NICE_MAP_WMTS_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/wmts/")
        endif()
        if(suite STREQUAL "wmts_example")
            target_include_directories(nice_gfx_map_${suite}_tests PRIVATE examples/17_TiandituLayers)
        endif()
    endif()
endforeach()
