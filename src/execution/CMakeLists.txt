set(SOURCES execution_manager.cpp load_executor.cpp)
add_library(execution STATIC ${SOURCES})

target_link_libraries(execution system record transaction planner)
