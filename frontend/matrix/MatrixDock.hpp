// OBS Matrix Studio native dock and loopback-only control API (GPL-2.0-or-later).
#pragma once

#include <QDockWidget>
#include <QHash>
#include <QJsonObject>
#include <QSet>

class QTcpServer;
class QTcpSocket;
class QTableWidget;
class QLineEdit;
class QTimer;
class MatrixController;

class MatrixDock : public QDockWidget {
public:
    explicit MatrixDock(QWidget *parent = nullptr);
    ~MatrixDock() override;
private:
    void refresh();
    void populateOutputs();
    void updateRoute(int row, int track, bool selected);
    void acceptConnection();
    void processRequest(QTcpSocket *socket);
    void reply(QTcpSocket *socket, int code, const QJsonObject &data);
    void dispatch(QTcpSocket *socket, const QByteArray &method, const QByteArray &path,
                  const QJsonObject &payload);
    void replyHtml(QTcpSocket *socket);
    void acceptWebSocket();
    void broadcastStatus();
    MatrixController *controller;
    QTableWidget *routesTable;
    QTableWidget *outputsTable;
    QTcpServer *server;
    QTcpServer *websocketServer;
    QSet<QTcpSocket *> websocketClients;
    QTimer *refreshTimer;
    QString apiToken;
    QHash<QTcpSocket *, QByteArray> pending;
    bool refreshing = false;
};
