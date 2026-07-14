cmake_minimum_required(VERSION 3.10)
project(http-server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# We will need threads for the Thread Pool phase
find_package(Threads REQUIRED)

# Add include directories
include_directories(include)

# Add executable
# Find OpenSSL
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

add_executable(http-server 
    src/main.cpp 
    src/HttpParser.cpp 
    src/Router.cpp 
    src/Response.cpp
    src/WorkerPool.cpp
    src/ThreadPool.cpp
    src/EpollServer.cpp
    src/HttpHandler.cpp
    src/SmtpHandler.cpp
    src/FtpHandler.cpp
    src/ImapHandler.cpp
)

# Link libraries
target_link_libraries(http-server OpenSSL::SSL OpenSSL::Crypto Threads::Threads ZLIB::ZLIB)
