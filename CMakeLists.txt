cmake_minimum_required(VERSION 2.6)

PROJECT(seriald C)
ADD_DEFINITIONS(-Os -Wall -Werror --std=gnu99 -g3 -Wmissing-declarations)

SET(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")

FIND_LIBRARY(ubox_library NAMES ubox)
FIND_PATH(ubox_include_dir libubox/usock.h)
INCLUDE_DIRECTORIES(${ubox_include_dir})

FIND_LIBRARY(blob_library NAMES blobmsg_json)
FIND_LIBRARY(ubus NAMES ubus)

ADD_EXECUTABLE(seriald seriald.c term.c fdio.c ubus.c ubus_loop.c)
TARGET_LINK_LIBRARIES(seriald ${ubus} ${ubox_library} ${blob_library})

INSTALL(TARGETS seriald
	RUNTIME DESTINATION sbin
)
