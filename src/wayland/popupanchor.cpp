#include "popupanchor.hpp"

#include <private/qhighdpiscaling_p.h>
#include <private/qwayland-xdg-shell.h>
#include <private/qwaylandwindow_p.h>
#include <private/wayland-xdg-shell-client-protocol.h>
#include <qvariant.h>
#include <qwindow.h>

#include "../core/popupanchor.hpp"
#include "../core/types.hpp"
#include "xdgshell.hpp"

using QtWaylandClient::QWaylandWindow;
using XdgPositioner = QtWayland::xdg_positioner;
using qs::wayland::xdg_shell::XdgWmBase;

// Pre-applies slide constraint adjustment client-side so the popup stays on screen
// even on compositors (e.g. wlroots-based) that do not honor xdg_positioner
// constraint_adjustment for layer-shell popups.
// Returns the translation to apply to the anchor rect (parent-window-relative, logical pixels).
static QPoint computeSlideAdjustmentDelta(
    PopupAnchor* anchor,
    QWindow* window,
    const QRect& anchorRectLogical
) {
	auto adj = anchor->adjustment();
	if (!(adj & PopupAdjustment::Slide)) return {};

	auto* parent = window->transientParent();
	if (!parent) return {};

	auto popupSize = window->geometry().size();
	if (popupSize.isEmpty()) return {};

	// On Wayland, parent->geometry().topLeft() is always (0,0) — windows do not know
	// their global screen position. Work entirely in parent-surface-local coordinates,
	// using the screen dimensions as bounds. This is correct for layer-shell surfaces,
	// which always originate at the screen's top-left corner.
	auto screenSize = parent->screen()->geometry().size();
	QRect localBounds = {0, 0, screenSize.width(), screenSize.height()};

	auto anchorEdges = anchor->edges();
	auto anchorGravity = anchor->gravity();

	// Compute anchor point in parent-local coordinates
	int ax = anchorEdges.testFlag(Edges::Left)  ? anchorRectLogical.left()
	       : anchorEdges.testFlag(Edges::Right) ? anchorRectLogical.right()
	                                            : anchorRectLogical.center().x();
	int ay = anchorEdges.testFlag(Edges::Top)    ? anchorRectLogical.top()
	       : anchorEdges.testFlag(Edges::Bottom) ? anchorRectLogical.bottom()
	                                             : anchorRectLogical.center().y();

	// Compute effective popup top-left (same convention as PopupPositioner::reposition)
	int ex = (anchorGravity.testFlag(Edges::Left)  ? ax - popupSize.width()
	        : anchorGravity.testFlag(Edges::Right) ? ax - 1
	                                               : ax - popupSize.width() / 2) + 1;
	int ey = (anchorGravity.testFlag(Edges::Top)    ? ay - popupSize.height()
	        : anchorGravity.testFlag(Edges::Bottom) ? ay - 1
	                                                : ay - popupSize.height() / 2) + 1;

	int dx = 0, dy = 0;

	// Only apply corrections when the popup starts within the parent surface (ex/ey >= 0).
	// If the popup starts at a negative offset, the parent surface is a narrow side/bottom
	// bar and the popup is intentionally positioned outside the surface (e.g., a left-directed
	// tooltip on a right-side bar). In that case the popup is on-screen globally — don't
	// slide it, because we cannot know the parent's global position and any correction here
	// would move the popup onto the wrong screen.
	if (adj.testFlag(PopupAdjustment::SlideX) && ex >= 0) {
		if (ex + popupSize.width() > localBounds.right())
			dx = localBounds.right() - popupSize.width() + 1 - ex;
		if (ex + dx < localBounds.left())
			dx = localBounds.left() - ex;
	}

	if (adj.testFlag(PopupAdjustment::SlideY) && ey >= 0) {
		if (ey + popupSize.height() > localBounds.bottom())
			dy = localBounds.bottom() - popupSize.height() + 1 - ey;
		if (ey + dy < localBounds.top())
			dy = localBounds.top() - ey;
	}

	return {dx, dy};
}

void WaylandPopupPositioner::reposition(PopupAnchor* anchor, QWindow* window, bool onlyIfDirty) {
	auto* waylandWindow = dynamic_cast<QWaylandWindow*>(window->handle());
	auto* popupRole = waylandWindow ? waylandWindow->surfaceRole<::xdg_popup>() : nullptr;

	anchor->updateAnchor();

	// If a popup becomes invisble after creation ensure the _q properties will
	// be set and not ignored because the rest is the same.
	anchor->updatePlacement({popupRole != nullptr, 0}, window->size());

	if (onlyIfDirty && !anchor->isDirty()) return;
	anchor->markClean();

	if (popupRole) {
		auto* xdgWmBase = XdgWmBase::instance();

		if (xdgWmBase->QtWayland::xdg_wm_base::version() < XDG_POPUP_REPOSITION_SINCE_VERSION) {
			window->setVisible(false);
			WaylandPopupPositioner::setFlags(anchor, window);
			window->setVisible(true);
			return;
		}

		auto positioner = XdgPositioner(xdgWmBase->create_positioner());

		positioner.set_constraint_adjustment(anchor->adjustment().toInt());

		auto anchorRect = anchor->windowRect();

		// Pre-apply slide adjustment for compositors that don't honor constraint_adjustment
		auto delta = computeSlideAdjustmentDelta(anchor, window, anchorRect);
		if (!delta.isNull()) anchorRect.translate(delta);

		if (auto* p = window->transientParent()) {
			anchorRect = QHighDpi::toNativePixels(anchorRect, p);
		}

		positioner
		    .set_anchor_rect(anchorRect.x(), anchorRect.y(), anchorRect.width(), anchorRect.height());

		XdgPositioner::anchor anchorFlag = XdgPositioner::anchor_none;
		switch (anchor->edges()) {
		case Edges::Top: anchorFlag = XdgPositioner::anchor_top; break;
		case Edges::Top | Edges::Right: anchorFlag = XdgPositioner::anchor_top_right; break;
		case Edges::Right: anchorFlag = XdgPositioner::anchor_right; break;
		case Edges::Bottom | Edges::Right: anchorFlag = XdgPositioner::anchor_bottom_right; break;
		case Edges::Bottom: anchorFlag = XdgPositioner::anchor_bottom; break;
		case Edges::Bottom | Edges::Left: anchorFlag = XdgPositioner::anchor_bottom_left; break;
		case Edges::Left: anchorFlag = XdgPositioner::anchor_left; break;
		case Edges::Top | Edges::Left: anchorFlag = XdgPositioner::anchor_top_left; break;
		default: break;
		}

		positioner.set_anchor(anchorFlag);

		XdgPositioner::gravity gravity = XdgPositioner::gravity_none;
		switch (anchor->gravity()) {
		case Edges::Top: gravity = XdgPositioner::gravity_top; break;
		case Edges::Top | Edges::Right: gravity = XdgPositioner::gravity_top_right; break;
		case Edges::Right: gravity = XdgPositioner::gravity_right; break;
		case Edges::Bottom | Edges::Right: gravity = XdgPositioner::gravity_bottom_right; break;
		case Edges::Bottom: gravity = XdgPositioner::gravity_bottom; break;
		case Edges::Bottom | Edges::Left: gravity = XdgPositioner::gravity_bottom_left; break;
		case Edges::Left: gravity = XdgPositioner::gravity_left; break;
		case Edges::Top | Edges::Left: gravity = XdgPositioner::gravity_top_left; break;
		default: break;
		}

		positioner.set_gravity(gravity);
		auto geometry = waylandWindow->geometry();
		positioner.set_size(geometry.width(), geometry.height());

		// Note: this needs to be set for the initial position as well but no compositor
		// supports it enough to test
		positioner.set_reactive();

		xdg_popup_reposition(popupRole, positioner.object(), 0);

		positioner.destroy();
	} else {
		WaylandPopupPositioner::setFlags(anchor, window);
	}
}

// Should be false but nobody supports set_reactive.
// This just tries its best when something like a bar gets resized.
bool WaylandPopupPositioner::shouldRepositionOnMove() const { return true; }

void WaylandPopupPositioner::setFlags(PopupAnchor* anchor, QWindow* window) {
	anchor->updateAnchor();
	auto anchorRect = anchor->windowRect();

	// Pre-apply slide adjustment for compositors that don't honor constraint_adjustment
	auto delta = computeSlideAdjustmentDelta(anchor, window, anchorRect);
	if (!delta.isNull()) anchorRect.translate(delta);

	if (auto* p = window->transientParent()) {
		anchorRect = QHighDpi::toNativePixels(anchorRect, p);
	}

	// clang-format off
	window->setProperty("_q_waylandPopupConstraintAdjustment", anchor->adjustment().toInt());
	window->setProperty("_q_waylandPopupAnchorRect", anchorRect);
	window->setProperty("_q_waylandPopupAnchor", QVariant::fromValue(Edges::toQt(anchor->edges())));
	window->setProperty("_q_waylandPopupGravity", QVariant::fromValue(Edges::toQt(anchor->gravity())));
	// clang-format on
}

void installPopupPositioner() { PopupPositioner::setInstance(new WaylandPopupPositioner()); }
