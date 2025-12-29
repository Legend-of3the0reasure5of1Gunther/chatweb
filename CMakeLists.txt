# 文件：CMakeLists.txt
cmake_minimum_required(VERSION 3.15)
project(SecureChatFrontend C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# 版本信息
set(PROJECT_VERSION_MAJOR 1)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_PATCH 0)

# 可执行文件配置
option(BUILD_GUI "Build GUI application" ON)
option(BUILD_CLI "Build CLI application" ON)
option(BUILD_WEB "Build Web interface" ON)
option(BUILD_MOBILE "Build Mobile apps" OFF)
option(BUILD_TESTS "Build tests" ON)
option(BUILD_DOCS "Build documentation" OFF)

# 依赖查找
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

if(BUILD_GUI)
    find_package(Qt5 COMPONENTS Core Widgets Network REQUIRED)
    find_package(GTK3 REQUIRED)
endif()

if(BUILD_WEB)
    find_package(WebAssembly REQUIRED)
endif()

# 包含目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
    ${ZLIB_INCLUDE_DIR}
)

# 源文件
set(FRONTEND_CORE_SOURCES
    src/frontend_sdk.c
    src/gui_adapter.c
    src/message_router.c
    src/security_module.c
    src/cache_manager.c
    src/event_handler.c
    src/file_transfer_frontend.c
)

set(FRONTEND_GUI_SOURCES
    src/gui/qt_adapter.c
    src/gui/gtk_adapter.c
    src/gui/win32_adapter.c
    src/gui/web_adapter.c
)

set(FRONTEND_CLI_SOURCES
    src/cli/main.c
    src/cli/interface.c
    src/cli/commands.c
)

# 创建库
add_library(frontend_core SHARED ${FRONTEND_CORE_SOURCES})
target_link_libraries(frontend_core
    PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    ${ZLIB_LIBRARIES}
)

# GUI应用
if(BUILD_GUI)
    add_executable(secure_chat_gui
        src/gui/main.c
        ${FRONTEND_GUI_SOURCES}
    )
    
    target_link_libraries(secure_chat_gui
        PRIVATE
        frontend_core
        Qt5::Core
        Qt5::Widgets
        Qt5::Network
        ${GTK3_LIBRARIES}
    )
endif()

# CLI应用
if(BUILD_CLI)
    add_executable(secure_chat_cli
        ${FRONTEND_CLI_SOURCES}
    )
    
    target_link_libraries(secure_chat_cli
        PRIVATE
        frontend_core
    )
endif()

# WebAssembly构建
if(BUILD_WEB AND EMSCRIPTEN)
    set(CMAKE_EXECUTABLE_SUFFIX ".html")
    
    add_executable(secure_chat_web
        src/web/main.c
        src/web/wasm_bridge.c
    )
    
    target_link_libraries(secure_chat_web
        PRIVATE
        frontend_core
    )
    
    # Emscripten设置
    set_target_properties(secure_chat_web PROPERTIES
        LINK_FLAGS "-s WASM=1 -s ALLOW_MEMORY_GROWTH=1 -s EXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
    )
endif()

# 安装规则
install(TARGETS frontend_core
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include/secure_chat
)

# 测试
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# 文档
if(BUILD_DOCS)
    find_package(Doxygen)
    if(DOXYGEN_FOUND)
        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in
                       ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile @ONLY)
        add_custom_target(docs
            COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating API documentation with Doxygen"
            VERBATIM
        )
    endif()
endif()