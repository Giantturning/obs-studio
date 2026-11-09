// OBS Matrix Studio: native multi-RTMP controller (GPL-2.0-or-later).
#pragma once

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <array>

#include <obs.h>

class MatrixController : public QObject {
public:
    explicit MatrixController(QObject *parent = nullptr);
    ~MatrixController() override;

    QJsonArray outputs() const;
    QJsonArray sources() const;
    QJsonObject status() const;
    bool configure(const QString &id, const QJsonObject &changes, QString &error);
    bool start(const QString &id, QString &error);
    bool stop(const QString &id, QString &error);
    bool routeSource(const QString &uuid, int mixers, QString &error);

private:
    struct Destination {
        QString id;
        QString name;
        QString server;
        QString key;
        int audioMix = 0; // OBS mixer index (0..5).
        int videoBitrate = 3500; // kbps
        int audioBitrate = 160; // kbps
        obs_output_t *output = nullptr;
        obs_encoder_t *video = nullptr;
        obs_encoder_t *audio = nullptr;
        obs_service_t *service = nullptr;
        QString lastError;
    };

    std::array<Destination, 4> destinations;
    Destination *find(const QString &id);
    const Destination *find(const QString &id) const;
    void release(Destination &d);
    void save() const;
};
