cmake_minimum_required(VERSION 2.8)
project(lws-minimal-ws-client-okx C)

set(SAMP lws-minimal-ws-client-okx)
set(SRCS main.c)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall")

add_executable(${SAMP} ${SRCS})
target_link_libraries(${SAMP} websockets)

install(TARGETS ${SAMP}
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    PUBLIC_HEADER DESTINATION include
)
