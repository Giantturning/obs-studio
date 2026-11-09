// OBS Matrix Studio: native multi-RTMP controller (GPL-2.0-or-later).
#pragma once

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <memory>
#include <vector>

#include <obs.h>

class MatrixController : public QObject {
public:
    explicit MatrixController(QObject *parent = nullptr);
    ~MatrixController() override;

    QJsonArray outputs() const;
    QJsonArray sources() const;
    QJsonArray scenes() const;
    QJsonArray groups() const;
    QJsonArray presets() const;
    QJsonObject status() const;
    bool addOutput(QString &id, QString &error);
    bool removeOutput(const QString &id, QString &error);
    bool configure(const QString &id, const QJsonObject &changes, QString &error);
    bool start(const QString &id, QString &error);
    bool stop(const QString &id, QString &error);
    bool routeSource(const QString &uuid, int mixers, QString &error);
    QJsonObject startGroup(const QString &group);
    QJsonObject stopGroup(const QString &group);
    bool savePreset(const QString &name, QString &error);
    bool loadPreset(const QString &name, QString &error);
    bool deletePreset(const QString &name, QString &error);
    void collectTelemetry();

private:
    struct Destination {
        QString id;
        QString name;
        QString server;
        QString key;
        int audioMix = 0; // OBS mixer index (0..15); fork has 16 independent buses.
        int videoBitrate = 3500; // kbps
        int audioBitrate = 160; // kbps
        QString videoSourceUuid; // empty means the main OBS program video
        QString group = "default";
        QString backupServer;
        QString backupKey;
        bool failoverEnabled = false;
        bool desiredActive = false;
        bool backupActive = false;
        int failureCount = 0;
        quint64 lastBytes = 0;
        double lastKbps = 0;
        obs_view_t *view = nullptr;
        obs_output_t *output = nullptr;
        obs_encoder_t *video = nullptr;
        obs_encoder_t *audio = nullptr;
        obs_service_t *service = nullptr;
        QString lastError;
    };

    static constexpr size_t maxOutputs = 64;
    std::vector<std::unique_ptr<Destination>> destinations;
    quint64 nextOutputId = 9;
    Destination *find(const QString &id);
    const Destination *find(const QString &id) const;
    void release(Destination &d);
    void save() const;
    void tickFailover();
    bool startAttempt(Destination &d, bool useBackup, QString &error);
    bool anyActive() const;
    qint64 lastTelemetryMs = 0;
};
