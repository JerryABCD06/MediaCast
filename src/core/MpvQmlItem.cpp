// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "MpvQmlItem.h"

#include "MpvCore.h"

#include <QDebug>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>

#include <mpv/client.h>
#include <mpv/render_gl.h>

// ── 渲染器 ───────────────────────────────────────────────────────────────
//
// 它活在 Qt 的**渲染线程**上，而且构造和每次 render() 的时候 GL 上下文都是
// 当前的。这正是 mpv render API 要的环境，也是为什么渲染上下文的创建不能
// 放在 MpvQmlItem 的构造函数里（那在界面线程上跑）。

namespace {

class MpvItemRenderer : public QQuickFramebufferObject::Renderer
{
public:
    MpvItemRenderer(MpvCore *core, MpvQmlItem *)
        : m_core(core)
    {
        // 构造和 render() 都是在渲染线程、GL 上下文当前的时候跑的，所以这儿取
        // 到的就是"这块画布用的那个上下文"。
        m_context = QOpenGLContext::currentContext();
        createRenderContext();
    }

    ~MpvItemRenderer() override
    {
        destroyRenderContext();
    }

    void render() override
    {
        // ── GL 上下文换了，就把 mpv 的渲染上下文整个重建 ────────────────────
        //
        // **这一步是必须的。** mpv 的 render API（OpenGL 后端）跟**创建它的那个
        // GL 上下文绑死** —— 它内部的纹理、FBO、着色器程序都是那个上下文里的
        // 对象；mpv 明确要求"渲染时当前的这个上下文必须和创建时是同一个"。
        // 而 Qt 这边**会换上下文**：全屏切换时我们主动让原生窗口重来了一遍
        // （见 NewUiWindow::setFullscreenWindowMode），图形那一套跟着重建，
        // 上下文自然换了一个（实测地址会变，日志里量到过）。
        //
        // 换完之后如果还拿老上下文里的对象去渲染，就是一堆失效句柄：mpv 这一笔
        // 画的就不是画面（也可能把 GL 状态搅乱）。所以：**上下文一变就重建**。
        //
        // 判据就是上下文指针本身。重建很便宜（全屏切换本来就是低频动作），换来
        // 的是"一个 GL 上下文配一个 render context"这条规矩始终成立。
        QOpenGLContext *currentContext = QOpenGLContext::currentContext();
        if (currentContext != m_context) {
            m_context = currentContext;
            destroyRenderContext();
            createRenderContext();
        }

        if (!m_ctx)
            return;

        QOpenGLFramebufferObject *fbo = framebufferObject();
        mpv_opengl_fbo mpvFbo{ static_cast<int>(fbo->handle()), fbo->width(), fbo->height(), 0 };

        // flip_y 要 0，不是 1。
        //
        // 网上很多例子写 1，那是给别种宿主用的。Qt 的 FBO 本来就是以左下角为
        // 原点交给场景图的，设成 1 画面会整个上下颠倒 —— 实测就是这样。
        int flipY = 0;

        mpv_render_param params[] = {
            { MPV_RENDER_PARAM_OPENGL_FBO, &mpvFbo },
            { MPV_RENDER_PARAM_FLIP_Y, &flipY },
            { MPV_RENDER_PARAM_INVALID, nullptr },
        };

        mpv_render_context_render(m_ctx, params);

        // 视频在播就一直要下一帧。不做按需刷新的优化 —— 那种优化要跟 mpv
        // 的"这一帧有没有变"配合，做错了会掉帧，收益却不值。
        update();
    }

private:
    /**
     * 建 mpv 的渲染上下文。
     *
     * **必须在渲染线程上、且 GL 上下文当前的时候调** —— mpv 会在这里把 GL 那
     * 一套函数指针（`getProcAddress`）抄走，之后再拿它们建自己的纹理/FBO/程序。
     * 也就是说：**一个 GL 上下文配一个 render context**，换上下文就得整份重建
     * （见 render() 开头那段）。
     */
    void createRenderContext()
    {
        if (m_ctx || !m_core || !m_core->handle()) {
            if (!m_ctx && (!m_core || !m_core->handle()))
                qWarning() << "[mpv-qml] 没有可用的 mpv 实例，画面出不来";
            return;
        }

        mpv_opengl_init_params glInit{ &MpvItemRenderer::getProcAddress, nullptr };

        // 这就是"用哪个渲染后端"的全部内容。将来 mpv 要是支持了别的后端
        // （D3D11 / Vulkan 之类），换的是这个字符串和上面那个参数结构 ——
        // 别处不用动，这也是把这段单独收在一处的原因。
        mpv_render_param params[] = {
            { MPV_RENDER_PARAM_API_TYPE,
              const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL) },
            { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit },
            { MPV_RENDER_PARAM_INVALID, nullptr },
        };

        const int rc = mpv_render_context_create(&m_ctx, m_core->handle(), params);
        if (rc < 0) {
            qWarning() << "[mpv-qml] mpv_render_context_create 失败:" << mpv_error_string(rc);
            m_ctx = nullptr;
            return;
        }

        // **回调的上下文给 MpvCore，不给这个 item。**
        //
        // 这个回调是从**渲染线程**发出来的，而画画的 item 随时可能在界面线程上
        // 被销毁（切页面、关窗口都会）。原来这里传的是 item 的裸指针，回调里
        // 直接拿它去 invokeMethod —— 那是一次正经的野指针访问，实测崩在 Qt6Core：
        //
        //     MpvItemRenderer::onMpvUpdate -> QMetaObject::invokeMethod -> 崩
        //
        // MpvCore 活到程序结束，invoke 它什么时候都安全；到了界面线程再由它发
        // 信号，真正接收的 item 已经不在了的话，Qt 自己会把那条连接摘掉。
        mpv_render_context_set_update_callback(m_ctx, &MpvItemRenderer::onMpvUpdate, m_core);

        // 告诉核心：渲染面挂上来了，攒着的片子可以放了。
        //
        // 这里在渲染线程上，所以用队列连接跳回界面线程 —— setRendererAttached
        // 会去碰"攒着的那条地址"并发 loadfile，那些都得在界面线程做。
        QMetaObject::invokeMethod(m_core, "setRendererAttached", Qt::QueuedConnection,
                                  Q_ARG(bool, true));
    }

    void destroyRenderContext()
    {
        if (!m_ctx)
            return;

        mpv_render_context_set_update_callback(m_ctx, nullptr, nullptr);
        mpv_render_context_free(m_ctx);
        m_ctx = nullptr;
    }

    static void *getProcAddress(void *, const char *name)
    {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        return ctx ? reinterpret_cast<void *>(ctx->getProcAddress(name)) : nullptr;
    }

    static void onMpvUpdate(void *ctx)
    {
        // 这个回调可能从 mpv 的任意线程进来，必须跳回界面线程再碰 Qt 对象。
        // 这里**只碰 MpvCore**（长期存活），不碰 item，理由见上面那段注释。
        QMetaObject::invokeMethod(static_cast<MpvCore *>(ctx),
                                  "notifyRenderUpdate",
                                  Qt::QueuedConnection);
    }

    MpvCore *m_core = nullptr;
    mpv_render_context *m_ctx = nullptr;
    /** 建 m_ctx 时那个 GL 上下文。它一变就得重建（见 render() 开头那段）。 */
    QOpenGLContext *m_context = nullptr;
};

} // namespace

// ── MpvQmlItem ───────────────────────────────────────────────────────────

MpvQmlItem::MpvQmlItem(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
{
}

MpvQmlItem::~MpvQmlItem()
{
    // 画面 item 没了 —— 告诉核心"渲染面不在了"。窗口被关掉又重开的话，
    // 新的 item 会把渲染上下文重新建起来、再报一次 true。
    //
    // 这个标记不清掉的话，中间那段空档里来的投屏会以为渲染面还在，
    // 直接把 loadfile 发出去 —— 然后 mpv 报 "No render context set" 放弃。
    if (m_core)
        m_core->setRendererAttached(false);
}

QQuickFramebufferObject::Renderer *MpvQmlItem::createRenderer() const
{
    return new MpvItemRenderer(m_core, const_cast<MpvQmlItem *>(this));
}

void MpvQmlItem::setCore(MpvCore *core)
{
    if (m_core == core)
        return;

    // 换核心的时候把旧的那条线摘掉。不摘的话旧核心（如果还活着）发信号，
    // 这边照样重画 —— 画面就归错的播放器管了。
    if (m_core)
        disconnect(m_core, &MpvCore::renderUpdate, this, nullptr);

    m_core = core;
    emit coreChanged();

    if (m_core) {
        // mpv 有新帧就重画一次。信号是 MpvCore 在**界面线程**上发出来的
        // （渲染线程的回调先被转成队列调用，见 MpvCore::notifyRenderUpdate），
        // 所以直连即可；item 析构时 Qt 自动断开这条连接 —— 这正是把回调绕到
        // MpvCore 上的意义：mpv 那边永远不会拿到这个 item 的指针。
        connect(m_core, &MpvCore::renderUpdate, this, [this] { update(); });
    }

    // 换了播放器就得把渲染上下文重建到新的 mpv 实例上。QQuickFramebufferObject
    // 会在下一帧重新调 createRenderer()，所以这里只要催一帧。
    update();
}
