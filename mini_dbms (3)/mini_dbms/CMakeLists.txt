cmake_minimum_required(VERSION 3.10)
project(mini_dbms CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(mini_dbms
    src/main.cpp
    src/pager.cpp
    src/btree.cpp
    src/table.cpp
    src/database.cpp
    src/sql.cpp
)

target_include_directories(mini_dbms PRIVATE src)