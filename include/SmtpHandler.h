#pragma once

#include "ProtocolHandler.h"
#include <string>

enum class SmtpState { INIT, HELO, MAIL_FROM, RCPT_TO, DATA, QUIT };

class SmtpHandler : public ProtocolHandler {
public:
    SmtpHandler();
    virtual ~SmtpHandler() = default;

    void onConnect(std::shared_ptr<Connection> conn, int epoll_fd) override;
    void onDataReceived(std::shared_ptr<Connection> conn, WorkerPool& pool, int epoll_fd) override;
    void onDisconnect(std::shared_ptr<Connection> conn) override;

private:
    SmtpState state_;
    std::string email_data_;
    std::string mail_from_;
    std::string rcpt_to_;
    
    void sendResponse(std::shared_ptr<Connection> conn, int epoll_fd, const std::string& response);
    void processLine(std::shared_ptr<Connection> conn, int epoll_fd, const std::string& line);
    void saveEmail();
};
