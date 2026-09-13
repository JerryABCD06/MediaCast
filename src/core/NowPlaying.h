// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <QMetaType>
#include <QString>

/**
 * 这段内容是什么类型。
 *
 * 不能小看它：Windows 的媒体面板按类型决定卡片长什么样 —— 音乐卡片显示"歌手"，
 * 视频和图片卡片显示"副标题"。一律报成音乐的话，投过来的图片在面板里显示得不对。
 */
enum class MediaKind
{
    Unknown,
    Video,
    Audio,
    Image,
};

/**
 * 内容是从哪儿来的。
 *
 * **这里不带协议名。** 界面上不该让用户看见 "DLNA" 这种词 —— 那是实现细节，
 * 而且以后还会有别的协议。具体是哪个协议，日志里说去。
 */
enum class MediaSource
{
    /** 投送过来的。默认值 —— 这条路上绝大多数都是投送。 */
    Cast,
    /** 在电脑上直接打开的。 */
    Local,
};

/**
 * NowPlaying —— "正在播放什么"。
 *
 * 界面要用它显示，Windows 的媒体控制面板（那个按音量键弹出来的东西）也要用它。
 * 所以它得是纯数据，不依赖任何一方。
 *
 * Q_GADGET + 那几个 Q_PROPERTY 是为了让 QML 能读它（新界面底下那条控制栏的
 * 标题/副标题就是这儿来的）。QML 读不了裸结构体的字段。
 */
struct NowPlaying
{
    Q_GADGET
    Q_PROPERTY(QString title MEMBER title)
    Q_PROPERTY(QString subtitle MEMBER subtitle)
    Q_PROPERTY(QString artist MEMBER artist)
    Q_PROPERTY(QString album MEMBER album)
    Q_PROPERTY(QString lyrics MEMBER lyrics)

public:
    /**
     * 谁送来的。**是个枚举，不是文字** —— 文字由界面那边按语言查。
     *
     * （以前这里不分青红皂白写死成 "DLNA"，而电脑上那个「播放」按钮走的是同一个
     * 入口，结果本地放的片子副标题也显示 "DLNA" —— 谁也不认识那是谁放的。）
     */
    MediaSource source = MediaSource::Cast;

    MediaKind kind = MediaKind::Unknown;

    /**
     * 标题和副标题是**算好的显示文本**（界面直接显示），其余几个是原始字段。
     *
     * 兜底出来的字跟语言走（"未知" / "Unknown"），所以切语言的时候会被重算 ——
     * 见 PlaybackController::retranslate()。
     */
    QString title;

    /**
     * 第二行写什么。**按类型分工，规则在 PlaybackController 里**：
     * 音频看歌手（副标题在音频里没意义），视频和图片看副标题（那儿的"艺术家"
     * 是演员/导演，含义完全不同）。
     */
    QString subtitle;

    QString artist;
    QString album;

    /**
     * 文件里带的歌词。**现在还没有地方显示它**，先读进来备着 ——
     * 歌词只可能在文件里（DLNA 协议没有这一项），而文件里也只有音频可能有。
     */
    QString lyrics;

    /**
     * 有没有拿到至少一个标题。
     *
     * 注意 title 里可能是**兜底**出来的东西（类型名之类），见 PlaybackController
     * 里那段回退链 —— 所以这个为真只说明"有东西可以显示"。
     */
    bool hasTitle() const { return !title.isEmpty(); }

    bool isEmpty() const { return title.isEmpty() && artist.isEmpty() && album.isEmpty(); }
};

Q_DECLARE_METATYPE(NowPlaying)

/**
 * 类型的名字：「视频」/「音频」/「图片」。认不出来返回空串。
 *
 * **中文，而且只给日志用。** 日志不翻译（见 lang/README.md），界面上那份要
 * 走 mediaKindKey() + 语言文件。
 */
QString mediaKindLabel(MediaKind kind);

/**
 * 类型对应的**语言键名**（"media_kind_video" …）。认不出来返回空串。
 *
 * 界面上要显示的类型名从这儿查 —— **映射只此一份**，界面和媒体面板都问它。
 * 具体译文在 lang/*.json 里。
 */
QString mediaKindKey(MediaKind kind);

/**
 * 这个值是不是一条"我没填"的占位符（unknown / none / 未知 ……）。
 *
 * 不是洁癖：实测 vivo 相册投图片时，DIDL 里固定带一条 `upnp:artist = "unkown"`
 * （它自己拼错了）。照单全收地显示出去，面板上就挂着一条 "unkown"，看着像程序
 * 坏了 —— 那是**它的**错，替它背锅没道理。文件标签那边也会遇到同样的东西，
 * 所以这个判断放在这儿，两边共用。
 *
 * 认不出来就返回 false，也就是照常显示：宁可多显示一条可疑的值，也不要把真的
 * 歌手名字误伤掉 —— 所以那张表只收最不可能撞车的几个。
 */
bool looksLikePlaceholder(const QString &text);

/**
 * 去掉末尾的**已知媒体扩展名**："a.mp4" → "a"。
 *
 * 只认已知的那几类，不是见到点就切 —— 否则 "Live at 2.5" 会变成 "Live at 2"。
 *
 * 这个函数是给控制点给的标题用的：很多 App（vivo 相册就是一例）把文件名原样塞进
 * dc:title，连 ".jpg" 都带着。那说明这个字段里装的根本不是标题，是文件名。
 */
QString stripMediaExtension(const QString &text);

/**
 * 从地址的扩展名猜类型。
 *
 * 控制点给的元数据里通常带 upnp:class，那个最准；没有的时候只能看扩展名。
 * 什么都认不出来时按**视频**算 —— 投屏场景里绝大多数就是视频，
 * 猜错的代价只是卡片样式不太对，不影响播放。
 */
MediaKind mediaKindFromUri(const QString &uri);

/**
 * 从媒体地址里推一个标题出来 —— 控制点没给元数据时的兜底。
 *
 * 媒体地址里的文件名常常就是给人看的名字，只是它长这样：
 *
 *     http://192.168.1.5:8080/media/%E5%8E%9F%E7%A5%9E-%E5%AE%A3%E4%BC%A0%E7%89%87.mp4
 *
 * 中文在 URL 里不能直接写，得逐字节转成 %XX，所以要先解码，再去掉扩展名，
 * 得到 "原神-宣传片"。
 */
QString titleFromUri(const QString &uri);

/**
 * 判断推出来的东西"像不像一个真标题"。
 *
 * 这一步不能省。很多 App 的媒体服务器拿内部编号当文件名，推出来是这样的：
 *
 *     41742108132_qe1-1-192       （B 站的 CDN 地址）
 *     push-video-item-1000140373  （vivo 相册）
 *
 * 把这种东西当标题显示出来，比显示"未知"还糟 —— 它占着位置却没有信息。
 * 所以不像就返回 false，让调用方跳过这一层、继续往下找。
 */
bool looksLikeATitle(const QString &text);
