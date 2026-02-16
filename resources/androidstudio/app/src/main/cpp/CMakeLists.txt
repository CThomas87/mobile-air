cmake_minimum_required(VERSION 3.22.1)
project("com_nativephp_mobile")

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC -fvisibility=default")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -fexceptions -frtti -fPIC -fvisibility=default")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384")

set(PHP_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/arm64-v8a)
set(PHP_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/php)

add_definitions(
        -DHAVE_CONFIG_H
        -D__ANDROID__
        -DANDROID
        -D_GNU_SOURCE
        -D_LARGEFILE64_SOURCE
        -D_FILE_OFFSET_BITS=64
        -DHAVE_SYS_PARAM_H
        -DHAVE_BUILD_DEFS_H
        -D_REENTRANT
        -DHAVE_BUILTIN_EXPECT
)

include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include/php
        ${CMAKE_CURRENT_SOURCE_DIR}/include/php/main
        ${CMAKE_CURRENT_SOURCE_DIR}/include/php/Zend
        ${CMAKE_CURRENT_SOURCE_DIR}/include/php/TSRM
        ${CMAKE_CURRENT_SOURCE_DIR}/include/php/sapi/embed
        ${CMAKE_CURRENT_SOURCE_DIR}/compat
)

# Compat library first
add_library(compat SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/compat/android_compat.cpp
)

target_link_libraries(compat
        android
        log
)


set_target_properties(compat PROPERTIES
        LINK_FLAGS "-Wl,--export-dynamic -Wl,-z,defs -Wl,-z,max-page-size=16384"
        VERSION 1.0
        SOVERSION 1
)

# Import PHP as an imported library
add_library(php SHARED IMPORTED)
set_target_properties(php PROPERTIES
        IMPORTED_LOCATION ${PHP_LIB_DIR}/libphp.so
        IMPORTED_NO_SONAME 1
)

# PHP wrapper
add_library(php_wrapper SHARED
        PHP.c
        php_bridge.c
        libphp_wrapper.cpp
        bridge_jni.cpp
        php_engine.c
        php_thread_context.c
        php_request_context.c
        worker_pool.c
        scheduler_gate.c
        supervisor.c
)

target_include_directories(php_wrapper PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include/php
        ${CMAKE_CURRENT_SOURCE_DIR}/include/php/main
        ${CMAKE_CURRENT_SOURCE_DIR}/include/php/Zend
        ${CMAKE_CURRENT_SOURCE_DIR}/include/php/TSRM
)

target_link_libraries(php_wrapper
        compat
        android
        log
        dl
        php
)

# ─── ZTS Build Check ───
# Check whether php_config.h has ZTS defined.
# If not, the build proceeds in NTS (non-thread-safe) mode with single-worker
# concurrency only. Multi-worker requires a ZTS-enabled PHP binary.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/php_config.h" PHP_CONFIG_CONTENT)
string(FIND "${PHP_CONFIG_CONTENT}" "#define ZTS 1" ZTS_FOUND)
string(FIND "${PHP_CONFIG_CONTENT}" "/* #undef ZTS */" ZTS_UNDEF_FOUND)

if(ZTS_UNDEF_FOUND GREATER -1)
    message(FATAL_ERROR
        "PHP was built WITHOUT ZTS (Zend Thread Safety). "
        "Multi-worker concurrency requires ZTS. "
        "Rebuild PHP with --enable-zts.")
endif()

if(ZTS_FOUND GREATER -1)
    message(STATUS "ZTS confirmed in php_config.h — multi-worker mode available.")
endif()

target_compile_definitions(php_wrapper PRIVATE
        NATIVEPHP_REQUIRE_ZTS=1
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(DEBUG=1)
    add_compile_options(-g -O0 -fno-limit-debug-info)
endif()

# Copy libraries to output directory
add_custom_command(
        TARGET compat POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:compat> ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/
        COMMAND ${CMAKE_COMMAND} -E copy ${PHP_LIB_DIR}/libphp.so ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/
)
