set(SOURCES execution_manager.cpp predicate_manager.h)
add_library(execution STATIC ${SOURCES})

target_link_libraries(execution system record transaction planner)