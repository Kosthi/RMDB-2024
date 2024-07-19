set(SOURCES concurrency/lock_manager.cpp concurrency/watermark.cpp transaction_manager.cpp)
add_library(transaction STATIC ${SOURCES})
target_link_libraries(transaction system recovery pthread)