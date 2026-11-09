// OBS Matrix Studio: native multi-RTMP controller (GPL-2.0-or-later).
#include "MatrixController.hpp"

#include <utility/audio-encoders.hpp>
#include <obs-frontend-api.h>

#include <QJsonArray>
#include <QSettings>

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
    for (size_t i = 0; i < destinations.size(); ++i) {
        auto &d = destinations[i];
        d.id = QString("rtmp-%1").arg(i + 1, 2, 10, QLatin1Char('0'));
        settings.beginGroup(d.id);
        d.name = settings.value("name", QString("Output %1").arg(i + 1)).toString();
        d.server = settings.value("server").toString();
        d.key = settings.value("key").toString();
        d.audioMix = qBound(0, settings.value("audioMix", int(i < 4 ? i : 0)).toInt(), 5);
        d.videoBitrate = qBound(500, settings.value("videoBitrate", 3500).toInt(), 50000);
        d.audioBitrate = qBound(64, settings.value("audioBitrate", 160).toInt(), 320);
        settings.endGroup();
    }
}

MatrixController::~MatrixController()
{
    for (auto &d : destinations) {
        if (d.output && obs_output_active(d.output))
            obs_output_stop(d.output);
        release(d);
    }
}

MatrixController::Destination *MatrixController::find(const QString &id)
{
    for (auto &d : destinations)
        if (d.id == id)
            return &d;
    return nullptr;
}

const MatrixController::Destination *MatrixController::find(const QString &id) const
{
    for (const auto &d : destinations)
        if (d.id == id)
            return &d;
    return nullptr;
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
}

void MatrixController::save() const
{
    // The per-user OBSMatrixStudio.ini file contains stream keys.
    // Do not share this file; API responses never include the keys.
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Giantturning", "OBSMatrixStudio");
    for (const auto &d : destinations) {
        settings.beginGroup(d.id);
        settings.setValue("name", d.name);
        settings.setValue("server", d.server);
        settings.setValue("key", d.key);
        settings.setValue("audioMix", d.audioMix);
        settings.setValue("videoBitrate", d.videoBitrate);
        settings.setValue("audioBitrate", d.audioBitrate);
        settings.endGroup();
    }
    settings.sync();
}

QJsonArray MatrixController::outputs() const
{
    QJsonArray result;
    for (const auto &d : destinations) {
        QJsonObject item;
        item["id"] = d.id;
        item["name"] = d.name;
        item["server"] = d.server;
        item["keyConfigured"] = !d.key.isEmpty();
        item["audioMix"] = d.audioMix + 1;
        item["videoBitrate"] = d.videoBitrate;
        item["audioBitrate"] = d.audioBitrate;
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
                       {"audioSources", sources()}};
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

    if ((!server.isEmpty() && !isAllowedServer(server)) || server.size() > 2048 ||
        key.size() > 2048 || name.trimmed().isEmpty() || name.size() > 128 ||
        mix < 1 || mix > 6 || videoBitrate < 500 || videoBitrate > 50000 ||
        audioBitrate < 64 || audioBitrate > 320) {
        error = "Invalid configuration (RTMP(S) URL, name, mix 1-6 or bitrate)";
        return false;
    }

    d->server = server;
    d->key = key;
    d->name = name;
    d->audioMix = mix - 1;
    d->videoBitrate = videoBitrate;
    d->audioBitrate = audioBitrate;
    d->lastError.clear();
    save();
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
    if (!isAllowedServer(d->server) || d->key.isEmpty()) {
        error = "Set the RTMP(S) server and stream key first";
        return false;
    }
    if (!obs_get_video() || !obs_get_audio()) {
        error = "OBS audio/video is not initialized";
        return false;
    }

    release(*d);
    const QByteArray idUtf8 = d->id.toUtf8();
    const QString prefix = QString("matrix_%1").arg(d->id);

    obs_data_t *videoSettings = obs_data_create();
    obs_data_set_string(videoSettings, "rate_control", "CBR");
    obs_data_set_int(videoSettings, "bitrate", d->videoBitrate);
    obs_data_set_int(videoSettings, "keyint_sec", 2);
    obs_data_set_string(videoSettings, "preset", "veryfast");
    d->video = obs_video_encoder_create("obs_x264", (prefix + "_video").toUtf8().constData(),
                                        videoSettings, nullptr);
    obs_data_release(videoSettings);

    const char *aacId = GetSimpleAACEncoderForBitrate(d->audioBitrate);
    if (aacId) {
        obs_data_t *audioSettings = obs_data_create();
        obs_data_set_int(audioSettings, "bitrate", d->audioBitrate);
        d->audio = obs_audio_encoder_create(aacId, (prefix + "_audio").toUtf8().constData(),
                                             audioSettings, size_t(d->audioMix), nullptr);
        obs_data_release(audioSettings);
    }

    obs_data_t *serviceSettings = obs_data_create();
    obs_data_set_string(serviceSettings, "server", d->server.toUtf8().constData());
    obs_data_set_string(serviceSettings, "key", d->key.toUtf8().constData());
    d->service = obs_service_create("rtmp_custom", (prefix + "_service").toUtf8().constData(),
                                     serviceSettings, nullptr);
    obs_data_release(serviceSettings);
    d->output = obs_output_create("rtmp_output", (prefix + "_output").toUtf8().constData(),
                                   nullptr, nullptr);

    if (!d->video || !d->audio || !d->service || !d->output) {
        error = "Required OBS RTMP, x264 or AAC encoder is unavailable";
        release(*d);
        d->lastError = error;
        return false;
    }

    obs_encoder_set_video(d->video, obs_get_video());
    obs_encoder_set_audio(d->audio, obs_get_audio());
    obs_output_set_video_encoder(d->output, d->video);
    obs_output_set_audio_encoder(d->output, d->audio, 0);
    obs_output_set_service(d->output, d->service);
    obs_output_set_reconnect_settings(d->output, 20, 3);

    if (!obs_output_start(d->output)) {
        const char *last = obs_output_get_last_error(d->output);
        error = last && *last ? QString::fromUtf8(last) : QString("RTMP output start failed");
        d->lastError = error;
        release(*d);
        return false;
    }

    d->lastError.clear();
    return true;
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
    obs_output_stop(d->output);
    return true;
}

bool MatrixController::routeSource(const QString &uuid, int mixers, QString &error)
{
    if (mixers < 0 || mixers > 63) {
        error = "Mixer mask must be 0..63";
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
