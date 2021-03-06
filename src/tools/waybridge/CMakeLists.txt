PROJECT( arcan-wayland )
cmake_minimum_required(VERSION 2.8.0 FATAL_ERROR)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(Wayland)
find_package(Wayland)
find_package(WaylandProtocols REQUIRED)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
	add_definitions(-D_DEBUG)
endif()

if (ARCAN_SOURCE_DIR)
	add_subdirectory(${ARCAN_SOURCE_DIR}/shmif ashmif)
else()
	find_package(arcan_shmif REQUIRED WITH_EXT)
endif()

include_directories(
	${ARCAN_SHMIF_INCLUDE_DIR}
	${WAYLAND_SERVER_INCLUDE_DIRS}
	${CMAKE_CURRENT_BINARY_DIR}
)
add_definitions(
	-Wall
	-D__UNIX
	-DPOSIX_C_SOURCE
	-DGNU_SOURCE
  -Wno-unused-but-set-variable
	-Wno-unused-function
	-std=gnu11 # shmif-api requires this
)

# copied from Cloudef/wlc/protos
set(PROTOCOLS "${WAYLANDPROTOCOLS_PATH}/unstable/xdg-shell/xdg-shell-unstable-v6")
foreach(proto ${PROTOCOLS})
   get_filename_component(base ${proto} NAME)
   wayland_add_protocol_server(src "${proto}.xml" ${base})
   list(APPEND sources ${src})
endforeach()
set_source_files_properties(${sources} PROPERTIES GENERATED ON)
add_library(wlprot STATIC ${sources})

SET(LIBRARIES
	pthread
	m
	EGL
	${ARCAN_SHMIF_LIBRARY}
	${ARCAN_SHMIF_EXT_LIBRARY}
	${WAYLAND_SERVER_LIBRARIES}
	wlprot
)

SET(SOURCES
	waybridge.c
)

add_executable(${PROJECT_NAME} ${SOURCES})
target_link_libraries(${PROJECT_NAME} ${LIBRARIES})
