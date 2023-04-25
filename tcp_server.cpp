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

    /**
     * Создаем серверный сокет и начинаем прослушивать порт.
     * Обратите внимание что при создании сокета мы НЕ УСТАНАВЛИВАЕМ флаг O_NON_BLOCK,
     * но при этом все чтения и записи не будут блокировать приложение.
     * Происходит это потому, что io_uring спокойно превращает операции над блокирующими сокетами в non-block системные вызовы.
     */

    client_len = sizeof(client_addr);

    sock_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    const int val = 1;
    setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(portno);
    serv_addr.sin_addr.s_addr = INADDR_ANY;

    assert(bind(sock_listen_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) >= 0);
    assert(listen(sock_listen_fd, BACKLOG) >= 0);

    /**
     * Создаем инстанс io_uring, не используем никаких кастомных опций.
     * Емкость очередей SQ и CQ указываем как 4096 вхождений.
     */
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    assert(io_uring_queue_init_params(4096, ring, &params) >= 0);

    /**
     * Проверяем наличие фичи IORING_FEAT_FAST_POLL.
     * Для нас это наиболее "перформящая" фича в данном приложении,
     * фактически это встроенный в io_uring движок для поллинга I/O.
     */
    if (!(params.features & IORING_FEAT_FAST_POLL)) {
        printf("IORING_FEAT_FAST_POLL not available in the kernel, quiting...\n");
        exit(0);
    }

    /**
     * Добавляем в SQ первую операцию - слушаем сокет сервера для приема входящих соединений.
     */
    add_accept(sock_listen_fd, (struct sockaddr *) &client_addr, &client_len);

    QTimer::singleShot(500, this, SLOT(checkMessage()));

    return true;
}



bool TcpServer::checkMessage()
{
    struct io_uring_cqe *cqe;
    int ret;

    /**
     * Сабмитим все SQE которые были добавлены на предыдущей итерации.
     */
    io_uring_submit(ring);

    /**
     * Ждем когда в CQ буфере появится хотя бы одно CQE.
     */
    ret = io_uring_wait_cqe(ring, &cqe);
    assert(ret == 0);

    /**
     * Положим все "готовые" CQE в буфер cqes.
     */
    struct io_uring_cqe *cqes[BACKLOG];
    int cqe_count = io_uring_peek_batch_cqe(ring, cqes, sizeof(cqes) / sizeof(cqes[0]));

    for (int i = 0; i < cqe_count; ++i) {
        cqe = cqes[i];

        /**
         * В поле user_data мы заранее положили указатель структуру
         * в которой находится служебная информация по сокету.
         */
        struct conn_info *user_data = (struct conn_info *) io_uring_cqe_get_data(cqe);

        /**
         * Используя тип идентифицируем операцию к которой относится CQE (accept/recv/send).
         */
        unsigned type = user_data->type;

        switch (type) {
        case ACCEPT:
        {
            int sock_conn_fd = cqe->res;

            /**
            * Если появилось новое соединение: добавляем в SQ операцию recv - читаем из клиентского сокета,
            * продолжаем слушать серверный сокет.
            */
            add_socket_read(sock_conn_fd, MAX_MESSAGE_LEN);
            add_accept(sock_listen_fd, (struct sockaddr *) &client_addr, &client_len);
        }
            break;

        case READ:
        {
            int bytes_read = cqe->res;

            /**
                         * В случае чтения из клиентского сокета:
                         * если прочитали 0 байт - закрываем сокет
                         * если чтение успешно: добавляем в SQ операцию send - пересылаем прочитанные данные обратно, на клиент.
                         */
            if (bytes_read <= 0)
            {
                shutdown(user_data->fd, SHUT_RDWR);
            }
            else
            {
                add_socket_write(user_data->fd, bytes_read);
                io_uring_cqe_seen(ring, cqe);
                QTimer::singleShot(msec_delay_answer, this, SLOT(checkMessage()));
                return true;
            }
        }
            break;

        case WRITE:
            /**
            * Запись в клиентский сокет окончена: добавляем в SQ операцию recv - читаем из клиентского сокета.
            */
            add_socket_read(user_data->fd, MAX_MESSAGE_LEN);
            break;

        }
        io_uring_cqe_seen(ring, cqe);
    }

    QTimer::singleShot(msec_check, this, SLOT(checkMessage()));
    return true;
}


/**
 *  Помещаем операцию accept в SQ, fd - дескриптор сокета на котором принимаем соединения.
 */
void TcpServer::add_accept(int fd, struct sockaddr *client_addr, socklen_t *client_len) {
    // Получаем указатель на первый доступный SQE.
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    // Хелпер io_uring_prep_accept помещает в SQE операцию ACCEPT.
    io_uring_prep_accept(sqe, fd, client_addr, client_len, 0);

    // Устанавливаем состояние серверного сокета в ACCEPT.
    conn_info *conn_i = &conns[fd];
    conn_i->fd = fd;
    conn_i->type = ACCEPT;

    // Устанавливаем в поле user_data указатель на socketInfo соответствующий серверному сокету.
    io_uring_sqe_set_data(sqe, conn_i);
}

/**
 *  Помещаем операцию recv в SQ.
 */
void TcpServer::add_socket_read(int fd, size_t size) {
    // Получаем указатель на первый доступный SQE.
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    // Хелпер io_uring_prep_recv помещает в SQE операцию RECV, чтение производится в буфер соответствующий клиентскому сокету.
    io_uring_prep_recv(sqe, fd, &bufs[fd], size, 0);

    // Устанавливаем состояние клиентского сокета в READ.
    conn_info *conn_i = &conns[fd];
    conn_i->fd = fd;
    conn_i->type = READ;

    // Устанавливаем в поле user_data указатель на socketInfo соответствующий клиентскому сокету.
    io_uring_sqe_set_data(sqe, conn_i);
}

/**
 *  Помещаем операцию send в SQ буфер.
 */
void TcpServer::add_socket_write(int fd, size_t size) {
    // Получаем указатель на первый доступный SQE.
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    // Хелпер io_uring_prep_send помещает в SQE операцию SEND, запись производится из буфера соответствующего клиентскому сокету.
    io_uring_prep_send(sqe, fd, &bufs[fd], size, 0);

    // Устанавливаем состояние клиентского сокета в WRITE.
    conn_info *conn_i = &conns[fd];
    conn_i->fd = fd;
    conn_i->type = WRITE;

    // Устанавливаем в поле user_data указатель на socketInfo соответсвующий клиентскому сокету.
    io_uring_sqe_set_data(sqe, conn_i);
}




void TcpServer::printServer(QString text)
{
    qDebug() << text;
    if(text_edit == nullptr)
        return;

    text.replace("\n", "<br>");
    text_edit->insertHtml(text);
}

