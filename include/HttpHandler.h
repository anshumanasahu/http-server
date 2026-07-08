#pragma once

#include "ProtocolHandler.h"
#include "Router.h"

class HttpHandler : public ProtocolHandler {
public:
    HttpHandler(Router& router);
    virtual ~HttpHandler() = default;

    void onConnect(std::shared_ptr<Connection> conn, int epoll_fd) override;
    void onDataReceived(std::shared_ptr<Connection> conn, WorkerPool& pool, int epoll_fd) override;
    void onDisconnect(std::shared_ptr<Connection> conn) override;

private:
    Router& router_;
};
