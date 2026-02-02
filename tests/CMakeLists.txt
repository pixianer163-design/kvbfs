# 测试构建配置

# KV 存储测试
add_executable(test_kv_store test_kv_store.c ../src/kv_rocksdb.c)
target_link_libraries(test_kv_store ${ROCKSDB_LIBRARIES})
add_test(NAME test_kv_store COMMAND test_kv_store)

# inode 测试
add_executable(test_inode test_inode.c ../src/inode.c ../src/kv_rocksdb.c)
target_link_libraries(test_inode ${ROCKSDB_LIBRARIES} pthread)
add_test(NAME test_inode COMMAND test_inode)
