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
 * NowPlaying —— "正在播放什么"。
 *
 * 界面要用它显示，Windows 的媒体控制面板（那个按音量键弹出来的东西）也要用它。
 * 所以它得是纯数据，不依赖任何一方。
 */
struct NowPlaying
{
    /**
     * 谁送来的。这是**给人看的字**，会直接当副标题显示出来。
     *
     * 两个取值：手机投过来的是「DLNA 投送」，电脑上自己放的是「本地播放」。
     *
     * （以前这里不分青红皂白写死成 "DLNA"，而电脑上那个「播放」按钮走的是同一个
     * 入口，结果本地放的片子副标题也显示 "DLNA" —— 谁也不认识那是谁放的。）
     */
    QString senderName;

    MediaKind kind = MediaKind::Unknown;

    QString title;
    QString artist;
    QString album;

    /** 有没有拿到至少一个标题。界面靠它决定显示"正在播放"还是"等待投送"。 */
    bool hasTitle() const { return !title.isEmpty(); }

    bool isEmpty() const { return title.isEmpty() && artist.isEmpty() && album.isEmpty(); }
};

Q_DECLARE_METATYPE(NowPlaying)

/** 类型的名字：「视频」/「音频」/「图片」。认不出来返回空串。 */
QString mediaKindLabel(MediaKind kind);

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
