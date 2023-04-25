#include "tcp_server.h"
#include <QDebug>
#include <QTimer>
#include <QApplication>

TcpServer::TcpServer(QTextEdit* te, int port)
    : text_edit(te), portno(port)
{

}



bool TcpServer::startServer()
{
    // some variables we need
//    int portno = 8080;
    //strtol(argv[1], NULL, 10);
    client_len = sizeof(client_addr);

    // setup socket
    int sock_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    const int val = 1;
    setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(portno);
    serv_addr.sin_addr.s_addr = INADDR_ANY;

    // bind and listen
    if (bind(sock_listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printServer("Error binding socket...\n");
        return false;
        //        exit(1);
    }
    if (listen(sock_listen_fd, BACKLOG) < 0) {
        printServer("Error listening on socket...\n");
        return false;
        //        exit(1);
    }
    printServer( QString("io_uring echo server listening for connections on port: %1\n").arg(portno) );

    // initialize io_uring
    struct io_uring_params params;

    memset(&params, 0, sizeof(params));

    if (io_uring_queue_init_params(2048, ring, &params) < 0) {
        printServer("io_uring_init_failed...\n");
        return false;
        //        exit(1);
    }

    // check if IORING_FEAT_FAST_POLL is supported
    if (!(params.features & IORING_FEAT_FAST_POLL)) {
        printServer("IORING_FEAT_FAST_POLL not available in the kernel, quiting...\n");
        return false;
        //        exit(0);
    }

    // check if buffer selection is supported
    struct io_uring_probe *probe;
    probe = io_uring_get_probe_ring(ring);
    if (!probe || !io_uring_opcode_supported(probe, IORING_OP_PROVIDE_BUFFERS)) {
        printServer("Buffer select not supported, skipping...\n");
        return false;
        //        exit(0);
    }
    //    free(probe);

    // register buffers for buffer selection
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;

    sqe = io_uring_get_sqe(ring);
    io_uring_prep_provide_buffers(sqe, bufs, MAX_MESSAGE_LEN, BUFFERS_COUNT, group_id, 0);

    io_uring_submit(ring);
    io_uring_wait_cqe(ring, &cqe);
    if (cqe->res < 0) {
        printServer( QString("cqe->res = %1\n").arg(cqe->res) );
        return false;
        //        exit(1);
    }
    io_uring_cqe_seen(ring, cqe);

    // add first accept SQE to monitor for new incoming connections
    add_accept(sock_listen_fd, (struct sockaddr *)&client_addr, &client_len, 0);

    QTimer::singleShot(500, this, SLOT(checkMessage()));

    return true;
}



bool TcpServer::checkMessage()
{
//    qDebug() << "checkMessage()";
    QString message;

    io_uring_submit_and_wait(ring, 1);
    struct io_uring_cqe *cqe;
    unsigned head;
    unsigned count = 0;

    // go through all CQEs
    io_uring_for_each_cqe(ring, head, cqe)
    {
        ++count;
        struct conn_info conn_i;
        memcpy(&conn_i, &cqe->user_data, sizeof(conn_i));

        int type = conn_i.type;
        if (cqe->res == -ENOBUFS)
        {
            message = "bufs in automatic buffer selection empty, this should not happen...\n";
            printServer(message);
            fprintf(stdout, message.toStdString().c_str() );
            fflush(stdout);
            return false;
        }

        switch (type) {
        case PROV_BUF:
            if (cqe->res < 0)
            {
                printServer( QString("cqe->res = %1\n").arg(cqe->res) );
                return false;
            }
            break;

        case ACCEPT:
        {
            int sock_conn_fd = cqe->res;
            // only read when there is no error, >= 0
            if (sock_conn_fd >= 0) {
                add_socket_read(sock_conn_fd, group_id, MAX_MESSAGE_LEN, IOSQE_BUFFER_SELECT);
            }

            // new connected client; read data from socket and re-add accept to monitor for new connections
            add_accept(sock_listen_fd, (struct sockaddr *)&client_addr, &client_len, 0);
        }
            break;

        case READ:
        {
            qDebug() << "Have message";

            int bytes_read = cqe->res;
            int bid = cqe->flags >> 16;
            if (cqe->res <= 0) {
                // read failed, re-add the buffer
                add_provide_buf(bid, group_id);
                // connection closed or error
                //                    close(conn_i.fd);
            }
            else
            {
                qDebug() << bufs[0];
                qDebug() << conn_i.fd << "  " << bid << "  " << bytes_read;
                // bytes have been read into bufs, now add write to socket sqe
                add_socket_write(conn_i.fd, bid, bytes_read, 0);
                io_uring_cq_advance(ring, count);
                QTimer::singleShot(msec_delay_answer, this, SLOT(checkMessage()));
                return true;
            }
        }
            break;

        case WRITE:

            qDebug() << "Write message";
            // write has been completed, first re-add the buffer
            add_provide_buf(conn_i.bid, group_id);
            // add a new read for the existing connection
            add_socket_read(conn_i.fd, group_id, MAX_MESSAGE_LEN, IOSQE_BUFFER_SELECT);
            break;
        }
    }

    io_uring_cq_advance(ring, count);

    QTimer::singleShot(msec_check, this, SLOT(checkMessage()));
    return true;
}



void TcpServer::add_accept(int fd, struct sockaddr *client_addr, socklen_t *client_len, unsigned flags) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_accept(sqe, fd, client_addr, client_len, 0);
    io_uring_sqe_set_flags(sqe, flags);

    conn_info conn_i = {
        .fd = __u32(fd),
        .type = ACCEPT,
    };
    memcpy(&sqe->user_data, &conn_i, sizeof(conn_i));
}

void TcpServer::add_socket_read(int fd, unsigned gid, size_t message_size, unsigned flags) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_recv(sqe, fd, NULL, message_size, 0);
    io_uring_sqe_set_flags(sqe, flags);
    sqe->buf_group = gid;

    conn_info conn_i = {
        .fd = __u32(fd),
        .type = READ,
    };
    memcpy(&sqe->user_data, &conn_i, sizeof(conn_i));
}

void TcpServer::add_socket_write(int fd, __u16 bid, size_t message_size, unsigned flags) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_send(sqe, fd, &bufs[bid], message_size, 0);
    io_uring_sqe_set_flags(sqe, flags);

    conn_info conn_i = {
        .fd = __u32(fd),
        .type = WRITE,
        .bid = bid,
    };
    memcpy(&sqe->user_data, &conn_i, sizeof(conn_i));
}

void TcpServer::add_provide_buf(__u16 bid, unsigned gid) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_provide_buffers(sqe, bufs[bid], MAX_MESSAGE_LEN, 1, gid, bid);

    conn_info conn_i = {
        .fd = 0,
        .type = PROV_BUF,
    };
    memcpy(&sqe->user_data, &conn_i, sizeof(conn_i));
}


void TcpServer::printServer(QString text)
{
    qDebug() << text;
    if(text_edit == nullptr)
        return;

    text.replace("\n", "<br>");
    text_edit->insertHtml(text);
}

