#pragma once
#include <openssl/ssl.h>
#include "Router.h"
#include "WorkerPool.h"
#include <memory>

class EpollServer {
public:
    EpollServer(int port, int tls_port, Router& router, SSL_CTX* ctx);
    void run();

private:
    int port_;
    int tls_port_;
    Router& router_;
    SSL_CTX* ctx_;
    WorkerPool thread_pool_;
    int make_socket_non_blocking(int sfd);
};
