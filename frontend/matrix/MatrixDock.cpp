// OBS Matrix Studio native dock and loopback-only control API (GPL-2.0-or-later).
#include "MatrixDock.hpp"
#include "MatrixController.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QString generateToken()
{
    QByteArray bytes;
    bytes.reserve(64);
    for (int i = 0; i < 8; ++i) {
        const quint64 n = QRandomGenerator::system()->generate64();
        bytes.append(QByteArray::number(n, 16).rightJustified(16, '0'));
    }
    return QString::fromLatin1(bytes);
}
}

MatrixDock::MatrixDock(QWidget *parent) : QDockWidget("Matrix Control", parent),
    controller(new MatrixController(this)), routesTable(new QTableWidget(this)),
    outputsTable(new QTableWidget(this)), server(new QTcpServer(this)), refreshTimer(new QTimer(this))
{
    setObjectName("matrixControlDock");
    auto *root = new QWidget(this);
    auto *layout = new QVBoxLayout(root);
    layout->addWidget(new QLabel("Audio routing: each source may feed tracks 1–6.", root));

    routesTable->setColumnCount(7);
    routesTable->setHorizontalHeaderLabels({"Audio source", "1", "2", "3", "4", "5", "6"});
    routesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int col = 1; col < 7; ++col)
        routesTable->horizontalHeader()->setSectionResizeMode(col, QHeaderView::ResizeToContents);
    routesTable->verticalHeader()->hide();
    layout->addWidget(routesTable);

    layout->addWidget(new QLabel("Independent RTMP outputs (one audio track per destination):", root));
    outputsTable->setColumnCount(6);
    outputsTable->setHorizontalHeaderLabels({"Name", "RTMP(S) URL", "Stream key", "Audio track", "State", "Control"});
    outputsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    outputsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    outputsTable->verticalHeader()->hide();
    layout->addWidget(outputsTable);
    auto *addOutputButton = new QPushButton("Add RTMP output", root);
    layout->addWidget(addOutputButton);
    connect(addOutputButton, &QPushButton::clicked, this, [this]() {
        QString id;
        QString error;
        if (!controller->addOutput(id, error)) {
            QMessageBox::warning(this, "Matrix Control", error);
            return;
        }
        populateOutputs();
        refresh();
    });


    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Giantturning", "OBSMatrixStudio");
    apiToken = settings.value("apiToken").toString();
    if (apiToken.size() < 32) {
        apiToken = generateToken();
        settings.setValue("apiToken", apiToken);
        settings.sync();
    }
    auto *apiLabel = new QLabel(root);
    apiLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    apiLabel->setWordWrap(true);
    layout->addWidget(apiLabel);
    if (server->listen(QHostAddress::LocalHost, 4456)) {
        apiLabel->setText("Local control API: http://127.0.0.1:4456/api/v1/status"
                          "\nToken in per-user OBSMatrixStudio.ini under apiToken."
                          "\nHeader: Authorization: Bearer <token>");
        connect(server, &QTcpServer::newConnection, this, &MatrixDock::acceptConnection);
    } else {
        apiLabel->setText("API unavailable: " + server->errorString());
    }

    setWidget(root);
    populateOutputs();
    refreshTimer->setInterval(1000);
    connect(refreshTimer, &QTimer::timeout, this, &MatrixDock::refresh);
    refreshTimer->start();
    refresh();
}

MatrixDock::~MatrixDock()
{
    refreshTimer->stop();
    server->close();
    for (QTcpSocket *socket : pending.keys())
        socket->abort();
}

void MatrixDock::populateOutputs()
{
    outputsTable->setRowCount(0);
    outputsTable->setRowCount(controller->outputs().size());
    const QJsonArray configured = controller->outputs();
    for (int row = 0; row < configured.size(); ++row) {
        const QJsonObject output = configured[row].toObject();
        const QString id = output.value("id").toString();
        auto *name = new QLineEdit(output.value("name").toString(), outputsTable);
        auto *url = new QLineEdit(output.value("server").toString(), outputsTable);
        auto *key = new QLineEdit(outputsTable);
        key->setEchoMode(QLineEdit::Password);
        key->setPlaceholderText(output.value("keyConfigured").toBool() ? "Key saved (leave blank to keep)" : "Stream key");
        auto *track = new QSpinBox(outputsTable);
        track->setRange(1, 6);
        track->setValue(output.value("audioMix").toInt());
        auto *state = new QLabel("Stopped", outputsTable);
        auto *button = new QPushButton("Start", outputsTable);
        auto *remove = new QPushButton("Delete", outputsTable);
        auto *control = new QWidget(outputsTable);
        auto *buttons = new QHBoxLayout(control);
        buttons->setContentsMargins(0, 0, 0, 0);
        buttons->addWidget(button);
        buttons->addWidget(remove);
        outputsTable->setCellWidget(row, 0, name);
        outputsTable->setCellWidget(row, 1, url);
        outputsTable->setCellWidget(row, 2, key);
        outputsTable->setCellWidget(row, 3, track);
        outputsTable->setCellWidget(row, 4, state);
        outputsTable->setCellWidget(row, 5, control);
        connect(remove, &QPushButton::clicked, this, [this, id]() {
            QString error;
            if (!controller->removeOutput(id, error)) {
                QMessageBox::warning(this, "Matrix Control", error);
                return;
            }
            populateOutputs();
            refresh();
        });

        connect(button, &QPushButton::clicked, this, [this, id, name, url, key, track, state]() {
            QString error;
            const QJsonArray items = controller->outputs();
            bool active = false;
            for (const auto &item : items)
                if (item.toObject().value("id").toString() == id)
                    active = item.toObject().value("active").toBool();
            if (active) {
                if (!controller->stop(id, error))
                    state->setText(error);
            } else {
                QJsonObject changes{{"name", name->text()}, {"server", url->text()},
                                    {"audioMix", track->value()}};
                if (!key->text().isEmpty())
                    changes["key"] = key->text();
                if (!controller->configure(id, changes, error) ||
                    !controller->start(id, error)) {
                    state->setText(error);
                } else {
                    key->clear();
                }
            }
            refresh();
        });
    }

}

void MatrixDock::refresh()
{
    refreshing = true;
    const QJsonArray sources = controller->sources();
    routesTable->setRowCount(sources.size());
    for (int row = 0; row < sources.size(); ++row) {
        const QJsonObject source = sources[row].toObject();
        auto *name = new QTableWidgetItem(source.value("name").toString());
        name->setFlags(Qt::ItemIsEnabled);
        routesTable->setItem(row, 0, name);
        const int mixers = source.value("mixers").toInt();
        const QString uuid = source.value("uuid").toString();
        for (int col = 1; col <= 6; ++col) {
            auto *check = qobject_cast<QCheckBox *>(routesTable->cellWidget(row, col));
            if (!check || check->property("sourceUuid").toString() != uuid) {
                check = new QCheckBox(routesTable);
                check->setProperty("sourceUuid", uuid);
                routesTable->setCellWidget(row, col, check);
                connect(check, &QCheckBox::toggled, this, [this, row, col](bool checked) {
                    updateRoute(row, col - 1, checked);
                });
            }
            const QSignalBlocker blocker(check);
            check->setChecked((mixers & (1 << (col - 1))) != 0);
        }
    }
    const QJsonArray outputs = controller->outputs();
    for (int row = 0; row < outputs.size(); ++row) {
        const QJsonObject output = outputs[row].toObject();
        auto *state = qobject_cast<QLabel *>(outputsTable->cellWidget(row, 4));
        auto *control = outputsTable->cellWidget(row, 5);
        auto *button = control ? control->findChild<QPushButton *>() : nullptr;
        if (!state || !button)
            continue;
        const bool active = output.value("active").toBool();
        const bool reconnecting = output.value("reconnecting").toBool();
        state->setText(reconnecting ? "Reconnecting" : active ? "LIVE" : "Stopped");
        state->setToolTip(output.value("lastError").toString());
        button->setText(active ? "Stop" : "Start");
    }
    refreshing = false;
}

void MatrixDock::updateRoute(int row, int track, bool selected)
{
    if (refreshing)
        return;
    const QJsonArray sources = controller->sources();
    if (row >= sources.size())
        return;
    const QJsonObject source = sources[row].toObject();
    int mask = source.value("mixers").toInt();
    mask = selected ? mask | (1 << track) : mask & ~(1 << track);
    QString error;
    controller->routeSource(source.value("uuid").toString(), mask, error);
    refresh();
}

void MatrixDock::acceptConnection()
{
    while (server->hasPendingConnections()) {
        QTcpSocket *socket = server->nextPendingConnection();
        pending.insert(socket, {});
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { processRequest(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            pending.remove(socket);
            socket->deleteLater();
        });
    }
}

void MatrixDock::processRequest(QTcpSocket *socket)
{
    QByteArray &buffer = pending[socket];
    buffer += socket->readAll();
    if (buffer.size() > 65536) {
        reply(socket, 413, QJsonObject{{"error", "Request too large"}});
        return;
    }
    const int separator = buffer.indexOf("\r\n\r\n");
    if (separator < 0)
        return;
    const QByteArray header = buffer.left(separator);
    const QList<QByteArray> lines = header.split('\n');
    if (lines.isEmpty())
        return;
    const QList<QByteArray> firstLine = lines[0].trimmed().split(' ');
    if (firstLine.size() != 3) {
        reply(socket, 400, QJsonObject{{"error", "Invalid HTTP request"}});
        return;
    }
    int contentLength = 0;
    bool authenticated = false;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
            bool ok = false;
            contentLength = line.mid(line.indexOf(':') + 1).trimmed().toInt(&ok);
            if (!ok || contentLength < 0 || contentLength > 32768) {
                reply(socket, 413, QJsonObject{{"error", "Invalid content length"}});
                return;
            }
        }
        if (line.left(14).compare("Authorization:", Qt::CaseInsensitive) == 0) {
            const QByteArray presented = line.mid(line.indexOf(':') + 1).trimmed();
            authenticated = presented == ("Bearer " + apiToken.toUtf8());
        }
    }
    if (buffer.size() < separator + 4 + contentLength)
        return;
    if (!authenticated) {
        reply(socket, 401, QJsonObject{{"error", "Authorization required"}});
        return;
    }
    const QByteArray body = buffer.mid(separator + 4, contentLength);
    QJsonObject data;
    if (!body.isEmpty()) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            reply(socket, 400, QJsonObject{{"error", "Invalid JSON object"}});
            return;
        }
        data = document.object();
    }
    dispatch(socket, firstLine[0], firstLine[1].split('?')[0], data);
}

void MatrixDock::reply(QTcpSocket *socket, int code, const QJsonObject &data)
{
    const QByteArray body = QJsonDocument(data).toJson(QJsonDocument::Compact);
    QByteArray response = "HTTP/1.1 " + QByteArray::number(code) +
                          (code == 200 ? " OK\r\n" : " Error\r\n") +
                          "Content-Type: application/json\r\nCache-Control: no-store\r\n"
                          "Connection: close\r\nContent-Length: " +
                          QByteArray::number(body.size()) + "\r\n\r\n" + body;
    socket->write(response);
    socket->disconnectFromHost();
}

void MatrixDock::dispatch(QTcpSocket *socket, const QByteArray &method, const QByteArray &path,
                          const QJsonObject &payload)
{
    if (method == "GET" && path == "/api/v1/status") {
        reply(socket, 200, controller->status());
        return;
    }
    if (method == "GET" && path == "/api/v1/outputs") {
        reply(socket, 200, QJsonObject{{"outputs", controller->outputs()}});
        return;
    }
    if (method == "GET" && path == "/api/v1/matrix") {
        reply(socket, 200, QJsonObject{{"sources", controller->sources()}});
        return;
    }
    if (method == "PUT" && path.startsWith("/api/v1/matrix/")) {
        QString error;
        if (!controller->routeSource(QString::fromUtf8(path.mid(15)),
                                     payload.value("mixers").toInt(-1), error)) {
            reply(socket, 400, QJsonObject{{"error", error}});
        } else {
            reply(socket, 200, QJsonObject{{"ok", true}});
            refresh();
        }
        return;
    }
    if (method == "POST" && path == "/api/v1/outputs") {
        QString id;
        QString error;
        if (!controller->addOutput(id, error)) {
            reply(socket, 400, QJsonObject{{"error", error}});
        } else {
            reply(socket, 200, QJsonObject{{"ok", true}, {"id", id}});
            populateOutputs();
            refresh();
        }
        return;
    }
    if (method == "DELETE" && path.startsWith("/api/v1/outputs/")) {
        const QByteArray id = path.mid(sizeof("/api/v1/outputs/") - 1);
        if (id.isEmpty() || id.contains('/')) {
            reply(socket, 400, QJsonObject{{"error", "Invalid output ID"}});
            return;
        }
        QString error;
        if (!controller->removeOutput(QString::fromUtf8(id), error)) {
            reply(socket, 400, QJsonObject{{"error", error}});
        } else {
            reply(socket, 200, QJsonObject{{"ok", true}});
            populateOutputs();
            refresh();
        }
        return;
    }
    const QList<QByteArray> parts = path.split('/');
    if (parts.size() == 6 && parts[1] == "api" && parts[2] == "v1" &&
        parts[3] == "outputs") {
        QString error;
        const QString id = QString::fromUtf8(parts[4]);
        bool ok = false;
        if (method == "POST" && parts[5] == "start")
            ok = controller->start(id, error);
        else if (method == "POST" && parts[5] == "stop")
            ok = controller->stop(id, error);
        else if (method == "PATCH" && parts[5] == "config")
            ok = controller->configure(id, payload, error);
        else {
            reply(socket, 404, QJsonObject{{"error", "Unknown endpoint"}});
            return;
        }
        reply(socket, ok ? 200 : 400,
              ok ? QJsonObject{{"ok", true}} : QJsonObject{{"error", error}});
        refresh();
        return;
    }
    reply(socket, 404, QJsonObject{{"error", "Unknown endpoint"}});
}
