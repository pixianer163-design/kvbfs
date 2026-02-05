cmake_minimum_required(VERSION 3.16)
project(kvbfs C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 编译选项
add_compile_options(-Wall -Wextra -Wpedantic)

# 后端选择
set(KVBFS_BACKEND "rocksdb" CACHE STRING "KV backend: rocksdb or nvme")

# 查找依赖
find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED fuse3)

if(KVBFS_BACKEND STREQUAL "rocksdb")
    pkg_check_modules(ROCKSDB REQUIRED rocksdb)
    set(BACKEND_SRC src/kv_rocksdb.c)
    set(BACKEND_LIBS ${ROCKSDB_LIBRARIES})
    set(BACKEND_INCLUDE_DIRS ${ROCKSDB_INCLUDE_DIRS})
elseif(KVBFS_BACKEND STREQUAL "nvme")
    set(BACKEND_SRC src/kv_nvme.c)
    set(BACKEND_LIBS "")
    set(BACKEND_INCLUDE_DIRS "")
else()
    message(FATAL_ERROR "Unknown KVBFS_BACKEND: ${KVBFS_BACKEND} (use 'rocksdb' or 'nvme')")
endif()

message(STATUS "KVBFS backend: ${KVBFS_BACKEND}")

# 包含目录
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${FUSE3_INCLUDE_DIRS}
    ${BACKEND_INCLUDE_DIRS}
)

# 源文件
set(KVBFS_SOURCES
    src/main.c
    src/fuse_ops.c
    src/kv_store.c
    ${BACKEND_SRC}
    src/inode.c
    src/super.c
    src/context.c
    src/utils.c
)

# 主程序
add_executable(kvbfs ${KVBFS_SOURCES})

target_link_libraries(kvbfs
    ${FUSE3_LIBRARIES}
    ${BACKEND_LIBS}
    pthread
)

target_compile_definitions(kvbfs PRIVATE
    FUSE_USE_VERSION=35
    _FILE_OFFSET_BITS=64
)

# 模拟器 (始终可构建)
add_subdirectory(sim)

# 测试（可选）
option(BUILD_TESTS "Build tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
