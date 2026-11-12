// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "MpvCore.h"

#include "ImageSequencePlaylist.h"
#include "ImageSequenceStrategy.h"
#include "WrappedTsProxy.h"

#include <mpv/client.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>
#include <QtGlobal>

#include <cmath>
#include <optional>

namespace {

/**
 * 画面调节项 —— 这张表是照 mpv 自己的选项表抄的，不是凭印象写的。
 *
 * 怎么来的：`mpv --list-options`，把每一项的取值范围和默认值都打出来对着填。
 * 表里没有"色温""颜色增益""梯形校正"这几项，因为 mpv 根本没有对应属性 ——
 * 它们是 DLNA RenderingControl 里的标准名字，但要在这套后端上做出来得自己写
 * shader，那是另一个量级的事。宁可少几项，也不要摆一排按了没反应的滑块。
 *
 * scale 是"我们的整数 × scale = 塞给 mpv 的浮点数"。mpv 那几项 -100~100 的
 * 正好乘以 1；像 sharpen、video-align 这类 0~1 / -1~1 的，就用 0.01 把它换算成
 * 整数来给界面用 —— 界面不需要知道底下是浮点。
 *
 * **这张表里没有"给人看的名字"。** name 是给程序用的中性短名，界面拿
 * `ui_picture_<name>` 去语言文件里查译文（见 MediaPlayer.h 里 PictureControlInfo
 * 那段）。以前这里多一列中文，切到英文界面就会蹦中文。
 */
struct PictureControlSpec
{
    const char *name;         // 短名，界面和 DLNA 那边都用它
    const char *mpvProperty;
    int         min;
    int         max;
    int         neutral;      // 复位到这儿
    double      scale;
};

const PictureControlSpec kPictureControls[] = {
    { "brightness", "brightness",     -100, 100, 0, 1.0  },
    { "contrast",   "contrast",       -100, 100, 0, 1.0  },
    { "saturation", "saturation",     -100, 100, 0, 1.0  },
    { "gamma",      "gamma",          -100, 100, 0, 1.0  },
    { "hue",        "hue",            -100, 100, 0, 1.0  },
    // 锐度这里是空的：它不走属性，走视频滤镜链。原因见 applySharpen 上面那段。
    { "sharpen",    nullptr,          -100, 100, 0, 1.0  },
    { "zoom",       "video-zoom",      -20,  20, 0, 1.0  },
    // 位置用 pan 而不是 align。
    //
    // align 是"在**留白**里挪画面"：没有留白就没得挪。我们这窗口是宽屏，视频被加的
    // 是左右留白，于是"垂直位置"拉了半天纹丝不动 —— 实测就是这样。
    // pan 是直接平移画面，什么时候都有反应（代价是会裁掉移出画面的部分，但"把画面
    // 往上挪一点"本来就是干这个的）。
    { "pan-x",      "video-pan-x",   -100, 100, 0, 0.01 },
    { "pan-y",      "video-pan-y",   -100, 100, 0, 0.01 },
    { "rotate",     "video-rotate",      0, 359, 0, 1.0  },
};

const PictureControlSpec *findPictureSpec(const QString &name)
{
    for (const PictureControlSpec &spec : kPictureControls) {
        if (name == QLatin1String(spec.name))
            return &spec;
    }
    return nullptr;
}

/** 每个被订阅的属性配一个编号，事件回来时用它区分是哪一条变了。 */
enum PropertyId {
    IdTimePos  = 1,
    IdDuration = 2,
    IdPause    = 3,
    IdVolume   = 4,
    IdMute     = 5,
    IdEof      = 6,
    IdMetadata = 7,
    IdTrackList = 8,
};

/** 把 mpv 的返回码翻成人能看的话。 */
QString mpvError(int code)
{
    return QString::fromUtf8(mpv_error_string(code));
}

/**
 * 把 mpv 的 `metadata`（一张字符串表）转成 QVariantMap。
 *
 * metadata 正常就是"键 -> 字符串"，别的类型（数字之类）先不管 —— 真遇到了
 * 也只是少一个字段，不影响别的。
 */
QVariantMap metadataFromNode(const mpv_node *node)
{
    QVariantMap out;
    if (!node || node->format != MPV_FORMAT_NODE_MAP || !node->u.list)
        return out;

    const mpv_node_list *list = node->u.list;
    for (int i = 0; i < list->num; ++i) {
        const mpv_node &value = list->values[i];
        if (list->keys[i] && value.format == MPV_FORMAT_STRING && value.u.string) {
            out.insert(QString::fromUtf8(list->keys[i]),
                       QString::fromUtf8(value.u.string));
        }
    }
    return out;
}

const mpv_node *mapValue(const mpv_node &map, const char *key)
{
    if (map.format != MPV_FORMAT_NODE_MAP || !map.u.list)
        return nullptr;
    for (int index = 0; index < map.u.list->num; ++index) {
        if (map.u.list->keys[index]
            && qstrcmp(map.u.list->keys[index], key) == 0) {
            return &map.u.list->values[index];
        }
    }
    return nullptr;
}

std::optional<QString> stringValue(
    const mpv_node &map, const char *key)
{
    const mpv_node *value = mapValue(map, key);
    if (!value || value->format != MPV_FORMAT_STRING
        || !value->u.string) {
        return std::nullopt;
    }
    return QString::fromUtf8(value->u.string);
}

std::optional<qint64> intValue(
    const mpv_node &map, const char *key)
{
    const mpv_node *value = mapValue(map, key);
    if (!value || value->format != MPV_FORMAT_INT64)
        return std::nullopt;
    return value->u.int64;
}

std::optional<double> doubleValue(
    const mpv_node &map, const char *key)
{
    const mpv_node *value = mapValue(map, key);
    if (!value)
        return std::nullopt;
    if (value->format == MPV_FORMAT_DOUBLE)
        return value->u.double_;
    if (value->format == MPV_FORMAT_INT64)
        return static_cast<double>(value->u.int64);
    return std::nullopt;
}

bool flagValue(const mpv_node &map, const char *key)
{
    const mpv_node *value = mapValue(map, key);
    return value && value->format == MPV_FORMAT_FLAG
        && value->u.flag != 0;
}

struct MediaSummary {
    QVariantMap info;
    MediaKind kind = MediaKind::Unknown;
};

MediaSummary mediaSummaryFromMpv(
    mpv_handle *mpv, double durationSeconds)
{
    MediaSummary summary;
    if (!mpv)
        return summary;

    mpv_node tracks{};
    if (mpv_get_property(
            mpv, "track-list", MPV_FORMAT_NODE, &tracks) < 0
        || tracks.format != MPV_FORMAT_NODE_ARRAY || !tracks.u.list) {
        return summary;
    }
    struct NodeGuard {
        mpv_node *node;
        ~NodeGuard() { mpv_free_node_contents(node); }
    } tracksGuard{&tracks};

    mpv_node videoParams{};
    const bool hasVideoParams =
        mpv_get_property(
            mpv, "video-params", MPV_FORMAT_NODE, &videoParams) >= 0
        && videoParams.format == MPV_FORMAT_NODE_MAP;
    NodeGuard videoGuard{&videoParams};

    mpv_node audioParams{};
    const bool hasAudioParams =
        mpv_get_property(
            mpv, "audio-params", MPV_FORMAT_NODE, &audioParams) >= 0
        && audioParams.format == MPV_FORMAT_NODE_MAP;
    NodeGuard audioGuard{&audioParams};

    QVariantMap video;
    QVariantMap audio;
    bool image = false;
    for (int index = 0; index < tracks.u.list->num; ++index) {
        const mpv_node &track = tracks.u.list->values[index];
        const QString type = stringValue(track, "type").value_or("");
        if (type == QStringLiteral("video")
            && !flagValue(track, "albumart") && video.isEmpty()) {
            image = flagValue(track, "image");
            video.insert(
                QStringLiteral("codec"),
                stringValue(track, "codec").value_or(""));
            video.insert(
                QStringLiteral("profile"),
                stringValue(track, "codec-profile").value_or(
                    stringValue(track, "profile").value_or("")));
            if (const auto width = intValue(track, "demux-w"))
                video.insert(QStringLiteral("width"), *width);
            if (const auto height = intValue(track, "demux-h"))
                video.insert(QStringLiteral("height"), *height);
            if (const auto fps = doubleValue(track, "demux-fps"))
                video.insert(QStringLiteral("fps"), *fps);
            if (const auto bitrate = intValue(track, "demux-bitrate"))
                video.insert(QStringLiteral("bitrate"), *bitrate);
            if (hasVideoParams) {
                if (!video.contains(QStringLiteral("width"))) {
                    if (const auto width = intValue(videoParams, "w"))
                        video.insert(QStringLiteral("width"), *width);
                }
                if (!video.contains(QStringLiteral("height"))) {
                    if (const auto height = intValue(videoParams, "h"))
                        video.insert(QStringLiteral("height"), *height);
                }
                const QString pixelFormat =
                    stringValue(videoParams, "pixelformat").value_or("");
                if (!pixelFormat.isEmpty())
                    video.insert(QStringLiteral("pixel_format"), pixelFormat);
                if (const auto aspect = doubleValue(videoParams, "aspect"))
                    video.insert(QStringLiteral("aspect"), *aspect);
            }
        } else if (
            type == QStringLiteral("audio") && audio.isEmpty()) {
            audio.insert(
                QStringLiteral("codec"),
                stringValue(track, "codec").value_or(""));
            audio.insert(
                QStringLiteral("profile"),
                stringValue(track, "codec-profile").value_or(
                    stringValue(track, "profile").value_or("")));
            if (const auto rate = intValue(track, "demux-samplerate"))
                audio.insert(QStringLiteral("sample_rate"), *rate);
            if (const auto channels = intValue(track, "demux-channel-count"))
                audio.insert(QStringLiteral("channels"), *channels);
            const QString layout =
                stringValue(track, "demux-channels").value_or("");
            if (!layout.isEmpty())
                audio.insert(QStringLiteral("channel_layout"), layout);
            if (const auto bitrate = intValue(track, "demux-bitrate"))
                audio.insert(QStringLiteral("bitrate"), *bitrate);
            if (hasAudioParams) {
                if (!audio.contains(QStringLiteral("sample_rate"))) {
                    if (const auto rate =
                            intValue(audioParams, "samplerate")) {
                        audio.insert(QStringLiteral("sample_rate"), *rate);
                    }
                }
                if (!audio.contains(QStringLiteral("channels"))) {
                    if (const auto channels =
                            intValue(audioParams, "channel-count")) {
                        audio.insert(QStringLiteral("channels"), *channels);
                    }
                }
                if (!audio.contains(QStringLiteral("channel_layout"))) {
                    const QString layout =
                        stringValue(audioParams, "channels").value_or("");
                    if (!layout.isEmpty()) {
                        audio.insert(
                            QStringLiteral("channel_layout"), layout);
                    }
                }
                const QString sampleFormat =
                    stringValue(audioParams, "format").value_or("");
                if (!sampleFormat.isEmpty()) {
                    audio.insert(
                        QStringLiteral("sample_format"), sampleFormat);
                }
            }
        }
    }

    int64_t videoBitrate = 0;
    if (mpv_get_property(
            mpv, "video-bitrate", MPV_FORMAT_INT64, &videoBitrate) >= 0
        && videoBitrate > 0) {
        video.insert(QStringLiteral("bitrate"), videoBitrate);
    }
    int64_t audioBitrate = 0;
    if (mpv_get_property(
            mpv, "audio-bitrate", MPV_FORMAT_INT64, &audioBitrate) >= 0
        && audioBitrate > 0) {
        audio.insert(QStringLiteral("bitrate"), audioBitrate);
    }

    char *format = mpv_get_property_string(mpv, "file-format");
    if (format) {
        const QString container = QString::fromUtf8(format).simplified();
        if (!container.isEmpty())
            summary.info.insert(QStringLiteral("container"), container);
        mpv_free(format);
    }
    summary.info.insert(
        QStringLiteral("duration_seconds"), durationSeconds);
    if (!video.isEmpty()) {
        summary.info.insert(QStringLiteral("video"), video);
        summary.kind = image ? MediaKind::Image : MediaKind::Video;
    }
    if (!audio.isEmpty()) {
        summary.info.insert(QStringLiteral("audio"), audio);
        if (summary.kind == MediaKind::Unknown)
            summary.kind = MediaKind::Audio;
    }
    if (summary.kind == MediaKind::Video)
        summary.info.insert(QStringLiteral("kind"), QStringLiteral("video"));
    else if (summary.kind == MediaKind::Audio)
        summary.info.insert(QStringLiteral("kind"), QStringLiteral("audio"));
    else if (summary.kind == MediaKind::Image)
        summary.info.insert(QStringLiteral("kind"), QStringLiteral("image"));
    return summary;
}

QString currentMediaUri(mpv_handle *mpv)
{
    if (!mpv)
        return {};
    char *value = mpv_get_property_string(mpv, "path");
    if (!value)
        return {};
    const QString uri = QString::fromUtf8(value);
    mpv_free(value);
    return uri;
}

QString albumArtFileFromTracks(mpv_handle *mpv)
{
    mpv_node root{};
    if (!mpv || mpv_get_property(
            mpv, "track-list", MPV_FORMAT_NODE, &root) < 0) {
        return {};
    }
    struct NodeGuard {
        mpv_node *node;
        ~NodeGuard() { mpv_free_node_contents(node); }
    } guard{&root};

    if (root.format != MPV_FORMAT_NODE_ARRAY || !root.u.list)
        return {};
    for (int index = 0; index < root.u.list->num; ++index) {
        const mpv_node &track = root.u.list->values[index];
        const mpv_node *albumArt = mapValue(track, "albumart");
        if (!albumArt || albumArt->format != MPV_FORMAT_FLAG
            || albumArt->u.flag == 0) {
            continue;
        }
        const mpv_node *filename =
            mapValue(track, "external-filename");
        if (filename && filename->format == MPV_FORMAT_STRING
            && filename->u.string && *filename->u.string) {
            return QUrl::fromLocalFile(
                QString::fromUtf8(filename->u.string)).toString();
        }
        // Embedded artwork has no external filename. Let the caller extract
        // the current picture through screenshot-raw.
        return QStringLiteral("embedded:");
    }
    return {};
}

QString saveEmbeddedAlbumArt(mpv_handle *mpv, const QString &mediaUri)
{
    const char *args[] = {
        "screenshot-raw", "video", "bgra", nullptr};
    mpv_node result{};
    if (!mpv || mpv_command_ret(mpv, args, &result) < 0)
        return {};
    struct NodeGuard {
        mpv_node *node;
        ~NodeGuard() { mpv_free_node_contents(node); }
    } guard{&result};
    if (result.format != MPV_FORMAT_NODE_MAP)
        return {};

    const mpv_node *width = mapValue(result, "w");
    const mpv_node *height = mapValue(result, "h");
    const mpv_node *stride = mapValue(result, "stride");
    const mpv_node *format = mapValue(result, "format");
    const mpv_node *data = mapValue(result, "data");
    if (!width || !height || !stride || !format || !data
        || width->format != MPV_FORMAT_INT64
        || height->format != MPV_FORMAT_INT64
        || stride->format != MPV_FORMAT_INT64
        || format->format != MPV_FORMAT_STRING
        || data->format != MPV_FORMAT_BYTE_ARRAY
        || !format->u.string || !data->u.ba || !data->u.ba->data) {
        return {};
    }
    if (qstrcmp(format->u.string, "bgra") != 0
        || width->u.int64 <= 0 || height->u.int64 <= 0
        || stride->u.int64 <= 0
        || width->u.int64 > 4096 || height->u.int64 > 4096
        || width->u.int64 * height->u.int64 > 16'777'216) {
        return {};
    }

    const qsizetype required =
        (height->u.int64 - 1) * stride->u.int64
        + width->u.int64 * 4;
    if (required <= 0
        || static_cast<quint64>(required) > data->u.ba->size) {
        return {};
    }
    QImage view(
        static_cast<const uchar *>(data->u.ba->data),
        static_cast<int>(width->u.int64),
        static_cast<int>(height->u.int64),
        static_cast<int>(stride->u.int64), QImage::Format_ARGB32);
    if (view.isNull())
        return {};
    const QImage owned = view.copy();

    const QString directory =
        QStandardPaths::writableLocation(
            QStandardPaths::TempLocation)
        + QStringLiteral("/MediaCast/artwork");
    if (!QDir().mkpath(directory))
        return {};
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    hasher.addData(QByteArrayView(mediaUri.toUtf8()));
    hasher.addData(QByteArrayView(
        reinterpret_cast<const char *>(owned.constBits()),
        static_cast<qsizetype>(owned.sizeInBytes())));
    const QByteArray digest = hasher.result().toHex().left(20);
    const QString path =
        directory + QStringLiteral("/cover-")
        + QString::fromLatin1(digest) + QStringLiteral(".png");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || !owned.save(&file, "PNG")
        || !file.commit()) {
        return {};
    }
    const QString currentName = QFileInfo(path).fileName();
    for (const QString &name :
         QDir(directory).entryList(
             {QStringLiteral("cover-*.png")}, QDir::Files)) {
        if (name != currentName)
            QFile::remove(directory + QLatin1Char('/') + name);
    }
    return QUrl::fromLocalFile(path).toString();
}

void clearTemporaryArtwork()
{
    const QString directory =
        QStandardPaths::writableLocation(
            QStandardPaths::TempLocation)
        + QStringLiteral("/MediaCast/artwork");
    QDir dir(directory);
    for (const QString &name :
         dir.entryList(
             {QStringLiteral("cover-*.png")},
             QDir::Files)) {
        dir.remove(name);
    }
}

QString imageSequenceDirectory()
{
    return QStandardPaths::writableLocation(
               QStandardPaths::TempLocation)
        + QStringLiteral("/MediaCast/image-sequences");
}

void clearTemporaryImageSequences()
{
    QDir dir(imageSequenceDirectory());
    for (const QString &name :
         dir.entryList(
             {
                 QStringLiteral("sequence-*.txt"),
                 QStringLiteral("sequence-*.m3u8"),
                 QStringLiteral("sequence-*.edl"),
             },
             QDir::Files)) {
        dir.remove(name);
    }
}

} // namespace

MpvCore::MpvCore(QObject *parent)
    : MediaPlayer(parent)
    , m_imageSequence(
          std::make_unique<mcast::core::ImageSequenceStrategy>(this))
{
    connect(
        m_imageSequence.get(),
        &mcast::core::ImageSequenceStrategy::positionChanged,
        this, [this](double seconds) {
            m_positionSec.store(seconds);
            emit positionChanged(seconds);
        });
    connect(
        m_imageSequence.get(),
        &mcast::core::ImageSequenceStrategy::durationChanged,
        this, [this](double seconds) {
            m_durationSec.store(seconds);
            emit durationChanged(seconds);
            refreshMediaInfoSnapshot();
        });
    connect(
        m_imageSequence.get(),
        &mcast::core::ImageSequenceStrategy::ended,
        this, &MpvCore::ended);
    connect(
        m_imageSequence.get(),
        &mcast::core::ImageSequenceStrategy::loadFailed,
        this, [this](const QString &reason) {
            m_loadFailed.store(true);
            m_imageSequenceActive.store(false);
            emit logMessage(
                QStringLiteral("图片序列加载失败：%1").arg(reason));
            emit loadFailed(
                m_pendingLoadGeneration.load(), reason);
        });
    connect(
        m_imageSequence.get(),
        &mcast::core::ImageSequenceStrategy::logMessage,
        this, &MpvCore::logMessage);
}

MpvCore::~MpvCore()
{
    shutdown();
}

QVariantMap MpvCore::mediaInfo() const
{
    const std::scoped_lock lock(m_mediaInfoMutex);
    return m_mediaInfo;
}

void MpvCore::refreshMediaInfoSnapshot()
{
    publishMediaSummary();
}

void MpvCore::publishMediaSummary()
{
    if (!m_mpv)
        return;
    MediaSummary summary =
        mediaSummaryFromMpv(m_mpv, m_durationSec.load());
    if (m_imageSequenceActive.load()) {
        summary.kind = MediaKind::Video;
        summary.info.insert(
            QStringLiteral("duration_seconds"),
            m_durationSec.load());
        summary.info.insert(
            QStringLiteral("kind"), QStringLiteral("video"));
    }
    {
        std::lock_guard lock(m_mediaInfoMutex);
        m_mediaInfo = summary.info;
    }
    emit mediaInfoChanged();

    QString mediaUri = logicalMediaUri(currentMediaUri(m_mpv));
    {
        std::lock_guard lock(m_uriMutex);
        if (mediaUri.isEmpty())
            mediaUri = m_eventMediaUri;
        if (mediaUri.isEmpty())
            mediaUri = m_lastRequestedUri;
    }
    if (summary.kind != MediaKind::Unknown
        && !mediaUri.isEmpty()) {
        emit mediaKindChanged(summary.kind, mediaUri);
    }
}

bool MpvCore::start()
{
    if (m_mpv)
        return true;

    // Remove files left behind by a crash before this process writes a new
    // cover. The active UI process owns this cache directory.
    clearTemporaryArtwork();
    clearTemporaryImageSequences();
    m_stopRequested = false;

    m_mpv = mpv_create();
    if (!m_mpv) {
        emit statusChanged(QStringLiteral("libmpv 初始化失败：mpv_create 返回空"));
        return false;
    }

    // 没文件也保持运行，等着我们发 loadfile。
    mpv_set_option_string(m_mpv, "idle", "yes");

    // **播完了别把文件卸掉。**
    //
    // mpv 默认（keep-open=no）一到文件末尾就把文件卸了。卸掉之后再按播放，
    // play() 只是设 pause=0 —— 手上却没文件，结果就是那句老熟人：
    // "状态显示正在播放、画面全黑、位置永远是 0"。
    //
    // 这个 bug 以前修过一次（见 stop() 里那段），但**只修了"按停止"那条路**；
    // "让片子自然播完"走的是另一个入口（mpv 自己到 EOF），照样翻车。
    // 开了这个之后，播完会停在最后一帧、文件还在手上，再由 play() 里的
    // "播完了就从头来"把它重新放起来。
    mpv_set_option_string(m_mpv, "keep-open", "yes");

    // **图片放着别动。**
    //
    // mpv 默认把一张图片当成"时长为 1 秒的片子"：加载之后放一会儿，然后就到
    // 文件末尾了。对我们来说那意味着状态翻成 STOPPED —— 胶囊回"已连接"、
    // 容器上那块提示面板盖上来，**投过来的照片几秒后就被引导文字盖掉了**。
    // （实测：投上去 5 秒左右变 STOPPED，页面只剩"已经连上了……"那句。）
    //
    // 投屏语义里，一张照片就是"一直显示到下一张为止"，没有"放完了"这回事。
    // 参考实现（Macast）也是这么设的：--image-display-duration=inf。
    //
    // 只影响图片文件，视频一个字节都不碰。
    mpv_set_option_string(m_mpv, "image-display-duration", "inf");

    // 和进程版一样的理由：关掉 mpv 自带的一切控制。用户能直接操作的地方，就是能绕过
    // 状态机的地方 —— 那正是前面三处 bug 的根因。
    mpv_set_option_string(m_mpv, "osc", "no");
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "cursor-autohide", "always");
    mpv_set_option_string(m_mpv, "terminal", "no");

    // render API 必须在 mpv 初始化前设好视频输出参数。
    applyStartupOptions(m_mpv);

    const int rc = mpv_initialize(m_mpv);
    if (rc < 0) {
        emit statusChanged(QStringLiteral("libmpv 初始化失败：") + mpvError(rc));
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return false;
    }

    // 只要警告以上的日志，全开的话噪音太大。
    mpv_request_log_messages(m_mpv, "warn");

    // 订阅这几条：变了 mpv 会主动推事件过来，不用我们一直去问。
    mpv_observe_property(m_mpv, IdTimePos,  "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, IdDuration, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, IdPause,    "pause",    MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, IdEof,      "eof-reached", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, IdVolume,   "volume",   MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, IdMute,     "mute",     MPV_FORMAT_FLAG);
    // 文件自带的标签。和协议给的元数据是两回事，见 MediaPlayer::metadataChanged。
    mpv_observe_property(m_mpv, IdMetadata, "metadata", MPV_FORMAT_NODE);
    mpv_observe_property(
        m_mpv, IdTrackList, "track-list", MPV_FORMAT_NODE);

    // 窗口标题。播放核心自己处理，避免界面层越界去碰 mpv 细节。
    mpv_set_property_string(m_mpv, "title",
                            QCoreApplication::applicationName().toUtf8().constData());

    m_thread = std::thread(&MpvCore::eventLoop, this);

    emit statusChanged(QStringLiteral("libmpv 已就绪"));
    emit runningChanged();
    return true;
}

void MpvCore::shutdown()
{
    m_stopRequested = true;

    // mpv_wait_event 正阻塞着，叫醒它，让后台线程有机会看到停止标志。
    if (m_mpv)
        mpv_wakeup(m_mpv);

    if (m_thread.joinable())
        m_thread.join();

    m_wrappedTsProxy.reset();
    clearTemporaryArtwork();
    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        emit runningChanged();
    }
    clearTemporaryImageSequence();
    clearTemporaryImageSequences();
}

void MpvCore::eventLoop()
{
    while (true) {
        // A short timeout lets repeated demuxer warnings be collapsed into a
        // bounded start/end/count log burst instead of flooding the Ui log.
        mpv_event *event = mpv_wait_event(m_mpv, 0.25);

        if (!event || event->event_id == MPV_EVENT_NONE) {
            if (m_stopRequested.load()) {
                finishPacketCorruptBurst();
                break;
            }
            if (m_packetCorruptBurstActive
                && std::chrono::steady_clock::now()
                    - m_packetCorruptLast
                    >= std::chrono::seconds(1)) {
                finishPacketCorruptBurst();
            }
            continue;
        }

        switch (event->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto *prop = static_cast<mpv_event_property *>(event->data);
            if (!prop)
                break;

            // data 为空表示这个属性暂时没有值（比如停止播放后 time-pos 就没了），
            // 当成 0 处理，界面才会正确地归零。
            switch (event->reply_userdata) {
            case IdTimePos: {
                if (m_imageSequenceActive.load())
                    break;
                const double v = prop->data ? *static_cast<double *>(prop->data) : 0.0;
                if (!std::isfinite(v) || std::abs(v) > 1.0e9) {
                    emit logMessage(
                        QStringLiteral("忽略异常的 MPV 播放位置"));
                    break;
                }
                m_positionSec.store(v);
                emit positionChanged(v);
                break;
            }
            case IdDuration: {
                if (m_imageSequenceActive.load())
                    break;
                const double v = prop->data ? *static_cast<double *>(prop->data) : 0.0;
                m_durationSec.store(v);
                emit durationChanged(v);
                refreshMediaInfoSnapshot();
                break;
            }
            case IdPause: {
                const bool paused = prop->data && (*static_cast<int *>(prop->data) != 0);
                emit pausedChanged(paused);
                break;
            }
            case IdEof: {
                if (m_imageSequenceActive.load())
                    break;
                const bool eof = prop->data && (*static_cast<int *>(prop->data) != 0);
                const bool wasEof = m_eofReached.exchange(eof);

                // **"播完了"这件事现在只能从这儿报出去。**
                //
                // 开了 keep-open 之后，mpv 到末尾不再卸文件，于是 END_FILE 事件
                // 根本不来 —— 上层就永远以为自己还在播。以前靠 END_FILE 报，
                // 现在靠 eof-reached 由 false 变 true 这一下报。
                if (eof && !wasEof)
                    emit ended();
                break;
            }
            case IdVolume: {
                const int v = prop->data ? qRound(*static_cast<double *>(prop->data)) : 0;
                m_volumePercent.store(v);
                emit volumeChanged(v);
                break;
            }
            case IdMute: {
                const bool muted = prop->data && (*static_cast<int *>(prop->data) != 0);
                m_muted.store(muted);
                emit muteChanged(muted);
                break;
            }
            case IdMetadata: {
                // 文件自带的标签（标题/艺术家/专辑/歌词……）。**没有的话就是空的**，
                // 别指望它有东西 —— 图片和视频实测基本都是空的。
                QString mediaUri;
                {
                    std::lock_guard lock(m_uriMutex);
                    mediaUri = m_eventMediaUri;
                }
                emit metadataChanged(
                    metadataFromNode(static_cast<mpv_node *>(prop->data)),
                    mediaUri);
                break;
            }
            case IdTrackList:
                publishMediaSummary();
                break;
            default:
                break;
            }
            break;
        }

        case MPV_EVENT_FILE_LOADED: {
            // "文件加载好了" —— DLNA 的 TRANSITIONING 就是在等这一声。
            //
            // 顺手把这个文件的「暂停」标记清掉。**这一步不能省。**
            //
            // 上一版给 mpv 开了 keep-open，它在片子播完时会把自己置成暂停 ——
            // 它就是靠这个"停在最后一帧"，eof-reached 也一直挂着 yes。
            // 而 **mpv 换文件不清这个标记**：于是"上一条自然播完 → 控制点投
            // 下一条"就成了——文件加载了、时长也报得出来、上层状态还写着正在播放，
            // 但位置永远停在 0，画面一动不动。
            //
            // 实测：卡住的时候再按一次播放就活了（那正是 play() 里清 pause 那一步），
            // 说明残余的就是这个标记，不是别的东西。
            //
            // 放在这儿、而不是 loadNow() 里：命令是排队执行的，在发 loadfile 那一瞬间
            // 旧文件还在手上，那时候放开 pause 会在旧文件的末尾再滚一圈、又报一次播完。
            // 等到这一声，新文件已经就位了，放开就是干干净净地开始放。
            // 只有"没人喊过停/暂停"的时候才放开。加载期间控制点按了停止的话，
            // 文件照样加载（它得知道这条能不能放），但**不许自己开播** ——
            // 否则状态机会说"停了"，画面却在放。
            if (m_playOnLoad.load()) {
                int flag = 0;
                mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
            }

            m_loadFailed = false;
            emit ready(m_activeLoadGeneration.load());
            if (m_imageSequenceActive.load()) {
                const std::uint64_t imageGeneration =
                    m_imageSequenceLoadGeneration.load();
                QMetaObject::invokeMethod(
                    m_imageSequence.get(),
                    [this, imageGeneration] {
                        if (m_imageSequenceActive.load()
                            && imageGeneration
                                == m_imageSequenceLoadGeneration.load()) {
                            m_imageSequence->markLoaded();
                        }
                    },
                    Qt::QueuedConnection);
            }

            QString loadedMediaUri =
                logicalMediaUri(currentMediaUri(m_mpv));
            {
                std::lock_guard lock(m_uriMutex);
                if (!loadedMediaUri.isEmpty())
                    m_eventMediaUri = loadedMediaUri;
                else
                    loadedMediaUri = m_eventMediaUri;
                if (loadedMediaUri.isEmpty())
                    loadedMediaUri = m_lastRequestedUri;
            }

            publishMediaSummary();

            QString artwork = albumArtFileFromTracks(m_mpv);
            QString artworkMediaUri = loadedMediaUri;
            if (artwork == QStringLiteral("embedded:")) {
                {
                    std::lock_guard lock(m_uriMutex);
                    m_pendingArtworkUri = artworkMediaUri;
                    m_artworkPending = true;
                    m_artworkRetryCount = 0;
                }
                artwork = saveEmbeddedAlbumArt(
                    m_mpv, artworkMediaUri);
                if (!artwork.isEmpty()) {
                    std::lock_guard lock(m_uriMutex);
                    m_artworkPending = false;
                }
            }
            emit artworkChanged(artwork, artworkMediaUri);
            break;
        }

        case MPV_EVENT_PLAYBACK_RESTART: {
            const QString currentUri =
                logicalMediaUri(currentMediaUri(m_mpv));
            QString pendingUri;
            {
                std::lock_guard lock(m_uriMutex);
                if (!m_artworkPending)
                    break;
                pendingUri = m_pendingArtworkUri;
                if (currentUri.isEmpty()
                    || (!pendingUri.isEmpty()
                        && currentUri != pendingUri)
                    || m_artworkRetryCount >= 2) {
                    m_artworkPending = false;
                    m_pendingArtworkUri.clear();
                    m_artworkRetryCount = 0;
                    break;
                }
                ++m_artworkRetryCount;
            }
            const QString artwork = saveEmbeddedAlbumArt(
                m_mpv, pendingUri);
            if (artwork.isEmpty())
                break;
            {
                std::lock_guard lock(m_uriMutex);
                m_artworkPending = false;
                m_pendingArtworkUri.clear();
                m_artworkRetryCount = 0;
            }
            emit artworkChanged(artwork, currentUri);
            break;
        }

        case MPV_EVENT_START_FILE: {
            finishPacketCorruptBurst();
            m_activeLoadGeneration.store(
                m_pendingLoadGeneration.load());
            const QString startingUri =
                logicalMediaUri(currentMediaUri(m_mpv));
            {
                std::lock_guard lock(m_uriMutex);
                m_eventMediaUri = startingUri;
            }
            emit logMessage(QStringLiteral("开始加载媒体 ..."));
            break;
        }

        case MPV_EVENT_END_FILE: {
            finishPacketCorruptBurst();
            auto *endFile = static_cast<mpv_event_end_file *>(event->data);
            if (endFile && endFile->error < 0) {
                m_loadFailed = true;
                const QString reason =
                    QStringLiteral("播放失败：") + mpvError(endFile->error);
                emit logMessage(reason);
                if (m_imageSequenceActive.exchange(false)) {
                    ++m_imageSequenceLoadGeneration;
                    QMetaObject::invokeMethod(
                        m_imageSequence.get(),
                        [this] { m_imageSequence->reset(); },
                        Qt::QueuedConnection);
                }
                emit loadFailed(
                    m_activeLoadGeneration.load(), reason);
                break;
            }
            if (m_imageSequenceActive.load())
                break;
            emit ended();
            break;
        }

        case MPV_EVENT_LOG_MESSAGE: {
            auto *msg = static_cast<mpv_event_log_message *>(event->data);
            if (msg) {
                const QString text =
                    QString::fromUtf8(msg->text).trimmed();
                if (text.contains(
                        QStringLiteral("Packet corrupt"),
                        Qt::CaseInsensitive)) {
                    if (!m_packetCorruptBurstActive) {
                        m_packetCorruptBurstActive = true;
                        m_packetCorruptCount = 0;
                        emit logMessage(
                            QStringLiteral(
                                "mpv: Packet corrupt 连续错误开始"));
                    }
                    ++m_packetCorruptCount;
                    m_packetCorruptLast =
                        std::chrono::steady_clock::now();
                } else {
                    emit logMessage(
                        QStringLiteral("mpv: ") + text);
                }
            }
            break;
        }

        case MPV_EVENT_SHUTDOWN:
            finishPacketCorruptBurst();
            emit lost();
            break;

        default:
            break;
        }

        if (m_stopRequested.load())
            break;
    }
}

void MpvCore::finishPacketCorruptBurst()
{
    if (!m_packetCorruptBurstActive)
        return;
    emit logMessage(
        QStringLiteral("mpv: Packet corrupt 连续错误结束，共 %1 条")
            .arg(m_packetCorruptCount));
    m_packetCorruptBurstActive = false;
    m_packetCorruptCount = 0;
}

void MpvCore::load(const MediaLoadRequest &request)
{
    if (!m_mpv || request.logicalUri.isEmpty())
        return;
    {
        std::lock_guard lock(m_uriMutex);
        m_lastRequestedUri = request.logicalUri;
    }
    m_lastLoadRequest = request;
    m_pendingLoadGeneration.store(request.loadGeneration);
    m_loadFailed = false;

    // 渲染面没就绪就先攒着 —— 现在发出去，mpv 会报
    // "No render context set" 然后放弃，而且不会再重试。详见头文件那段。
    if (!m_rendererAttached) {
        m_pendingLoad = request;
        emit logMessage(QStringLiteral("渲染窗口还没准备好，这条先攒着，等窗口起来再放"));
        return;
    }

    activateLoad(request);
}

void MpvCore::activateLoad(const MediaLoadRequest &request)
{
    if (request.route == MediaLoadRoute::ImageSequence) {
        loadImageSequencePlan(request);
        return;
    }
    if (request.route == MediaLoadRoute::WrappedTransportStream) {
        QVector<mcast::core::ImageSequenceSegment> segments;
        segments.reserve(request.imageSequenceFrames.size());
        for (const MediaLoadFrame &frame :
             request.imageSequenceFrames) {
            segments.push_back({
                QUrl::fromEncoded(frame.uri.toUtf8()),
                frame.durationSeconds,
            });
        }
        if (segments.isEmpty()) {
            const QString reason =
                QStringLiteral("包装传输流计划没有分片");
            emit logMessage(reason);
            emit loadFailed(
                m_pendingLoadGeneration.load(), reason);
            return;
        }
        prepareRouteLoad(request.logicalUri);
        emit logMessage(
            QStringLiteral("收到协议指定的包装传输流计划：%1 段")
                .arg(segments.size()));
        loadWrappedTransportStream(segments, request.logicalUri);
        return;
    }
    loadNow(request.logicalUri);
}

void MpvCore::notifyRenderUpdate()
{
    // 只做转发 —— 谁要重画谁自己连这个信号（只有 MpvQmlItem 会连）。
    //
    // 在这儿**不能**去碰任何"当前那个 item"：这个函数是渲染线程通过队列连接
    // 调进来的，跑在界面线程上，而 item 可能刚刚被销毁。发个信号最省事也最安全：
    // 接收者没了，Qt 自己会把连接摘掉，不会有野指针。
    emit renderUpdate();
}

void MpvCore::setRendererAttached(bool attached)
{
    if (!attached) {
        m_rendererAttached = false;
        // Keep the pending URI across a short renderer detach. Qt can tear
        // down and recreate the framebuffer renderer while the same window is
        // still being brought up; clearing here permanently dropped the first
        // cast, then a second cast appeared to "fix" it. If the process is
        // really exiting, it disappears together with MpvCore anyway.
        return;
    }
    m_rendererAttached = true;
    emit rendererReady();

    std::optional<MediaLoadRequest> request = std::move(m_pendingLoad);
    m_pendingLoad.reset();
    if ((!request || request->logicalUri.isEmpty())
        && m_loadFailed.load()
        && m_lastLoadRequest) {
        request = m_lastLoadRequest;
        m_loadFailed = false;
    }
    if (!request || request->logicalUri.isEmpty())
        return;

    // 攒着的那条现在可以放了。这就是"先拉起界面、等渲染面就绪再投屏"那一步。
    emit logMessage(QStringLiteral("渲染窗口已就绪，把攒着的那条放出去"));
    activateLoad(*request);
}

bool MpvCore::loadImageSequencePlan(const MediaLoadRequest &request)
{
    QVector<mcast::core::ImageSequenceSegment> segments;
    segments.reserve(request.imageSequenceFrames.size());
    for (const MediaLoadFrame &frame : request.imageSequenceFrames) {
        const QUrl uri = QUrl::fromEncoded(frame.uri.toUtf8());
        segments.push_back({uri, frame.durationSeconds});
    }

    prepareRouteLoad(request.logicalUri);
    {
        std::lock_guard lock(m_uriMutex);
        m_activeImageListPath.clear();
    }
    emit positionChanged(0.0);
    emit durationChanged(0.0);
    emit logMessage(
        QStringLiteral("收到协议指定的图片序列计划：%1 帧")
            .arg(segments.size()));
    return loadMfImageSequence(segments, request.logicalUri);
}

void MpvCore::prepareRouteLoad(const QString &logicalUri)
{
    m_playOnLoad.store(true);
    m_wrappedTsProxy.reset();
    clearTemporaryImageSequence();
    resetImageTimeline();
    {
        std::lock_guard lock(m_uriMutex);
        m_lastRequestedUri = logicalUri;
        m_activeMpvUri.clear();
        m_eventMediaUri.clear();
        m_artworkPending = false;
        m_pendingArtworkUri.clear();
        m_artworkRetryCount = 0;
    }
    {
        std::lock_guard lock(m_mediaInfoMutex);
        m_mediaInfo.clear();
    }
    emit mediaInfoChanged();
}

void MpvCore::loadNow(const QString &uri)
{
    // 新的一条内容：加载好就该放。stop()/pause() 会把这一位按下去（见头文件），
    // 这里是把它扳回来 —— 不然"停止之后又投一条"就永远等不到自动播放了。
    m_playOnLoad.store(true);
    m_wrappedTsProxy.reset();
    clearTemporaryImageSequence();
    resetImageTimeline();
    {
        std::lock_guard lock(m_uriMutex);
        m_lastRequestedUri = uri;
        m_activeMpvUri.clear();
        m_eventMediaUri.clear();
    }
    {
        std::lock_guard lock(m_uriMutex);
        m_artworkPending = false;
        m_pendingArtworkUri.clear();
        m_artworkRetryCount = 0;
    }
    {
        std::lock_guard lock(m_mediaInfoMutex);
        m_mediaInfo.clear();
    }
    emit mediaInfoChanged();

    // 换片子的时候先把缓存的进度清掉。
    //
    // 不清的话，从"发出 loadfile"到"mpv 真的把新文件打开"之间有个空档，那会儿
    // time-pos 还是**上一条**的位置。控制点正好在这时候来问 GetPositionInfo，
    // 就会拿到上一条的进度 —— 它那边的进度条会跳一下。设备上每秒都被问一次，
    // 这个空档撞上的概率不小。
    m_positionSec = 0.0;
    m_durationSec = 0.0;

    beginLoad(uri);
}

void MpvCore::beginLoad(const QString &uri)
{
    loadMpvFile(uri, uri);
}

bool MpvCore::loadMfImageSequence(
    const QVector<mcast::core::ImageSequenceSegment> &segments,
    const QString &logicalUri)
{
    ++m_imageSequenceLoadGeneration;
    // Set this before loadfile reaches MPV. A very fast image load can publish
    // duration/time-pos events before start() returns; those values belong to
    // MPV's one-frame image model and must never overwrite the virtual timeline.
    m_imageSequenceActive.store(true);
    const bool started = m_imageSequence->start(
        segments,
        [this, logicalUri](
            const QString &mfUri, const QString &,
            double fps, bool play) {
            const QByteArray fpsText =
                QByteArray::number(fps, 'f', 6);
            mpv_set_property_string(
                m_mpv, "mf-fps", fpsText.constData());
            m_playOnLoad.store(play);
            loadMpvFile(
                mfUri, logicalUri, QString(), true);
            return true;
        });
    m_imageSequenceActive.store(started);
    if (!started)
        m_loadFailed.store(true);
    return started;
}

void MpvCore::loadWrappedTransportStream(
    const QVector<mcast::core::ImageSequenceSegment> &segments,
    const QString &logicalUri)
{
    m_wrappedTsProxy = std::make_unique<WrappedTsProxy>();
    connect(
        m_wrappedTsProxy.get(), &WrappedTsProxy::logMessage,
        this, &MpvCore::logMessage);
    QString proxyError;
    if (!m_wrappedTsProxy->start(segments, proxyError)) {
        const QString reason =
            QStringLiteral("无法启动本地媒体代理：") + proxyError;
        emit logMessage(reason);
        emit loadFailed(
            m_pendingLoadGeneration.load(), reason);
        return;
    }

    emit logMessage(
        QStringLiteral("检测到 PNG 包装的 MPEG-TS，已恢复 ")
        + QString::number(segments.size())
        + QStringLiteral(" 段视频"));
    loadMpvFile(
        m_wrappedTsProxy->playlistUrl().toString(QUrl::FullyEncoded),
        logicalUri);
}

void MpvCore::loadMpvFile(
    const QString &mpvUri, const QString &logicalUri,
    const QString &temporaryImageListPath, bool emitStarted)
{
    {
        std::lock_guard lock(m_uriMutex);
        m_lastRequestedUri = logicalUri;
        m_activeMpvUri = mpvUri;
        m_activeImageListPath = temporaryImageListPath;
        m_eventMediaUri = logicalUri;
    }

    // 用数组形式传参，不拼字符串 —— URL 里带引号或反斜杠时，拼字符串迟早出错。
    const QByteArray path = mpvUri.toUtf8();
    const char *command[] = {
        "loadfile", path.constData(), "replace", nullptr};
    mpv_command(m_mpv, command);
    if (emitStarted)
        emit loadStarted(m_pendingLoadGeneration.load());
}

void MpvCore::clearTemporaryImageSequence()
{
    std::lock_guard lock(m_uriMutex);
    if (!m_activeImageListPath.isEmpty()
        && QFile::remove(m_activeImageListPath)) {
        m_activeImageListPath.clear();
    }
}

QString MpvCore::logicalMediaUri(const QString &mpvUri) const
{
    std::lock_guard lock(m_uriMutex);
    const QString effectiveUri =
        mpvUri.isEmpty() ? m_activeMpvUri : mpvUri;
    if (!m_activeMpvUri.isEmpty()
        && effectiveUri == m_activeMpvUri
        && !m_lastRequestedUri.isEmpty()) {
        return m_lastRequestedUri;
    }
    return mpvUri;
}

void MpvCore::resetImageTimeline()
{
    ++m_imageSequenceLoadGeneration;
    m_imageSequence->reset();
    m_imageSequenceActive.store(false);
    m_eofReached.store(false);
}

void MpvCore::play()
{
    if (!m_mpv)
        return;

    if (m_imageSequenceActive.load())
        m_imageSequence->play();

    // **播完了再按播放，就是"从头再放一遍"。**
    //
    // 不先回到 0 的话，mpv 手上还停在那条片子的末尾（keep-open 让它把文件留着了），
    // 一放开就立刻又到 EOF —— 用户看到的是"按了播放，什么也没发生"。
    //
    // 注意这里只看"播没播到头"。用户自己按的「停止」走的是 stop()，那条路会把
    // 位置归零，eof-reached 也是 false，不会误判。
    if (!m_imageSequenceActive.load() && m_eofReached.load()) {
        const char *args[] = { "seek", "0", "absolute", nullptr };
        mpv_command(m_mpv, args);
        m_eofReached.store(false);
    }

    // 用户让放了 —— 加载还没结束的话，等它加载好也接着放。
    m_playOnLoad.store(true);

    int flag = 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvCore::pause()
{
    if (!m_mpv)
        return;

    if (m_imageSequenceActive.load())
        m_imageSequence->pause();

    // 同上，反着来：加载还没结束就按了暂停，那加载好也别自己开播。
    m_playOnLoad.store(false);

    int flag = 1;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvCore::stop()
{
    if (!m_mpv)
        return;

    if (m_imageSequenceActive.load())
        m_imageSequence->stop();

    // **加载期间就被喊停**的话，这一句是关键：文件还是会加载完，但加载好那一声
    // 不会再把它放开（见 MPV_EVENT_FILE_LOADED 那段）。
    m_playOnLoad.store(false);

    // 「停止」的语义是**停下，但媒体还装着** —— 之后再按播放要能接着放。
    //
    // 所以这里**不能**用 mpv 的 `stop` 命令：那条命令会把文件整个卸掉，之后
    // play() 只设 pause=0，手上却没文件，结果就是"状态显示正在播放、画面全黑、
    // 位置永远是 0"。这个 bug 存在了很久，因为它只在"停止 → 再播放"这条路径上
    // 出现，而回归测试一直只测"暂停 → 播放"。
    //
    // 正确做法：暂停 + 回到开头。两条命令都是对**当前这条**生效，不卸载。
    int flag = 1;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);

    const char *seek[] = {"seek", "0", "absolute", nullptr};
    mpv_command(m_mpv, seek);
}

void MpvCore::unload()
{
    if (!m_mpv)
        return;

    m_playOnLoad.store(false);
    m_pendingLoad.reset();
    m_lastLoadRequest.reset();
    m_wrappedTsProxy.reset();
    clearTemporaryImageSequence();
    resetImageTimeline();
    {
        std::lock_guard lock(m_uriMutex);
        m_lastRequestedUri.clear();
        m_activeMpvUri.clear();
        m_eventMediaUri.clear();
        m_pendingArtworkUri.clear();
        m_artworkPending = false;
        m_artworkRetryCount = 0;
    }
    {
        std::lock_guard lock(m_mediaInfoMutex);
        m_mediaInfo.clear();
    }
    emit mediaInfoChanged();
    m_loadFailed = false;

    // 这个才是"卸掉"。会话结束（挂断投送）时用。
    const char *command[] = {"stop", nullptr};
    mpv_command(m_mpv, command);
}

void MpvCore::seekTo(double seconds)
{
    if (!m_mpv)
        return;
    if (m_imageSequenceActive.load()) {
        m_imageSequence->seek(seconds);
        return;
    }
    const QByteArray target = QByteArray::number(seconds, 'f', 3);
    const char *command[] = {"seek", target.constData(), "absolute", nullptr};
    mpv_command(m_mpv, command);
}

void MpvCore::setVolumePercent(int percent)
{
    if (!m_mpv)
        return;
    double volume = qBound(0, percent, 100);
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &volume);
}

void MpvCore::setMuted(bool muted)
{
    if (!m_mpv)
        return;
    int flag = muted ? 1 : 0;
    mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &flag);
}

qint64 MpvCore::mediaSizeBytes() const
{
    if (!m_mpv)
        return 0;

    // 控制点的流要是没报 Content-Length（vivo 那种临时媒体服务器就常常不报），
    // mpv 这边就是 0 —— 那就如实说"不知道"，让按字节跳转回一个明确的错误，
    // 而不是拿一个瞎猜的比例去跳。
    int64_t size = 0;
    if (mpv_get_property(m_mpv, "file-size", MPV_FORMAT_INT64, &size) < 0)
        return 0;
    return size > 0 ? size : 0;
}

void MpvCore::applyStartupOptions(mpv_handle *mpv)
{
    // ── render API 这条路要设的两项 ──────────────────────────────────────
    //
    // vo=libmpv 是硬性前提。不设它，mpv 会自己去开视频输出 —— 而那时候
    // 渲染上下文还没建好（它要到渲染线程上、GL 上下文当前时才能建），
    // 于是直接报 "No render context set" 然后放弃：视频永远出不来。
    // 这个坑是实测踩出来的，不是从文档上看来的。
    mpv_set_option_string(mpv, "vo", "libmpv");

    // 渲染上下文是 OpenGL 的，GPU 后端也得跟着钉成 OpenGL。不钉的话 mpv 在
    // Windows 上默认去用 d3d11，跟我们的上下文对不上。
    mpv_set_option_string(mpv, "gpu-api", "opengl");
}

QVector<PictureControlInfo> MpvCore::pictureControls() const
{
    QVector<PictureControlInfo> out;
    out.reserve(static_cast<int>(std::size(kPictureControls)));

    for (const PictureControlSpec &spec : kPictureControls) {
        PictureControlInfo info;
        info.name = QString::fromLatin1(spec.name);
        info.min = spec.min;
        info.max = spec.max;
        info.neutral = spec.neutral;
        out.append(info);
    }
    return out;
}

bool MpvCore::setPictureControl(const QString &name, int value)
{
    if (!m_mpv)
        return false;

    const PictureControlSpec *spec = findPictureSpec(name);
    if (!spec)
        return false;

    // 锐度是唯一一个走滤镜的，单独办。
    if (!spec->mpvProperty)
        return applySharpen(qBound(spec->min, value, spec->max));

    // 不加 const：mpv_set_property 收的是 void*，const 指针塞不进去。
    double raw = qBound(spec->min, value, spec->max) * spec->scale;

    // 先按浮点塞。mpv 里有些属性是整数型（video-rotate 就是），浮点塞不进去，
    // 那就换成整数再试一次 —— 表里不用为这种小事多加一个字段。
    int rc = mpv_set_property(m_mpv, spec->mpvProperty, MPV_FORMAT_DOUBLE, &raw);
    if (rc < 0) {
        int64_t asInteger = static_cast<int64_t>(std::llround(raw));
        rc = mpv_set_property(m_mpv, spec->mpvProperty, MPV_FORMAT_INT64, &asInteger);
    }

    if (rc < 0)
        return false;

    // 成了才报。没成就别报 —— 报了控制点那边会显示一个其实没生效的值。
    emit pictureControlChanged(name, qBound(spec->min, value, spec->max));
    return true;
}

int MpvCore::pictureControlValue(const QString &name) const
{
    if (!m_mpv)
        return 0;

    const PictureControlSpec *spec = findPictureSpec(name);
    if (!spec)
        return 0;

    // 锐度走的是滤镜链，它的参数 mpv 不当属性暴露 —— 自己记着（只有界面线程动它）。
    if (!spec->mpvProperty)
        return m_sharpen;

    // 这里是直接问 mpv，没有缓存 —— 按接口的约定本该避免"读个值还要往返一趟"，
    // 但这是**同进程内的**属性读取，没有 I/O，问一次比维护一份缓存更省事也更准。
    double raw = 0.0;
    if (mpv_get_property(m_mpv, spec->mpvProperty, MPV_FORMAT_DOUBLE, &raw) < 0) {
        int64_t asInteger = 0;
        if (mpv_get_property(m_mpv, spec->mpvProperty, MPV_FORMAT_INT64, &asInteger) < 0)
            return spec->neutral;
        raw = static_cast<double>(asInteger);
    }

    return static_cast<int>(std::lround(raw / spec->scale));
}

bool MpvCore::applySharpen(int value)
{
    // 为什么不设 mpv 那个 --sharpen 属性：
    //
    // **实测它是空的**。两张截图逐像素比，--sharpen 从 0 拉到满，画面差 0 个像素。
    // 它是给老 vo=gpu 写的一个着色器钩子，我们用的是 mpv 0.41 的默认 vo（gpu-next），
    // 那条路根本没接。日志里连个警告都没有，就是这么不声不响地不干活。
    //
    // 走视频滤镜链里的 unsharp 就不一样了：它是 ffmpeg 的滤镜，跟在解码后面，
    // 跟用哪个 vo 没关系。同样两张截图比，这一项差 9.6% 的像素。
    //
    // 注意：设 vf 是**整条滤镜链一起换**，不是往里加一道。现在别处没人用 vf，
    // 所以没有冲突；将来要是加了别的滤镜，得改成"拼一串"而不是直接赋值。
    if (value == 0) {
        // 不锐化就把滤镜摘掉，别让它白白占一道工序。
        if (mpv_set_property_string(m_mpv, "vf", "") < 0)
            return false;
    } else {
        // unsharp 的 luma_amount 可用范围是 -2~5：正数锐化、负数模糊。
        // 我们这边是 -100~100，正半边映射到 0.5~3.0，负半边映射到 -2.0~0。
        const double amount = value > 0 ? 0.5 + value * 0.025 : value * 0.02;
        const QString vf = QStringLiteral("unsharp=luma_msize_x=7:luma_msize_y=7:luma_amount=%1")
                               .arg(amount, 0, 'f', 2);
        if (mpv_set_property_string(m_mpv, "vf", vf.toUtf8().constData()) < 0)
            return false;
    }

    m_sharpen = value;
    emit pictureControlChanged(QStringLiteral("sharpen"), value);
    return true;
}
