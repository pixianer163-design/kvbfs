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

# 本地 LLM 支持（可选）
option(CFS_LOCAL_LLM "Enable local LLM inference via llama.cpp" OFF)

set(LLM_SOURCES "")
set(LLM_LIBS "")
set(LLM_INCLUDE_DIRS "")
set(LLM_DEFINITIONS "")

if(CFS_LOCAL_LLM)
    # llama.cpp 路径（可通过 -DLLAMA_DIR= 指定）
    set(LLAMA_DIR "" CACHE PATH "Path to llama.cpp source tree")

    if(LLAMA_DIR)
        set(LLM_INCLUDE_DIRS ${LLAMA_DIR}/include ${LLAMA_DIR}/ggml/include)
        set(LLM_LINK_DIRS ${LLAMA_DIR}/build/bin)
        set(LLM_LIBS llama ggml ggml-base ggml-cpu)
    else()
        # 尝试 pkg-config
        pkg_check_modules(LLAMA llama)
        if(LLAMA_FOUND)
            set(LLM_INCLUDE_DIRS ${LLAMA_INCLUDE_DIRS})
            set(LLM_LIBS ${LLAMA_LIBRARIES})
        else()
            # 回退：系统路径
            set(LLM_LIBS llama)
        endif()
    endif()

    set(LLM_SOURCES src/llm.c)
    set(LLM_DEFINITIONS CFS_LOCAL_LLM=1)
    message(STATUS "CFS local LLM: enabled")
else()
    message(STATUS "CFS local LLM: disabled")
endif()

# Memory/embedding subsystem (independent of CFS_LOCAL_LLM)
option(CFS_MEMORY "Enable memory/embedding subsystem" OFF)

set(MEM_SOURCES "")
set(MEM_LIBS "")
set(MEM_INCLUDE_DIRS "")
set(MEM_DEFINITIONS "")
set(MEM_LINK_DIRS "")

if(CFS_MEMORY)
    # llama.cpp is needed for embeddings — reuse LLAMA_DIR if set, else find independently
    set(LLAMA_DIR "" CACHE PATH "Path to llama.cpp source tree")

    if(NOT CFS_LOCAL_LLM)
        # CFS_LOCAL_LLM already configured llama paths; only search if LLM is off
        if(LLAMA_DIR)
            set(MEM_INCLUDE_DIRS ${LLAMA_DIR}/include ${LLAMA_DIR}/ggml/include)
            set(MEM_LINK_DIRS ${LLAMA_DIR}/build/bin)
            set(MEM_LIBS llama ggml ggml-base ggml-cpu)
        else()
            pkg_check_modules(LLAMA_MEM llama)
            if(LLAMA_MEM_FOUND)
                set(MEM_INCLUDE_DIRS ${LLAMA_MEM_INCLUDE_DIRS})
                set(MEM_LIBS ${LLAMA_MEM_LIBRARIES})
            else()
                find_path(LLAMA_H_DIR llama.h)
                find_library(LLAMA_LIB llama)
                if(LLAMA_H_DIR AND LLAMA_LIB)
                    set(MEM_INCLUDE_DIRS ${LLAMA_H_DIR})
                    set(MEM_LIBS ${LLAMA_LIB})
                else()
                    set(MEM_LIBS llama)
                endif()
            endif()
        endif()
    endif()

    set(MEM_SOURCES src/mem.c src/events.c)
    list(APPEND MEM_DEFINITIONS CFS_MEMORY=1)
    message(STATUS "CFS memory subsystem: enabled")
else()
    message(STATUS "CFS memory subsystem: disabled")
endif()

# 包含目录
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${FUSE3_INCLUDE_DIRS}
    ${BACKEND_INCLUDE_DIRS}
    ${LLM_INCLUDE_DIRS}
    ${MEM_INCLUDE_DIRS}
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
    src/version.c
    src/utils.c
    src/vfs_versions.c
    ${LLM_SOURCES}
    ${MEM_SOURCES}
)

# 链接目录（用于本地 llama.cpp 构建）
if(LLM_LINK_DIRS)
    link_directories(${LLM_LINK_DIRS})
endif()
if(MEM_LINK_DIRS)
    link_directories(${MEM_LINK_DIRS})
endif()

# 主程序
add_executable(kvbfs ${KVBFS_SOURCES})

target_link_libraries(kvbfs
    ${FUSE3_LIBRARIES}
    ${BACKEND_LIBS}
    ${LLM_LIBS}
    ${MEM_LIBS}
    pthread
    m
)

target_compile_definitions(kvbfs PRIVATE
    FUSE_USE_VERSION=35
    _FILE_OFFSET_BITS=64
    ${LLM_DEFINITIONS}
    ${MEM_DEFINITIONS}
)

# 设置 RPATH 以便运行时找到本地 llama.cpp 库
set(_RPATH_DIRS "")
if(LLM_LINK_DIRS)
    list(APPEND _RPATH_DIRS ${LLM_LINK_DIRS})
endif()
if(MEM_LINK_DIRS)
    list(APPEND _RPATH_DIRS ${MEM_LINK_DIRS})
endif()
if(_RPATH_DIRS)
    list(REMOVE_DUPLICATES _RPATH_DIRS)
    set_target_properties(kvbfs PROPERTIES
        BUILD_RPATH "${_RPATH_DIRS}"
        INSTALL_RPATH "${_RPATH_DIRS}"
    )
endif()

# 模拟器 (始终可构建)
add_subdirectory(sim)

# 测试（可选）
option(BUILD_TESTS "Build tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
