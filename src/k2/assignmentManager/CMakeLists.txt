file(GLOB HEADERS "*.h")
file(GLOB SOURCES "*.cpp")

add_library(k2assignment_manager STATIC ${HEADERS} ${SOURCES})
target_link_libraries (k2assignment_manager PRIVATE k2common k2transport Seastar::seastar k2dto k2partition_manager)
