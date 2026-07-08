#include "FtpHandler.h"
#include "Connection.h"
#include "EpollServer.h"
#include "Metrics.h"
#include <iostream>
#include <algorithm>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

class FtpDataHandler : public ProtocolHandler {
    std::shared_ptr<FtpHandler> parent_;
public:
    FtpDataHandler(std::shared_ptr<FtpHandler> parent) : parent_(parent) {}
    void onConnect(std::shared_ptr<Connection> conn, int epoll_fd) override {
        parent_->handleDataConnection(conn, epoll_fd);
    }
    void onDataReceived(std::shared_ptr<Connection> conn, WorkerPool& pool, int epoll_fd) override {}
    void onDisconnect(std::shared_ptr<Connection> conn) override {}
};

FtpHandler::FtpHandler() : state_(FtpState::INIT) {}

void FtpHandler::onConnect(std::shared_ptr<Connection> conn, int epoll_fd) {
    conn->keep_alive = true;
    sendResponse(conn, epoll_fd, "220 localhost FTP server ready.\r\n");
}

void FtpHandler::sendResponse(std::shared_ptr<Connection> conn, int epoll_fd, const std::string& response) {
    std::lock_guard<std::mutex> lock(conn->write_mutex);
    conn->write_queue.push(response);
    struct epoll_event ev;
    ev.data.fd = conn->fd;
    ev.events = EPOLLIN | EPOLLOUT;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
}

void FtpHandler::onDataReceived(std::shared_ptr<Connection> conn, WorkerPool& pool, int epoll_fd) {
    std::string data(conn->read_buf.begin(), conn->read_buf.end());
    
    size_t pos = 0;
    while ((pos = data.find("\r\n")) != std::string::npos) {
        std::string line = data.substr(0, pos);
        data.erase(0, pos + 2);
        conn->read_buf.erase(conn->read_buf.begin(), conn->read_buf.begin() + pos + 2);
        
        processLine(conn, epoll_fd, line);
    }
}

void FtpHandler::processLine(std::shared_ptr<Connection> conn, int epoll_fd, const std::string& line) {
    std::string upper_line = line;
    std::transform(upper_line.begin(), upper_line.end(), upper_line.begin(), ::toupper);
    
    Metrics::getInstance().ftp_commands++;

    if (upper_line.find("USER") == 0) {
        sendResponse(conn, epoll_fd, "331 Please specify the password.\r\n");
    } else if (upper_line.find("PASS") == 0) {
        state_ = FtpState::LOGGED_IN;
        sendResponse(conn, epoll_fd, "230 Login successful.\r\n");
    } else if (upper_line.find("SYST") == 0) {
        sendResponse(conn, epoll_fd, "215 UNIX Type: L8\r\n");
    } else if (upper_line.find("PASV") == 0) {
        std::cout << "[FTP] Received PASV command" << std::endl;
        data_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(data_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        
        bool bound = false;
        int port = 0;
        for (int p = 30000; p <= 30010; ++p) {
            addr.sin_port = htons(p);
            if (bind(data_fd_, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                bound = true;
                port = p;
                break;
            }
        }
        
        if (!bound) {
            std::cout << "[FTP] Failed to bind data socket" << std::endl;
            sendResponse(conn, epoll_fd, "425 Can't open data connection.\r\n");
            close(data_fd_);
            data_fd_ = -1;
            return;
        }
        
        listen(data_fd_, 5);
        
        int p1 = port / 256;
        int p2 = port % 256;
        
        std::string response = "227 Entering Passive Mode (127,0,0,1," + std::to_string(p1) + "," + std::to_string(p2) + ").\r\n";
        std::cout << "[FTP] Sending PASV response: " << response << std::flush;
        sendResponse(conn, epoll_fd, response);
        
        std::cout << "[FTP] Registering dynamic listener..." << std::endl;
        std::shared_ptr<FtpHandler> self = shared_from_this();
        EpollServer::registerDynamicListener(data_fd_, [self]() {
            return std::make_shared<FtpDataHandler>(self);
        });
        
        struct epoll_event ev;
        ev.data.fd = data_fd_;
        ev.events = EPOLLIN;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, data_fd_, &ev);
    } else if (upper_line.find("LIST") == 0) {
        sendResponse(conn, epoll_fd, "150 Here comes the directory listing.\r\n");
        sendResponse(conn, epoll_fd, "226 Directory send OK.\r\n");
    } else if (upper_line.find("QUIT") == 0) {
        state_ = FtpState::QUIT;
        sendResponse(conn, epoll_fd, "221 Goodbye.\r\n");
        conn->keep_alive = false;
    } else {
        // As requested by user, we only build the skeleton for now and skip complex features like PASV
        sendResponse(conn, epoll_fd, "502 Command not implemented.\r\n");
    }
}

void FtpHandler::handleDataConnection(std::shared_ptr<Connection> data_conn, int epoll_fd) {
    std::string listing = "drwxr-xr-x 2 user group 4096 Jul 09 12:00 .\r\n";
    {
        std::lock_guard<std::mutex> lock(data_conn->write_mutex);
        data_conn->write_queue.push(listing);
    }
    data_conn->keep_alive = false;
    struct epoll_event ev;
    ev.data.fd = data_conn->fd;
    ev.events = EPOLLIN | EPOLLOUT;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, data_conn->fd, &ev);
    
    // Deregister the dynamic listener so it's not leaked
    EpollServer::unregisterDynamicListener(data_fd_);
    close(data_fd_);
    data_fd_ = -1;
}

void FtpHandler::onDisconnect(std::shared_ptr<Connection> conn) {
    if (data_fd_ != -1) {
        EpollServer::unregisterDynamicListener(data_fd_);
        close(data_fd_);
    }
}
