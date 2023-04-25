#ifndef SERVERTCP_H
#define SERVERTCP_H

#include <QTextEdit>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "liburing.h"


#define MAX_CONNECTIONS     4096
#define BACKLOG             512
#define MAX_MESSAGE_LEN     2048
#define BUFFERS_COUNT       MAX_CONNECTIONS


class TcpServer :QObject
{
    Q_OBJECT

public:
    TcpServer(QTextEdit* te, int port);
    bool startServer();

public slots:
    bool checkMessage();

private:
    enum {
        ACCEPT,
        READ,
        WRITE,
        PROV_BUF,
    };

    typedef struct conn_info {
        __u32 fd;
        __u16 type;
        __u16 bid;
    } conn_info;

    char bufs[BUFFERS_COUNT][MAX_MESSAGE_LEN] = {0};
    int group_id = 1337;

    struct io_uring *ring = new io_uring();
    QTextEdit* text_edit = nullptr;
    int sock_listen_fd = 0;
    int portno = 8080;

    struct sockaddr_in serv_addr, client_addr;
    socklen_t client_len;

    int msec_check = 500;

    int msec_delay_answer = 5000;

    void add_accept(int fd, struct sockaddr *client_addr, socklen_t *client_len, unsigned flags);
    void add_socket_read(int fd, unsigned gid, size_t size, unsigned flags);
    void add_socket_write(int fd, __u16 bid, size_t size, unsigned flags);
    void add_provide_buf(__u16 bid, unsigned gid);

    void printServer(QString text);
};

#endif // SERVERTCP_H
