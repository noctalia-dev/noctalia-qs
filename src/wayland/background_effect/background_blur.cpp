#include "background_blur.hpp"
#include "manager.hpp"

#include <QGuiApplication>
#include <QQuickWindow>
#include <qpa/qplatformnativeinterface.h>
#include <wayland-client-protocol.h>

// --- wl_compositor singleton via registry ---

static struct wl_compositor *s_compositor = nullptr;

static void registry_global(void */*data*/, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version)
{
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        s_compositor = static_cast<struct wl_compositor *>(
            wl_registry_bind(registry, name, &wl_compositor_interface, qMin(version, 6u)));
}

static void registry_global_remove(void *, struct wl_registry *, uint32_t) {}

static const struct wl_registry_listener s_registry_listener = {
    registry_global,
    registry_global_remove,
};

static struct wl_compositor *ensureCompositor()
{
    if (s_compositor)
        return s_compositor;

    auto *native = QGuiApplication::platformNativeInterface();
    auto *display = static_cast<struct wl_display *>(
        native->nativeResourceForIntegration("wl_display"));
    if (!display)
        return nullptr;

    auto *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &s_registry_listener, nullptr);
    wl_display_roundtrip(display);
    wl_registry_destroy(registry);

    return s_compositor;
}

// --- BackgroundBlur implementation ---

BackgroundBlur::BackgroundBlur(QQuickItem *parent)
    : QQuickItem(parent)
{
    auto *mgr = BackgroundEffectManager::instance();
    m_managerActiveConn = connect(mgr, &QWaylandClientExtension::activeChanged,
                                  this, &BackgroundBlur::tryAttach);
}

BackgroundBlur::~BackgroundBlur()
{
    detach();
    disconnect(m_managerActiveConn);
    disconnect(m_regionChangedConn);
    disconnect(m_regionDestroyedConn);
}

void BackgroundBlur::setBlurRegion(PendingRegion *region)
{
    if (region == m_blurRegion)
        return;

    if (m_blurRegion) {
        disconnect(m_regionChangedConn);
        disconnect(m_regionDestroyedConn);
    }

    m_blurRegion = region;

    if (region) {
        m_regionChangedConn = connect(region, &PendingRegion::changed,
                                      this, &BackgroundBlur::updateBlurRegion);
        m_regionDestroyedConn = connect(region, &QObject::destroyed, this, [this]() {
            m_blurRegion = nullptr;
            updateBlurRegion();
            Q_EMIT blurRegionChanged();
        });
    }

    Q_EMIT blurRegionChanged();
    updateBlurRegion();
}

void BackgroundBlur::componentComplete()
{
    QQuickItem::componentComplete();
    tryAttach();
}

void BackgroundBlur::itemChange(ItemChange change, const ItemChangeData &data)
{
    QQuickItem::itemChange(change, data);

    if (change != ItemSceneChange)
        return;

    detach();

    if (!data.window)
        return;

    if (data.window->isVisible()) {
        QMetaObject::invokeMethod(this, &BackgroundBlur::tryAttach, Qt::QueuedConnection);
    } else {
        m_windowVisibleConn = connect(data.window, &QWindow::visibleChanged,
                                       this, [this](bool visible) {
            if (visible)
                tryAttach();
        });
    }
}

void BackgroundBlur::tryAttach()
{
    if (m_effect)
        return;

    auto *mgr = BackgroundEffectManager::instance();
    if (!mgr->isActive())
        return;

    auto *surface = nativeSurface();
    if (!surface)
        return;

    auto *raw = mgr->get_background_effect(surface);
    if (!raw)
        return;

    m_effect = new QtWayland::ext_background_effect_surface_v1(raw);
    Q_EMIT activeChanged();
    updateBlurRegion();
}

void BackgroundBlur::detach()
{
    if (m_windowVisibleConn) {
        disconnect(m_windowVisibleConn);
        m_windowVisibleConn = {};
    }

    if (!m_effect)
        return;

    m_effect->destroy();
    delete m_effect;
    m_effect = nullptr;

    if (auto *w = window())
        w->requestUpdate();

    Q_EMIT activeChanged();
}

void BackgroundBlur::updateBlurRegion()
{
    if (!m_effect)
        return;

    if (!m_blurRegion) {
        m_effect->set_blur_region(nullptr);
    } else {
        QRegion qregion = m_blurRegion->build();

        if (qregion.isEmpty()) {
            m_effect->set_blur_region(nullptr);
        } else {
            auto *compositor = ensureCompositor();
            if (!compositor)
                return;

            auto *wlRegion = wl_compositor_create_region(compositor);
            for (const QRect &rect : qregion) {
                wl_region_add(wlRegion, rect.x(), rect.y(), rect.width(), rect.height());
            }
            m_effect->set_blur_region(wlRegion);
            wl_region_destroy(wlRegion);
        }
    }

    if (auto *w = window())
        w->requestUpdate();
}

struct wl_surface *BackgroundBlur::nativeSurface()
{
    auto *w = window();
    if (!w)
        return nullptr;
    auto *native = QGuiApplication::platformNativeInterface();
    return static_cast<struct wl_surface *>(
        native->nativeResourceForWindow("surface", w));
}

struct wl_compositor *BackgroundBlur::nativeCompositor()
{
    return ensureCompositor();
}
