#pragma once

#include "ProtocolHandler.h"
#include <string>

enum class ImapState { NOT_AUTHENTICATED, AUTHENTICATED, SELECTED, LOGOUT };

class ImapHandler : public ProtocolHandler {
public:
    ImapHandler();
    virtual ~ImapHandler() = default;

    void onConnect(std::shared_ptr<Connection> conn, int epoll_fd) override;
    void onDataReceived(std::shared_ptr<Connection> conn, WorkerPool& pool, int epoll_fd) override;
    void onDisconnect(std::shared_ptr<Connection> conn) override;

private:
    ImapState state_;
    
    void sendResponse(std::shared_ptr<Connection> conn, int epoll_fd, const std::string& response);
    void processLine(std::shared_ptr<Connection> conn, int epoll_fd, const std::string& line);
};
