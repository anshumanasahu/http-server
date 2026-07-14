#pragma once
#include <openssl/ssl.h>
#include "Router.h"
#include "WorkerPool.h"
#include "ProtocolHandler.h"
#include <memory>
#include <vector>
#include <functional>
#include <unordered_map>
#include <mutex>

enum class ProtocolType { HTTP, FTP, SMTP, IMAP, FTP_DATA };

struct ListenerConfig {
    int fd;
    int port;
    ProtocolType protocol;
    bool is_tls;
};

class EpollServer {
public:
    EpollServer(std::vector<ListenerConfig> listeners, Router& router, SSL_CTX* ctx);
    void run();
    static void registerDynamicListener(int fd, std::function<std::shared_ptr<ProtocolHandler>()> factory);
    static bool isDynamicListener(int fd, std::function<std::shared_ptr<ProtocolHandler>()>& factory_out);
    static void unregisterDynamicListener(int fd);

private:
    std::vector<ListenerConfig> listeners_;
    Router& router_;
    SSL_CTX* ctx_;
    WorkerPool thread_pool_;
    int make_socket_non_blocking(int sfd);

    static std::unordered_map<int, std::function<std::shared_ptr<ProtocolHandler>()>> dynamic_listeners_;
    static std::mutex dynamic_listeners_mutex_;
};
