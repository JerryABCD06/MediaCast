// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "NowPlaying.h"

#include <QStringList>
#include <QUrl>
#include <QtGlobal>

QString titleFromUri(const QString &uri)
{
    if (uri.isEmpty())
        return QString();

    // 1. 先统一分隔符再取最后一段。
    //
    // 这里踩过一次：地址可能是 URL（用 /），也可能是 Windows 本地路径（用 \）。
    // 只按 / 切的话，本地路径整条都会被当成"文件名"，
    // 推出一个 "D:\Me\Downloads\Waiting All Night" 这种没法看的标题。
    QString path = uri;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    QString name = path.section(QLatin1Char('/'), -1);

    // 2. 去掉 ?后面那串参数（签名、过期时间之类，不是名字的一部分）
    name = name.section(QLatin1Char('?'), 0, 0);

    // 3. 百分号解码 —— 中文在 URL 里就是 %E5%8E%9F 这种形式
    name = QUrl::fromPercentEncoding(name.toUtf8());

    // 4. 去掉扩展名
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot > 0)
        name = name.left(dot);

    // 5. 分隔符换成空格，读起来顺一点（"Cold-Enough_For_Snow" → "Cold Enough For Snow"）
    name.replace(QLatin1Char('_'), QLatin1Char(' '));
    name.replace(QLatin1Char('-'), QLatin1Char(' '));

    return name.simplified();
}

bool looksLikeATitle(const QString &text)
{
    const QString trimmed = text.simplified();
    if (trimmed.size() < 2)
        return false;

    int letters = 0;
    int digits = 0;
    int currentRun = 0;
    int longestRun = 0;

    for (const QChar ch : trimmed) {
        if (ch.isLetter()) {
            ++letters;
            currentRun = 0;
        } else if (ch.isDigit()) {
            ++digits;
            ++currentRun;
            longestRun = qMax(longestRun, currentRun);
        } else {
            currentRun = 0;
        }
    }

    // 一个字母都没有 —— 肯定是编号，不可能是标题。
    // （汉字也算字母：QChar::isLetter() 对中日韩文字返回真。）
    if (letters == 0)
        return false;

    // 连续五位以上的数字，基本上都是内部编号。
    // "41742108132"、"1000140373" 都栽在这条上；而正常标题里的年份是四位，撞不到。
    if (longestRun >= 5)
        return false;

    // 数字占比超过四成，也不像人起的名字。
    if (digits * 10 > (letters + digits) * 4)
        return false;

    return true;
}

QString mediaKindLabel(MediaKind kind)
{
    switch (kind)
    {
    case MediaKind::Video: return QStringLiteral("视频");
    case MediaKind::Audio: return QStringLiteral("音频");
    case MediaKind::Image: return QStringLiteral("图片");
    case MediaKind::Unknown: break;
    }
    return QString();
}

QString mediaKindKey(MediaKind kind)
{
    switch (kind)
    {
    case MediaKind::Video: return QStringLiteral("media_kind_video");
    case MediaKind::Audio: return QStringLiteral("media_kind_audio");
    case MediaKind::Image: return QStringLiteral("media_kind_image");
    case MediaKind::Unknown: break;
    }
    return QString();
}

bool looksLikePlaceholder(const QString &text)
{
    static const QStringList junk = {
        QStringLiteral("unknown"), QStringLiteral("unkown"),
        QStringLiteral("none"),    QStringLiteral("null"),
        QStringLiteral("n/a"),     QStringLiteral("na"),
        QStringLiteral("-"),       QStringLiteral("--"),
        QStringLiteral("未知"),    QStringLiteral("未知艺术家"),
        QStringLiteral("未知歌手"), QStringLiteral("未知专辑"),
        QStringLiteral("无"),
    };

    // 占位值还有好几种包装，都得认：
    //
    //   unknown            直白型
    //   <unknown>          用尖括号包起来（BubbleUPnP 就是这个）
    //   &lt;unknown&gt;  连尖括号一起转义了 —— 元数据里的实体只解一层，
    //                      到我们手上就是这副样子（日志里实测到的）
    //
    // 所以：把外层的括号/引号剥掉再比。转义那一层的处理在协议层（它才知道
    // 自己那套转义规则），这儿只管"剥壳 + 查表"。
    QString candidate = text.trimmed();
    while (candidate.size() >= 2
           && ((candidate.startsWith(QLatin1Char('<')) && candidate.endsWith(QLatin1Char('>')))
               || (candidate.startsWith(QLatin1Char('"'))
                   && candidate.endsWith(QLatin1Char('"'))))) {
        candidate = candidate.mid(1, candidate.size() - 2).trimmed();
    }

    return junk.contains(candidate.toLower());
}

namespace {

// 三张表放在这儿给两处用：一处是"猜类型"，一处是"去掉扩展名"。
const QStringList &imageExtensions()
{
    static const QStringList exts = {
        QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("png"),
        QStringLiteral("gif"),  QStringLiteral("bmp"),  QStringLiteral("webp"),
        QStringLiteral("heic"), QStringLiteral("heif"), QStringLiteral("tif"),
        QStringLiteral("tiff"), QStringLiteral("avif"),
    };
    return exts;
}

const QStringList &audioExtensions()
{
    static const QStringList exts = {
        QStringLiteral("mp3"),  QStringLiteral("flac"), QStringLiteral("wav"),
        QStringLiteral("m4a"),  QStringLiteral("aac"),  QStringLiteral("ogg"),
        QStringLiteral("opus"), QStringLiteral("wma"),  QStringLiteral("ape"),
    };
    return exts;
}

const QStringList &videoExtensions()
{
    static const QStringList exts = {
        QStringLiteral("mp4"),  QStringLiteral("mkv"), QStringLiteral("avi"),
        QStringLiteral("mov"),  QStringLiteral("wmv"), QStringLiteral("flv"),
        QStringLiteral("webm"), QStringLiteral("ts"),  QStringLiteral("m2ts"),
        QStringLiteral("mpg"),  QStringLiteral("mpeg"), QStringLiteral("m4v"),
        QStringLiteral("3gp"),  QStringLiteral("rmvb"),
    };
    return exts;
}

bool isKnownMediaExtension(const QString &ext)
{
    return imageExtensions().contains(ext) || audioExtensions().contains(ext)
        || videoExtensions().contains(ext);
}

} // namespace

QString stripMediaExtension(const QString &text)
{
    const int dot = text.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0)
        return text;

    const QString ext = text.mid(dot + 1).toLower();
    return isKnownMediaExtension(ext) ? text.left(dot) : text;
}

MediaKind mediaKindFromUri(const QString &uri)
{
    if (uri.isEmpty())
        return MediaKind::Unknown;

    // 先切掉 ?后面那串参数，再把扩展名取出来。
    QString path = uri;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    path = path.section(QLatin1Char('?'), 0, 0);

    const int dot = path.lastIndexOf(QLatin1Char('.'));
    if (dot < 0)
        return MediaKind::Video;   // 没扩展名，按最常见的算

    const QString ext = path.mid(dot + 1).toLower();

    if (imageExtensions().contains(ext))
        return MediaKind::Image;
    if (audioExtensions().contains(ext))
        return MediaKind::Audio;
    return MediaKind::Video;
}
