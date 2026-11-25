cmake_minimum_required(VERSION 3.20)
project(sable_cpp VERSION 4.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND)
    FetchContent_Declare(
        sqlite3_amalgamation
        URL https://www.sqlite.org/2024/sqlite-amalgamation-3470200.zip
    )
    FetchContent_MakeAvailable(sqlite3_amalgamation)
    add_library(sqlite3 STATIC ${sqlite3_amalgamation_SOURCE_DIR}/sqlite3.c)
    target_include_directories(sqlite3 PUBLIC ${sqlite3_amalgamation_SOURCE_DIR})
    set(SQLITE3_TARGET sqlite3)
else()
    set(SQLITE3_TARGET SQLite::SQLite3)
endif()

add_library(libsable STATIC
    src/sable.cpp
)
target_include_directories(libsable PUBLIC
    include
)
target_link_libraries(libsable PUBLIC
    ${SQLITE3_TARGET}
)

add_executable(sable
    src/main.cpp
)
target_link_libraries(sable PRIVATE
    libsable
)

add_executable(test_sable
    tests/test_sable.cpp
)
target_link_libraries(test_sable PRIVATE
    libsable
)
