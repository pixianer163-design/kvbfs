cmake_minimum_required(VERSION 3.16)
project(kvbfs C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 编译选项
add_compile_options(-Wall -Wextra -Wpedantic)

# 查找依赖
find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED fuse3)
pkg_check_modules(ROCKSDB REQUIRED rocksdb)

# 包含目录
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${FUSE3_INCLUDE_DIRS}
    ${ROCKSDB_INCLUDE_DIRS}
)

# 源文件
set(KVBFS_SOURCES
    src/main.c
    src/fuse_ops.c
    src/kv_store.c
    src/kv_rocksdb.c
    src/inode.c
    src/utils.c
)

# 主程序
add_executable(kvbfs ${KVBFS_SOURCES})

target_link_libraries(kvbfs
    ${FUSE3_LIBRARIES}
    ${ROCKSDB_LIBRARIES}
    pthread
)

target_compile_definitions(kvbfs PRIVATE
    FUSE_USE_VERSION=35
    _FILE_OFFSET_BITS=64
)

# 测试（可选）
option(BUILD_TESTS "Build tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
