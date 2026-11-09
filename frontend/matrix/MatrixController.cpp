// OBS Matrix Studio: native multi-RTMP controller (GPL-2.0-or-later).
#include "MatrixController.hpp"

#include <utility/audio-encoders.hpp>
#include <obs-frontend-api.h>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>

#include <algorithm>
#include <memory>
#include <utility>

namespace {
struct SourceList {
    QJsonArray sources;
};

bool collectAudioSources(void *data, obs_source_t *source)
{
    if (!(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO))
        return true;

    auto *list = static_cast<SourceList *>(data);
    QJsonObject item;
    item["uuid"] = QString::fromUtf8(obs_source_get_uuid(source));
    item["name"] = QString::fromUtf8(obs_source_get_name(source));
    item["mixers"] = int(obs_source_get_audio_mixers(source) & 63u);
    list->sources.append(item);
    return true;
}

bool isAllowedServer(const QString &server)
{
    return server.startsWith("rtmp://", Qt::CaseInsensitive) ||
           server.startsWith("rtmps://", Qt::CaseInsensitive);
}
}

MatrixController::MatrixController(QObject *parent) : QObject(parent)
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Giantturning", "OBSMatrixStudio");
    nextOutputId = qMax(quint64(9), settings.value("nextOutputId", 9).toULongLong());

    // Keep previously saved outputs; initialize eight destinations on a new install.
    QStringList ids = settings.childGroups();
    if (!settings.value("outputsInitialized", false).toBool()) {
        for (int i = 1; i <= 8; ++i) {
            const QString id = QString("rtmp-%1").arg(i, 2, 10, QLatin1Char('0'));
            if (!ids.contains(id))
                ids.append(id);
        }
        settings.setValue("outputsInitialized", true);
    }
    ids.removeDuplicates();
    std::sort(ids.begin(), ids.end(), [](const QString &a, const QString &b) {
        return a.mid(5).toInt() < b.mid(5).toInt();
    });

    for (const QString &id : ids) {
        if (!id.startsWith("rtmp-") || destinations.size() >= maxOutputs)
            continue;
        bool ok = false;
        const unsigned long long numericId = id.mid(5).toULongLong(&ok);
        if (!ok || numericId < 1)
            continue;
        nextOutputId = qMax(nextOutputId, quint64(numericId + 1));
        auto d = std::make_unique<Destination>();
        d->id = id;
        settings.beginGroup(id);
        d->name = settings.value("name", QString("Output %1").arg(numericId)).toString();
        d->server = settings.value("server").toString();
        d->key = settings.value("key").toString();
        d->audioMix = qBound(0, settings.value("audioMix", int((numericId - 1) % 16)).toInt(), 15);
        d->videoSourceUuid = settings.value("videoSourceUuid").toString();
        d->group = settings.value("group", "default").toString();
        d->backupServer = settings.value("backupServer").toString();
        d->backupKey = settings.value("backupKey").toString();
        d->failoverEnabled = settings.value("failoverEnabled", false).toBool();
        d->videoBitrate = qBound(500, settings.value("videoBitrate", 3500).toInt(), 50000);
        d->audioBitrate = qBound(64, settings.value("audioBitrate", 160).toInt(), 320);
        settings.endGroup();
        destinations.push_back(std::move(d));
    }
    save();
}

MatrixController::~MatrixController()
{
    for (auto &destination : destinations) {
        Destination &d = *destination;
        if (d.output && obs_output_active(d.output))
            obs_output_stop(d.output);
        release(d);
    }
}

MatrixController::Destination *MatrixController::find(const QString &id)
{
    for (const auto &destination : destinations)
        if (destination->id == id)
            return destination.get();
    return nullptr;
}

const MatrixController::Destination *MatrixController::find(const QString &id) const
{
    for (const auto &destination : destinations)
        if (destination->id == id)
            return destination.get();
    return nullptr;
}

bool MatrixController::addOutput(QString &id, QString &error)
{
    if (destinations.size() >= maxOutputs) {
        error = "Maximum of 64 outputs reached";
        return false;
    }
    auto d = std::make_unique<Destination>();
    d->id = QString("rtmp-%1").arg(nextOutputId++, 2, 10, QLatin1Char('0'));
    d->name = QString("Output %1").arg(destinations.size() + 1);
    d->audioMix = int(destinations.size() % 16);
    id = d->id;
    destinations.push_back(std::move(d));
    save();
    return true;
}

bool MatrixController::removeOutput(const QString &id, QString &error)
{
    const auto it = std::find_if(destinations.begin(), destinations.end(),
                                 [&id](const auto &d) { return d->id == id; });
    if (it == destinations.end()) {
        error = "Unknown output";
        return false;
    }
    if ((*it)->output && obs_output_active((*it)->output)) {
        error = "Stop the output before removing it";
        return false;
    }
    release(**it);
    destinations.erase(it);
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Giantturning", "OBSMatrixStudio");
    settings.remove(id);
    save();
    return true;
}

void MatrixController::release(Destination &d)
{
    // libobs reference-counts output/encoder/service attachment.
    if (d.output) {
        obs_output_release(d.output);
        d.output = nullptr;
    }
    if (d.video) {
        obs_encoder_release(d.video);
        d.video = nullptr;
    }
    if (d.audio) {
        obs_encoder_release(d.audio);
        d.audio = nullptr;
    }
    if (d.service) {
        obs_service_release(d.service);
        d.service = nullptr;
    }
    if (d.view) {
        obs_view_remove(d.view);
        obs_view_destroy(d.view);
        d.view = nullptr;
    }
}

void MatrixController::save() const
{
    // The per-user OBSMatrixStudio.ini file contains stream keys.
    // Do not share this file; API responses never include the keys.
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Giantturning", "OBSMatrixStudio");
    settings.setValue("nextOutputId", qulonglong(nextOutputId));
    for (const auto &destination : destinations) {
        const Destination &d = *destination;
        settings.beginGroup(d.id);
        settings.setValue("name", d.name);
        settings.setValue("server", d.server);
        settings.setValue("key", d.key);
        settings.setValue("audioMix", d.audioMix);
        settings.setValue("videoBitrate", d.videoBitrate);
        settings.setValue("audioBitrate", d.audioBitrate);
        settings.setValue("videoSourceUuid", d.videoSourceUuid);
        settings.setValue("group", d.group);
        settings.setValue("backupServer", d.backupServer);
        settings.setValue("backupKey", d.backupKey);
        settings.setValue("failoverEnabled", d.failoverEnabled);
        settings.endGroup();
    }
    settings.sync();
}

QJsonArray MatrixController::outputs() const
{
    QJsonArray result;
    for (const auto &destination : destinations) {
        const Destination &d = *destination;
        QJsonObject item;
        item["id"] = d.id;
        item["name"] = d.name;
        item["server"] = d.server;
        item["keyConfigured"] = !d.key.isEmpty();
        item["audioMix"] = d.audioMix + 1;
        item["videoBitrate"] = d.videoBitrate;
        item["audioBitrate"] = d.audioBitrate;
        item["videoSourceUuid"] = d.videoSourceUuid;
        item["group"] = d.group;
        item["backupServer"] = d.backupServer;
        item["backupKeyConfigured"] = !d.backupKey.isEmpty();
        item["failoverEnabled"] = d.failoverEnabled;
        item["backupActive"] = d.backupActive;
        item["desiredActive"] = d.desiredActive;
        item["bitrateKbps"] = d.lastKbps;
        item["connectTimeMs"] = d.output && obs_output_active(d.output) ? obs_output_get_connect_time_ms(d.output) : 0;
        item["totalFrames"] = d.output ? obs_output_get_total_frames(d.output) : 0;
        item["active"] = d.output && obs_output_active(d.output);
        item["reconnecting"] = d.output && obs_output_reconnecting(d.output);
        item["bytesSent"] = d.output ? double(obs_output_get_total_bytes(d.output)) : 0.0;
        item["droppedFrames"] = d.output ? obs_output_get_frames_dropped(d.output) : 0;
        item["lastError"] = d.lastError;
        result.append(item);
    }
    return result;
}

QJsonArray MatrixController::sources() const
{
    SourceList list;
    obs_enum_sources(collectAudioSources, &list);
    return list.sources;
}

QJsonObject MatrixController::status() const
{
    return QJsonObject{{"mainStreaming", obs_frontend_streaming_active()},
                       {"matrixOutputs", outputs()},
                       {"audioSources", sources()},
                       {"videoScenes", scenes()},
                       {"groups", groups()},
                       {"audioBusCount", 16}};
}

bool MatrixController::configure(const QString &id, const QJsonObject &changes, QString &error)
{
    Destination *d = find(id);
    if (!d) {
        error = "Unknown output";
        return false;
    }
    if (d->output && obs_output_active(d->output)) {
        error = "Stop this output before changing its configuration";
        return false;
    }

    // Validate everything before applying any field (atomic configuration update).
    const QString server = changes.contains("server") ? changes.value("server").toString() : d->server;
    const QString key = changes.contains("key") ? changes.value("key").toString() : d->key;
    const QString name = changes.contains("name") ? changes.value("name").toString() : d->name;
    const int mix = changes.contains("audioMix") ? changes.value("audioMix").toInt(-1) : d->audioMix + 1;
    const int videoBitrate =
        changes.contains("videoBitrate") ? changes.value("videoBitrate").toInt(-1) : d->videoBitrate;
    const int audioBitrate =
        changes.contains("audioBitrate") ? changes.value("audioBitrate").toInt(-1) : d->audioBitrate;
    const QString scene = changes.contains("videoSourceUuid") ? changes.value("videoSourceUuid").toString() : d->videoSourceUuid;
    const QString group = changes.contains("group") ? changes.value("group").toString() : d->group;
    const QString backupServer = changes.contains("backupServer") ? changes.value("backupServer").toString() : d->backupServer;
    const QString backupKey = changes.contains("backupKey") ? changes.value("backupKey").toString() : d->backupKey;
    const bool failover = changes.contains("failoverEnabled") ? changes.value("failoverEnabled").toBool() : d->failoverEnabled;
    if (!scene.isEmpty()) {
        bool found = false;
        for (const auto &value : scenes()) {
            if (value.toObject().value("uuid").toString() == scene) {
                found = true;
                break;
            }
        }
        if (!found) {
            error = "Scene not found";
            return false;
        }
    }

    if ((!server.isEmpty() && !isAllowedServer(server)) || server.size() > 2048 ||
        key.size() > 2048 || name.trimmed().isEmpty() || name.size() > 128 ||
        mix < 1 || mix > 16 || videoBitrate < 500 || videoBitrate > 50000 ||
        audioBitrate < 64 || audioBitrate > 320 || group.isEmpty() || group.size() > 64 ||
        (!backupServer.isEmpty() && !isAllowedServer(backupServer)) || backupServer.size() > 2048 ||
        backupKey.size() > 2048) {
        error = "Invalid configuration (RTMP(S) URL, name, audioBus 1-16 or bitrate)";
        return false;
    }

    d->server = server;
    d->key = key;
    d->name = name;
    d->audioMix = mix - 1;
    d->videoBitrate = videoBitrate;
    d->audioBitrate = audioBitrate;
    d->videoSourceUuid = scene;
    d->group = group;
    d->backupServer = backupServer;
    d->backupKey = backupKey;
    d->failoverEnabled = failover;
    d->lastError.clear();
    save();
    return true;
}

bool MatrixController::startAttempt(Destination &d, bool useBackup, QString &error)
{
    if (!obs_get_video() || !obs_get_audio()) {
        error = "OBS audio/video is not initialized";
        return false;
    }
    const QString server = useBackup ? d.backupServer : d.server;
    const QString key = useBackup ? d.backupKey : d.key;
    if (!isAllowedServer(server) || key.isEmpty()) {
        error = "RTMP(S) server or key is missing";
        return false;
    }

    release(d);
    const QString prefix = QString("matrix_%1").arg(d.id);
    video_t *videoContext = obs_get_video();
    if (!d.videoSourceUuid.isEmpty()) {
        obs_source_t *sceneSource = obs_get_source_by_uuid(d.videoSourceUuid.toUtf8().constData());
        if (!sceneSource || !obs_scene_from_source(sceneSource)) {
            if (sceneSource)
                obs_source_release(sceneSource);
            error = "Selected video scene no longer exists";
            return false;
        }
        d.view = obs_view_create();
        if (d.view) {
            obs_view_set_source(d.view, 0, sceneSource);
            videoContext = obs_view_add(d.view);
        }
        obs_source_release(sceneSource);
        if (!d.view || !videoContext) {
            error = "Could not create independent OBS video view";
            release(d);
            return false;
        }
    }

    obs_data_t *videoSettings = obs_data_create();
    obs_data_set_string(videoSettings, "rate_control", "CBR");
    obs_data_set_int(videoSettings, "bitrate", d.videoBitrate);
    obs_data_set_int(videoSettings, "keyint_sec", 2);
    obs_data_set_string(videoSettings, "preset", "veryfast");

    // Encoder sharing is safe only when video settings and video context match.
    for (const auto &other : destinations) {
        if (other.get() == &d || !other->video || !obs_encoder_active(other->video))
            continue;
        if (other->videoSourceUuid == d.videoSourceUuid && other->videoBitrate == d.videoBitrate &&
            other->video && other->videoSourceUuid.isEmpty()) {
            d.video = obs_encoder_get_ref(other->video);
            break;
        }
    }
    if (!d.video)
        d.video = obs_video_encoder_create("obs_x264", (prefix + "_video").toUtf8().constData(),
                                           videoSettings, nullptr);
    obs_data_release(videoSettings);

    const char *aacId = GetSimpleAACEncoderForBitrate(d.audioBitrate);
    if (aacId) {
        obs_data_t *audioSettings = obs_data_create();
        obs_data_set_int(audioSettings, "bitrate", d.audioBitrate);
        d.audio = obs_audio_encoder_create(aacId, (prefix + "_audio").toUtf8().constData(),
                                           audioSettings, size_t(d.audioMix), nullptr);
        obs_data_release(audioSettings);
    }

    obs_data_t *serviceSettings = obs_data_create();
    obs_data_set_string(serviceSettings, "server", server.toUtf8().constData());
    obs_data_set_string(serviceSettings, "key", key.toUtf8().constData());
    d.service = obs_service_create("rtmp_custom", (prefix + "_service").toUtf8().constData(),
                                    serviceSettings, nullptr);
    obs_data_release(serviceSettings);
    d.output = obs_output_create("rtmp_output", (prefix + "_output").toUtf8().constData(),
                                  nullptr, nullptr);

    if (!d.video || !d.audio || !d.service || !d.output) {
        error = "Required OBS RTMP, x264 or AAC encoder unavailable";
        release(d);
        d.lastError = error;
        return false;
    }

    if (!obs_encoder_video(d.video))
        obs_encoder_set_video(d.video, videoContext);
    obs_encoder_set_audio(d.audio, obs_get_audio());
    obs_output_set_video_encoder(d.output, d.video);
    obs_output_set_audio_encoder(d.output, d.audio, 0);
    obs_output_set_service(d.output, d.service);
    obs_output_set_reconnect_settings(d.output, 20, 3);

    if (!obs_output_start(d.output)) {
        const char *last = obs_output_get_last_error(d.output);
        error = last && *last ? QString::fromUtf8(last) : QString("RTMP output start failed");
        d.lastError = error;
        release(d);
        return false;
    }
    d.lastError.clear();
    d.backupActive = useBackup;
    d.failureCount = 0;
    d.lastBytes = 0;
    return true;
}

bool MatrixController::start(const QString &id, QString &error)
{
    Destination *d = find(id);
    if (!d) {
        error = "Unknown output";
        return false;
    }
    if (d->output && obs_output_active(d->output)) {
        error = "Already active";
        return false;
    }
    d->desiredActive = true;
    d->backupActive = false;
    d->failureCount = 0;
    if (startAttempt(*d, false, error))
        return true;
    if (d->failoverEnabled && startAttempt(*d, true, error))
        return true;
    d->desiredActive = false;
    return false;
}

bool MatrixController::stop(const QString &id, QString &error)
{
    Destination *d = find(id);
    if (!d) {
        error = "Unknown output";
        return false;
    }
    if (!d->output || !obs_output_active(d->output)) {
        error = "Output is not active";
        return false;
    }
    d->desiredActive = false;
    obs_output_stop(d->output);
    return true;
}

bool MatrixController::routeSource(const QString &uuid, int mixers, QString &error)
{
    if (mixers < 0 || mixers > 65535) {
        error = "Mixer mask must be 0..65535";
        return false;
    }
    obs_source_t *source = obs_get_source_by_uuid(uuid.toUtf8().constData());
    if (!source) {
        error = "Audio source not found";
        return false;
    }
    if (!(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)) {
        error = "Source has no audio";
        obs_source_release(source);
        return false;
    }
    obs_source_set_audio_mixers(source, uint32_t(mixers));
    obs_source_release(source);
    return true;
}


namespace {
struct SceneList {
    QJsonArray scenes;
};
bool collectScenes(void *param, obs_source_t *source)
{
    auto *data = static_cast<SceneList *>(param);
    QJsonObject scene;
    scene["uuid"] = QString::fromUtf8(obs_source_get_uuid(source));
    scene["name"] = QString::fromUtf8(obs_source_get_name(source));
    data->scenes.append(scene);
    return true;
}
}

QJsonArray MatrixController::scenes() const
{
    SceneList list;
    obs_enum_scenes(collectScenes, &list);
    return list.scenes;
}

QJsonArray MatrixController::groups() const
{
    QStringList names;
    for (const auto &destination : destinations) {
        if (!names.contains(destination->group))
            names.append(destination->group);
    }
    QJsonArray result;
    for (const auto &name : names)
        result.append(name);
    return result;
}

QJsonObject MatrixController::startGroup(const QString &group)
{
    QJsonArray started, errors;
    for (const auto &dest : destinations) {
        if (dest->group != group)
            continue;
        QString error;
        if (dest->output && obs_output_active(dest->output))
            continue;
        if (start(dest->id, error)) {
            started.append(dest->id);
        } else {
            errors.append(QJsonObject{{"id", dest->id}, {"error", error}});
        }
    }
    return QJsonObject{{"started", started}, {"errors", errors}};
}

QJsonObject MatrixController::stopGroup(const QString &group)
{
    QJsonArray stopped, errors;
    for (const auto &dest : destinations) {
        if (dest->group != group)
            continue;
        QString error;
        if (!dest->output || !obs_output_active(dest->output)) {
            dest->desiredActive = false;
            continue;
        }
        if (stop(dest->id, error)) {
            stopped.append(dest->id);
        } else {
            errors.append(QJsonObject{{"id", dest->id}, {"error", error}});
        }
    }
    return QJsonObject{{"stopped", stopped}, {"errors", errors}};
}

QJsonArray MatrixController::presets() const
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Giantturning", "OBSMatrixStudio");
    settings.beginGroup("Presets");
    const QStringList names = settings.childKeys();
    QJsonArray result;
    for (const QString &name : names)
        result.append(name);
    return result;
}

bool MatrixController::savePreset(const QString &name, QString &error)
{
    if (name.isEmpty() || name.size() > 64 || name.contains('/')) {
        error = "Invalid preset name";
        return false;
    }
    QJsonArray outputList;
    for (const auto &dest : destinations) {
        QJsonObject o{{"id", dest->id}, {"name", dest->name}, {"server", dest->server},
                      {"audioMix", dest->audioMix + 1}, {"videoBitrate", dest->videoBitrate},
                      {"audioBitrate", dest->audioBitrate}, {"videoSourceUuid", dest->videoSourceUuid},
                      {"group", dest->group}, {"backupServer", dest->backupServer},
                      {"failoverEnabled", dest->failoverEnabled}};
        outputList.append(o);
    }
    // Presets do not duplicate sensitive stream keys.
    const QJsonObject preset{{"outputs", outputList}, {"audioSources", sources()}};
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Giantturning", "OBSMatrixStudio");
    settings.beginGroup("Presets");
    settings.setValue(name, QString::fromUtf8(QJsonDocument(preset).toJson(QJsonDocument::Compact)));
    settings.sync();
    return true;
}

bool MatrixController::loadPreset(const QString &name, QString &error)
{
    if (anyActive()) {
        error = "Stop active outputs before loading a preset";
        return false;
    }
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Giantturning", "OBSMatrixStudio");
    settings.beginGroup("Presets");
    const QByteArray bytes = settings.value(name).toString().toUtf8();
    if (bytes.isEmpty()) {
        error = "Preset not found";
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject() || !doc.object().value("outputs").isArray()) {
        error = "Invalid preset";
        return false;
    }
    const QJsonArray list = doc.object().value("outputs").toArray();
    // Ensure the referenced scenes and all configurations are valid before applying them.
    for (const auto &value : list) {
        const QJsonObject config = value.toObject();
        if (!find(config.value("id").toString())) {
            error = "Preset references a removed output";
            return false;
        }
    }
    for (const auto &value : list) {
        const QJsonObject config = value.toObject();
        if (!configure(config.value("id").toString(), config, error))
            return false;
    }
    for (const auto &value : doc.object().value("audioSources").toArray()) {
        const QJsonObject source = value.toObject();
        QString routeError;
        routeSource(source.value("uuid").toString(), source.value("mixers").toInt(), routeError);
    }
    return true;
}

bool MatrixController::deletePreset(const QString &name, QString &error)
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Giantturning", "OBSMatrixStudio");
    settings.beginGroup("Presets");
    if (!settings.contains(name)) {
        error = "Preset not found";
        return false;
    }
    settings.remove(name);
    return true;
}

bool MatrixController::anyActive() const
{
    for (const auto &dest : destinations)
        if (dest->output && obs_output_active(dest->output))
            return true;
    return false;
}

void MatrixController::collectTelemetry()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastTelemetryMs > 0) {
        const double interval = double(now - lastTelemetryMs) / 1000.0;
        if (interval > 0.0) {
            for (const auto &dest : destinations) {
                const quint64 bytes = dest->output ? obs_output_get_total_bytes(dest->output) : 0;
                dest->lastKbps = bytes >= dest->lastBytes && dest->output && obs_output_active(dest->output)
                    ? (double(bytes - dest->lastBytes) * 8.0) / (interval * 1000.0) : 0.0;
                dest->lastBytes = bytes;
            }
        }
    }
    lastTelemetryMs = now;
    tickFailover();
}

void MatrixController::tickFailover()
{
    for (const auto &dest : destinations) {
        Destination &d = *dest;
        if (!d.desiredActive || !d.failoverEnabled || d.backupActive ||
            !isAllowedServer(d.backupServer) || d.backupKey.isEmpty())
            continue;
        if (d.output && (obs_output_active(d.output) || obs_output_reconnecting(d.output)))
            continue;
        if (d.failureCount >= 1)
            continue;
        d.failureCount++;
        QString error;
        if (!startAttempt(d, true, error))
            d.lastError = "Backup failed: " + error;
    }
}
