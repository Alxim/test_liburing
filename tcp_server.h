#ifndef SERVERTCP_H
#define SERVERTCP_H

#include <QTextEdit>

#include <liburing.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define MAX_CONNECTIONS 4096
#define BACKLOG 512
#define MAX_MESSAGE_LEN 2048
#define IORING_FEAT_FAST_POLL (1U << 5)


class TcpServer :QObject
{
    Q_OBJECT

public:
    TcpServer(QTextEdit* te, int port);
    bool startServer();

public slots:
    bool checkMessage();

private:

    /**
     * Каждое активное соединение в нашем приложение описывается структурой conn_info.
     * fd - файловый дескриптор сокета.
     * type - описывает состояние в котором находится сокет - ждет accept, read или write.
     */
    typedef struct conn_info {
        int fd;
        unsigned type;
    } conn_info;

    enum {
        ACCEPT,
        READ,
        WRITE,
    };

    // Буфер для соединений.
    conn_info conns[MAX_CONNECTIONS];

    // Для каждого возможного соединения инициализируем буфер для чтения/записи.
    char bufs[MAX_CONNECTIONS][MAX_MESSAGE_LEN];


    int group_id = 1337;

    struct io_uring *ring = new io_uring();
    QTextEdit* text_edit = nullptr;
    int sock_listen_fd = 0;
    int portno = 8080;

    struct sockaddr_in serv_addr, client_addr;
    socklen_t client_len;

    int msec_check = 500;

    int msec_delay_answer = 5000;


    void add_accept(int fd, struct sockaddr *client_addr, socklen_t *client_len);

    void add_socket_read(int fd, size_t size);

    void add_socket_write(int fd, size_t size);

    void printServer(QString text);
};

#endif // SERVERTCP_H
