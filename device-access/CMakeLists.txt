cmake_minimum_required(VERSION 3.16)
project(odac C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)
if(NOT CMAKE_CROSSCOMPILING)
    add_compile_options(-O2 -D_FORTIFY_SOURCE=2 -fstack-protector-strong)
endif()

# Explicit header and library lookup keeps the same CMakeLists working for the
# cross build against the OpenWrt staging directory and for host test builds.
find_path(JSONC_INCLUDE_DIR json-c/json.h)
find_library(JSONC_LIBRARY NAMES json-c)
find_path(SQLITE_INCLUDE_DIR sqlite3.h)
find_library(SQLITE_LIBRARY NAMES sqlite3)
find_path(ARGON2_INCLUDE_DIR argon2.h)
find_library(ARGON2_LIBRARY NAMES argon2)

foreach(required JSONC_INCLUDE_DIR JSONC_LIBRARY SQLITE_INCLUDE_DIR SQLITE_LIBRARY
              ARGON2_INCLUDE_DIR ARGON2_LIBRARY)
    if(NOT ${required})
        message(FATAL_ERROR "Required dependency not found: ${required}")
    endif()
endforeach()

add_library(odac_json_text STATIC src/json_text.c)
target_include_directories(odac_json_text PUBLIC src ${JSONC_INCLUDE_DIR})
target_link_libraries(odac_json_text PUBLIC ${JSONC_LIBRARY})

add_library(odac_transport STATIC src/transport.c src/config.c)
target_include_directories(odac_transport PUBLIC src ${JSONC_INCLUDE_DIR})
target_link_libraries(odac_transport PUBLIC ${JSONC_LIBRARY} odac_json_text)

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS
    src/core*.c src/db*.c src/account*.c src/session*.c src/policy*.c src/portal.c src/portal_*.c)
file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS src/platform*.c)

add_library(odac_core STATIC ${CORE_SOURCES})
target_include_directories(odac_core PUBLIC src ${JSONC_INCLUDE_DIR} ${SQLITE_INCLUDE_DIR} ${ARGON2_INCLUDE_DIR})
target_link_libraries(odac_core PUBLIC ${JSONC_LIBRARY} ${SQLITE_LIBRARY} ${ARGON2_LIBRARY} odac_json_text)

add_library(odac_platform STATIC ${PLATFORM_SOURCES})
target_include_directories(odac_platform PUBLIC src ${JSONC_INCLUDE_DIR})
target_link_libraries(odac_platform PUBLIC ${JSONC_LIBRARY} odac_core)

add_executable(odacd src/main.c)
target_link_libraries(odacd PRIVATE odac_core odac_platform odac_transport)

add_executable(odacctl src/odacctl.c src/readiness.c)
target_link_libraries(odacctl PRIVATE odac_transport)

add_executable(odac-portal src/portal-cgi.c)
target_link_libraries(odac-portal PRIVATE odac_transport)

install(TARGETS odacd odacctl RUNTIME DESTINATION sbin)
install(TARGETS odac-portal RUNTIME DESTINATION libexec/odac)

option(ODAC_TESTS "Build host tests" OFF)
if(ODAC_TESTS)
    enable_testing()
    add_library(odac_core_test STATIC ${CORE_SOURCES})
    target_include_directories(odac_core_test PUBLIC src ${JSONC_INCLUDE_DIR} ${SQLITE_INCLUDE_DIR} ${ARGON2_INCLUDE_DIR})
    target_compile_definitions(odac_core_test PRIVATE ODAC_TEST_HOOKS)
    target_link_libraries(odac_core_test PUBLIC ${JSONC_LIBRARY} ${SQLITE_LIBRARY} ${ARGON2_LIBRARY} odac_json_text)
    add_library(odac_test_hooks STATIC ../tests/core_hooks.c)
    target_compile_definitions(odac_test_hooks PRIVATE ODAC_TEST_HOOKS)
    add_executable(odac-portal-test src/portal-cgi.c)
    target_compile_definitions(odac-portal-test PRIVATE ODAC_TEST_BUILD)
    target_link_libraries(odac-portal-test PRIVATE odac_transport)
    add_custom_command(TARGET odac-portal-test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink odac-portal-test landing)
    add_test(NAME boundary COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/../tests/boundary_test.sh ${CMAKE_CURRENT_BINARY_DIR})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../tests/core_test.c)
        add_executable(core_test ../tests/core_test.c)
        target_compile_definitions(core_test PRIVATE ODAC_TEST_HOOKS)
        target_link_libraries(core_test PRIVATE odac_core_test odac_test_hooks)
        add_test(NAME core COMMAND core_test)
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../tests/platform_test.c)
        add_executable(platform_test ../tests/platform_test.c)
        target_link_libraries(platform_test PRIVATE odac_platform)
    endif()
endif()
