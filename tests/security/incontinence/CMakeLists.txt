PROJECT( icontinence )
cmake_minimum_required(VERSION 2.8.12 FATAL_ERROR)
include(ExternalProject)

find_package(arcan_shmif REQUIRED)

include_directories(${ARCAN_SHMIF_INCLUDE_DIR})
add_executable(incontinence "incontinence.c")
target_link_libraries(incontinence ${ARCAN_SHMIF_LIBRARY})
