file(GLOB HEADERS "*.h")
file(GLOB SOURCES "*.cpp")

add_library(k2node_pool_monitor STATIC ${HEADERS} ${SOURCES})
target_link_libraries (k2node_pool_monitor PRIVATE k2common k2transport Seastar::seastar k2dto)
