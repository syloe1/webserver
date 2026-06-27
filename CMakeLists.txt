cmake_minimum_required(VERSION 3.10)
project(TinyWebServer VERSION 1.0 LANGUAGES C CXX)

# ===================== 编译标准 =====================
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ===================== 编译选项 =====================
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -g -Wall -Wextra")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ===================== 头文件路径 =====================
include_directories(${PROJECT_SOURCE_DIR}/include)

# ===================== 查找依赖库 =====================
find_package(Threads REQUIRED)
find_library(MYSQL_CLIENT mysqlclient REQUIRED)

# ===================== 主服务源文件 =====================
set(SERVER_SOURCES
    main.cc

    src/config/config.cc

    src/core/log.cc
    src/core/lst_timer.cc

    src/db/sql_connection_pool.cc
    src/db/user_cache.cc

    src/net/socket_tool.cc
    src/net/http_conn.cc
    src/net/http_conn_init.cc
    src/net/http_conn_read.cc
    src/net/http_conn_write.cc
    src/net/http_conn_request.cc

    src/server/webserver.cc
)

# ===================== 编译主服务 =====================
add_executable(TinyWebServer ${SERVER_SOURCES})

target_link_libraries(TinyWebServer
    Threads::Threads
    ${MYSQL_CLIENT}
)

# ===================== 拷贝 static 到 build 目录 =====================
add_custom_command(TARGET TinyWebServer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${PROJECT_SOURCE_DIR}/static
    $<TARGET_FILE_DIR:TinyWebServer>/static
    COMMENT "Copying static/ resources to build directory"
)

# ===================== webbench 压测工具 (纯C) =====================
add_executable(webbench
    test/webbench.c
)
set_target_properties(webbench PROPERTIES
    LINKER_LANGUAGE C
)
target_include_directories(webbench PRIVATE ${PROJECT_SOURCE_DIR}/test)
target_compile_options(webbench PRIVATE -Wno-implicit-int -Wno-implicit-function-declaration)
