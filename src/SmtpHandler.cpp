#include "SmtpHandler.h"
#include "Connection.h"
#include "Metrics.h"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <sys/epoll.h>

SmtpHandler::SmtpHandler() : state_(SmtpState::INIT) {}

void SmtpHandler::onConnect(std::shared_ptr<Connection> conn, int epoll_fd) {
    conn->keep_alive = true; // SMTP is persistent until QUIT
    sendResponse(conn, epoll_fd, "220 localhost ESMTP\r\n");
}

void SmtpHandler::sendResponse(std::shared_ptr<Connection> conn, int epoll_fd, const std::string& response) {
    std::lock_guard<std::mutex> lock(conn->write_mutex);
    conn->write_queue.push(response);
    struct epoll_event ev;
    ev.data.fd = conn->fd;
    ev.events = EPOLLIN | EPOLLOUT;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
}

void SmtpHandler::onDataReceived(std::shared_ptr<Connection> conn, WorkerPool& pool, int epoll_fd) {
    // Read from conn->read_buf
    std::string data(conn->read_buf.begin(), conn->read_buf.end());
    
    if (state_ == SmtpState::DATA) {
        // Look for \r\n.\r\n
        size_t end_pos = data.find("\r\n.\r\n");
        if (end_pos != std::string::npos) {
            email_data_ += data.substr(0, end_pos);
            conn->read_buf.erase(conn->read_buf.begin(), conn->read_buf.begin() + end_pos + 5);
            saveEmail();
            sendResponse(conn, epoll_fd, "250 OK: queued as 12345\r\n");
            state_ = SmtpState::HELO; // Reset state
        } else {
            email_data_ += data;
            conn->read_buf.clear();
        }
        return;
    }

    // Process line by line
    size_t pos = 0;
    while ((pos = data.find("\r\n")) != std::string::npos) {
        std::string line = data.substr(0, pos);
        data.erase(0, pos + 2);
        conn->read_buf.erase(conn->read_buf.begin(), conn->read_buf.begin() + pos + 2);
        
        processLine(conn, epoll_fd, line);
        
        if (state_ == SmtpState::DATA) {
            // Re-eval remainder of buffer as data
            onDataReceived(conn, pool, epoll_fd);
            return;
        }
    }
}

void SmtpHandler::processLine(std::shared_ptr<Connection> conn, int epoll_fd, const std::string& line) {
    std::string upper_line = line;
    std::transform(upper_line.begin(), upper_line.end(), upper_line.begin(), ::toupper);

    if (upper_line.find("HELO") == 0 || upper_line.find("EHLO") == 0) {
        state_ = SmtpState::HELO;
        sendResponse(conn, epoll_fd, "250 localhost, I am glad to meet you\r\n");
    } else if (upper_line.find("MAIL FROM:") == 0) {
        mail_from_ = line.substr(10);
        state_ = SmtpState::MAIL_FROM;
        sendResponse(conn, epoll_fd, "250 Ok\r\n");
    } else if (upper_line.find("RCPT TO:") == 0) {
        rcpt_to_ = line.substr(8);
        state_ = SmtpState::RCPT_TO;
        sendResponse(conn, epoll_fd, "250 Ok\r\n");
    } else if (upper_line == "DATA") {
        state_ = SmtpState::DATA;
        email_data_.clear();
        sendResponse(conn, epoll_fd, "354 End data with <CR><LF>.<CR><LF>\r\n");
    } else if (upper_line == "RSET") {
        state_ = SmtpState::HELO;
        mail_from_.clear();
        rcpt_to_.clear();
        email_data_.clear();
        sendResponse(conn, epoll_fd, "250 Ok\r\n");
    } else if (upper_line == "QUIT") {
        state_ = SmtpState::QUIT;
        sendResponse(conn, epoll_fd, "221 Bye\r\n");
        conn->keep_alive = false;
    } else {
        sendResponse(conn, epoll_fd, "500 Syntax error, command unrecognized\r\n");
    }
}

void SmtpHandler::saveEmail() {
    std::ofstream out("smtp_inbox.log", std::ios::app);
    out << "--- New Email ---\n";
    out << "From: " << mail_from_ << "\n";
    out << "To: " << rcpt_to_ << "\n";
    out << "Data:\n" << email_data_ << "\n";
    out << "-----------------\n\n";
}

void SmtpHandler::onDisconnect(std::shared_ptr<Connection> conn) {
    // Cleanup if needed
}
