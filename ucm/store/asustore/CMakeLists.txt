add_library(asustore STATIC distributed_hash_table.cc view_mgr.cc)
target_include_directories(asustore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(asustore PUBLIC Threads::Threads)
