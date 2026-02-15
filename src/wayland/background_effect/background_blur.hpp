#pragma once

#include <QQuickItem>
#include "qwayland-ext-background-effect-v1.h"
#include "../../core/region.hpp"

class BackgroundBlur : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT

    /// Region (in surface-local coordinates) to blur.
    /// Setting to null removes the blur effect.
    Q_PROPERTY(PendingRegion* blurRegion READ blurRegion WRITE setBlurRegion NOTIFY blurRegionChanged)

    /// Whether the blur effect is currently active on the surface.
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)

public:
    explicit BackgroundBlur(QQuickItem *parent = nullptr);
    ~BackgroundBlur() override;

    PendingRegion* blurRegion() const { return m_blurRegion; }
    void setBlurRegion(PendingRegion *region);

    bool active() const { return m_effect != nullptr; }

Q_SIGNALS:
    void blurRegionChanged();
    void activeChanged();

protected:
    void componentComplete() override;
    void itemChange(ItemChange change, const ItemChangeData &data) override;

private:
    void tryAttach();
    void detach();
    void updateBlurRegion();

    struct wl_surface *nativeSurface();
    struct wl_compositor *nativeCompositor();

    PendingRegion *m_blurRegion = nullptr;
    QtWayland::ext_background_effect_surface_v1 *m_effect = nullptr;
    QMetaObject::Connection m_windowVisibleConn;
    QMetaObject::Connection m_managerActiveConn;
    QMetaObject::Connection m_regionChangedConn;
    QMetaObject::Connection m_regionDestroyedConn;
};
