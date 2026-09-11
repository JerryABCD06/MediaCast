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
    MpvItemRenderer(MpvCore *core, MpvQmlItem *item)
        : m_core(core)
        , m_item(item)
    {
        if (!m_core || !m_core->handle()) {
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

        mpv_render_context_set_update_callback(m_ctx, &MpvItemRenderer::onMpvUpdate, m_item);
    }

    ~MpvItemRenderer() override
    {
        if (m_ctx) {
            mpv_render_context_set_update_callback(m_ctx, nullptr, nullptr);
            mpv_render_context_free(m_ctx);
            m_ctx = nullptr;
        }
    }

    void render() override
    {
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
    static void *getProcAddress(void *, const char *name)
    {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        return ctx ? reinterpret_cast<void *>(ctx->getProcAddress(name)) : nullptr;
    }

    static void onMpvUpdate(void *ctx)
    {
        // 这个回调可能从 mpv 的任意线程进来，必须跳回界面线程再碰 Qt 对象。
        QMetaObject::invokeMethod(static_cast<MpvQmlItem *>(ctx),
                                  "update",
                                  Qt::QueuedConnection);
    }

    MpvCore *m_core = nullptr;
    MpvQmlItem *m_item = nullptr;
    mpv_render_context *m_ctx = nullptr;
};

} // namespace

// ── MpvQmlItem ───────────────────────────────────────────────────────────

MpvQmlItem::MpvQmlItem(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
{
}

MpvQmlItem::~MpvQmlItem() = default;

QQuickFramebufferObject::Renderer *MpvQmlItem::createRenderer() const
{
    return new MpvItemRenderer(m_core, const_cast<MpvQmlItem *>(this));
}

void MpvQmlItem::setCore(MpvCore *core)
{
    if (m_core == core)
        return;

    m_core = core;
    emit coreChanged();

    // 换了播放器就得把渲染上下文重建到新的 mpv 实例上。QQuickFramebufferObject
    // 会在下一帧重新调 createRenderer()，所以这里只要催一帧。
    update();
}
