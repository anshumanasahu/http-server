#include "ImapHandler.h"
#include "Connection.h"
#include "Metrics.h"
#include <iostream>
#include <algorithm>
#include <sys/epoll.h>

ImapHandler::ImapHandler() : state_(ImapState::NOT_AUTHENTICATED) {}

void ImapHandler::onConnect(std::shared_ptr<Connection> conn, int epoll_fd) {
    conn->keep_alive = true;
    sendResponse(conn, epoll_fd, "* OK IMAP4rev1 Service Ready\r\n");
}

void ImapHandler::sendResponse(std::shared_ptr<Connection> conn, int epoll_fd, const std::string& response) {
    std::lock_guard<std::mutex> lock(conn->write_mutex);
    conn->write_queue.push(response);
    struct epoll_event ev;
    ev.data.fd = conn->fd;
    ev.events = EPOLLIN | EPOLLOUT;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
}

void ImapHandler::onDataReceived(std::shared_ptr<Connection> conn, WorkerPool& pool, int epoll_fd) {
    std::string data(conn->read_buf.begin(), conn->read_buf.end());
    
    size_t pos = 0;
    while ((pos = data.find("\r\n")) != std::string::npos) {
        std::string line = data.substr(0, pos);
        data.erase(0, pos + 2);
        conn->read_buf.erase(conn->read_buf.begin(), conn->read_buf.begin() + pos + 2);
        
        processLine(conn, epoll_fd, line);
    }
}

void ImapHandler::processLine(std::shared_ptr<Connection> conn, int epoll_fd, const std::string& line) {
    // IMAP commands are prefixed with a tag, e.g., "A001 LOGIN user pass"
    size_t space_pos = line.find(' ');
    if (space_pos == std::string::npos) {
        sendResponse(conn, epoll_fd, "* BAD Invalid command format\r\n");
        return;
    }
    
    std::string tag = line.substr(0, space_pos);
    std::string cmd_with_args = line.substr(space_pos + 1);
    
    std::string upper_cmd = cmd_with_args;
    std::transform(upper_cmd.begin(), upper_cmd.end(), upper_cmd.begin(), ::toupper);
    
    Metrics::getInstance().imap_commands++;
    
    if (upper_cmd.find("CAPABILITY") == 0) {
        sendResponse(conn, epoll_fd, "* CAPABILITY IMAP4rev1\r\n" + tag + " OK CAPABILITY completed\r\n");
    } else if (upper_cmd.find("LOGIN") == 0) {
        state_ = ImapState::AUTHENTICATED;
        sendResponse(conn, epoll_fd, tag + " OK LOGIN completed\r\n");
    } else if (upper_cmd.find("LOGOUT") == 0) {
        state_ = ImapState::LOGOUT;
        sendResponse(conn, epoll_fd, "* BYE IMAP4rev1 Server logging out\r\n" + tag + " OK LOGOUT completed\r\n");
        conn->keep_alive = false;
    } else {
        sendResponse(conn, epoll_fd, tag + " BAD Command not implemented\r\n");
    }
}

void ImapHandler::onDisconnect(std::shared_ptr<Connection> conn) {
    // Cleanup if needed
}
