cmake_minimum_required(VERSION 3.16)

# NVMe KV 模拟器
set(SIM_SOURCES
    main.c
    cmd_dispatch.c
    kv_mem.c
)

add_executable(nvme-kv-sim ${SIM_SOURCES})

target_include_directories(nvme-kv-sim PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(nvme-kv-sim pthread)
