#pragma once

#include <memory>

struct Connection;
class WorkerPool;

class ProtocolHandler {
public:
    virtual ~ProtocolHandler() = default;
    
    // Called when the connection is established
    virtual void onConnect(std::shared_ptr<Connection> conn, int epoll_fd) = 0;
    
    // Called when data is available to be read from the connection
    virtual void onDataReceived(std::shared_ptr<Connection> conn, WorkerPool& pool, int epoll_fd) = 0;
    
    // Called when the connection is closed
    virtual void onDisconnect(std::shared_ptr<Connection> conn) = 0;
};
