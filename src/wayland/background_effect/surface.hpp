#pragma once

#include <qregion.h>
#include <qtclasshelpermacros.h>
#include <qwayland-ext-background-effect-v1.h>
#include <wayland-client-protocol.h>

namespace qs::wayland::background_effect::impl {

class BackgroundEffectSurface: public QtWayland::ext_background_effect_surface_v1 {
public:
	explicit BackgroundEffectSurface(::ext_background_effect_surface_v1* surface, ::wl_surface* wlSurface);
	~BackgroundEffectSurface() override;
	Q_DISABLE_COPY_MOVE(BackgroundEffectSurface);

	void setBlurRegion(const QRegion& region);
	void commitSurface();
	void setInert();

private:
	bool mInert = false;
	::wl_surface* mWlSurface = nullptr;
};

} // namespace qs::wayland::background_effect::impl
