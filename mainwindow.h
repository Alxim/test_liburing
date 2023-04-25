#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include "tcp_server.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent* ce);

private:
    Ui::MainWindow *ui;
    TcpServer* tcp_server = nullptr;

    int server_port = 8080;

    bool startServer();

    void printServer(QString text);
};
#endif // MAINWINDOW_H
