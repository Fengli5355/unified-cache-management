file(GLOB KV_COMMON_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(kv_common STATIC ${KV_COMMON_SOURCES})
target_include_directories(kv_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(BUILD_UNIT_TESTS)
    add_executable(kv_router_perf perf/router_perf.cpp)
    target_link_libraries(kv_router_perf PRIVATE kv_common)
endif()
