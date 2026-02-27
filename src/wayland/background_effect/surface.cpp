#include "surface.hpp"

#include <private/qwaylanddisplay_p.h>
#include <private/qwaylandintegration_p.h>
#include <qregion.h>
#include <qwayland-ext-background-effect-v1.h>
#include <wayland-client-protocol.h>

namespace qs::wayland::background_effect::impl {

BackgroundEffectSurface::BackgroundEffectSurface(
    ::ext_background_effect_surface_v1* surface, // NOLINT(misc-include-cleaner)
    ::wl_surface* wlSurface                      // NOLINT(misc-include-cleaner)
)
    : QtWayland::ext_background_effect_surface_v1(surface), mWlSurface(wlSurface) {}

BackgroundEffectSurface::~BackgroundEffectSurface() {
	if (!this->isInitialized()) return;

	if (this->mInert) {
		wl_proxy_destroy(reinterpret_cast<wl_proxy*>(this->object())); // NOLINT(misc-include-cleaner)
	} else {
		this->destroy();
	}
}

void BackgroundEffectSurface::setInert() { this->mInert = true; }

void BackgroundEffectSurface::commitSurface() {
	if (this->mInert || !this->mWlSurface) return;
	wl_surface_commit(this->mWlSurface); // NOLINT(misc-include-cleaner)
}

void BackgroundEffectSurface::setBlurRegion(const QRegion& region) {
	if (!this->isInitialized()) return;

	if (region.isEmpty()) {
		this->set_blur_region(nullptr);
		return;
	}

	static const auto* waylandIntegration = QtWaylandClient::QWaylandIntegration::instance();
	auto* display = waylandIntegration->display();

	auto* wlRegion = display->createRegion(region);
	this->set_blur_region(wlRegion);
	wl_region_destroy(wlRegion); // NOLINT(misc-include-cleaner)
}

} // namespace qs::wayland::background_effect::impl
