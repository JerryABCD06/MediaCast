#pragma once

#include <QQuickFramebufferObject>

class MpvCore;

/**
 * MpvQmlItem —— 把 mpv 的画面渲染进 QML 的一个 item。
 *
 * ── 和 LibMpvPlayer 的关系 ────────────────────────────────────────────────
 *
 * 两个都是 MpvCore 的壳，区别只在"画面往哪出"：
 *
 *   LibMpvPlayer   走 mpv 的 wid 选项，画面画到一个原生窗口里（旧界面用）
 *   MpvQmlItem     走 mpv 的 render API，画面由 Qt 的场景图合成（新界面用）
 *
 * MpvQmlItem **不继承** MpvCore，只是**持有**一个 —— 因为它得继承
 * QQuickFramebufferObject，而两个 QObject 没法多重继承。所以播放命令和状态
 * 都从 MpvCore 那边走，这个类只管"把画面画出来"。
 *
 * ── 为什么必须用 render API，不能用 wid ───────────────────────────────────
 *
 * wid 要的是一个原生窗口句柄（HWND），而 QML 里每个 item 都没有句柄 ——
 * 整个 QML 窗口才有一个。所以 wid 那条路在 QML 里走不通。
 *
 * 换到 render API 之后有个额外的好处：画面变成**场景图里的一个普通图层**，
 * 可以被别的 QML 控件压住、可以被裁剪、能加半透明遮罩。wid 模式下视频是一个
 * 独立的原生子窗口，永远盖在所有 UI 之上，这些一件都做不到。
 *
 * ── 线程 ─────────────────────────────────────────────────────────────────
 *
 * 渲染上下文的创建和每一帧的渲染都发生在 **Qt 的渲染线程**上，而且那时 GL
 * 上下文是当前的 —— 这是 mpv render API 的硬性要求，不是我们自己选的。
 * 所以渲染器那部分代码不在界面线程上跑，碰数据要小心。
 */
class MpvQmlItem : public QQuickFramebufferObject
{
    Q_OBJECT

    /// 用哪个 MpvCore。由 QML 那边把实例接进来（见 main.cpp 的注册）。
    Q_PROPERTY(MpvCore *core READ core WRITE setCore NOTIFY coreChanged)

public:
    explicit MpvQmlItem(QQuickItem *parent = nullptr);
    ~MpvQmlItem() override;

    Renderer *createRenderer() const override;

    MpvCore *core() const { return m_core; }
    void setCore(MpvCore *core);

signals:
    void coreChanged();

private:
    MpvCore *m_core = nullptr;
};
