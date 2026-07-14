#pragma once

#include "ProtocolHandler.h"
#include <string>

enum class FtpState { INIT, LOGGED_IN, QUIT };

class FtpHandler : public ProtocolHandler, public std::enable_shared_from_this<FtpHandler> {
public:
    FtpHandler();
    virtual ~FtpHandler() = default;

    void onConnect(std::shared_ptr<Connection> conn, int epoll_fd) override;
    void onDataReceived(std::shared_ptr<Connection> conn, WorkerPool& pool, int epoll_fd) override;
    void onDisconnect(std::shared_ptr<Connection> conn) override;

private:
    FtpState state_;
    int data_fd_ = -1;
    
    void sendResponse(std::shared_ptr<Connection> conn, int epoll_fd, const std::string& response);
    void processLine(std::shared_ptr<Connection> conn, int epoll_fd, const std::string& line);
public:
    void handleDataConnection(std::shared_ptr<Connection> data_conn, int epoll_fd);
};
