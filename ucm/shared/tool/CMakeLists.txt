if(RUNTIME_ENVIRONMENT STREQUAL "ascend")
    set(DEVICE_ID_HELPER_SOURCE ascend/device_id_helper.cc)
else()
    set(DEVICE_ID_HELPER_SOURCE simu/device_id_helper.cc)
endif()

add_library(device_id_helper STATIC ${DEVICE_ID_HELPER_SOURCE})
target_include_directories(device_id_helper PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(device_id_helper PUBLIC infra_status)

if(RUNTIME_ENVIRONMENT STREQUAL "ascend")
    target_link_libraries(device_id_helper PRIVATE trans ${CMAKE_DL_LIBS})
endif()
