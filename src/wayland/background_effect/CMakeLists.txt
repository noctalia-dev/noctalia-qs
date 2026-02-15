qt_add_library(quickshell-wayland-backgroundeffect STATIC
	manager.cpp
	background_blur.cpp
)

qt_add_qml_module(quickshell-wayland-backgroundeffect
	URI Quickshell.Wayland._BackgroundEffect
	VERSION 0.1
	DEPENDENCIES QtQuick
)

install_qml_module(quickshell-wayland-backgroundeffect)

qs_add_module_deps_light(quickshell-wayland-backgroundeffect Quickshell)

wl_proto(wlp-ext-background-effect ext-background-effect-v1 "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(quickshell-wayland-backgroundeffect PRIVATE
	Qt::Quick Qt::WaylandClient Qt::WaylandClientPrivate wayland-client
	wlp-ext-background-effect
)

qs_module_pch(quickshell-wayland-backgroundeffect SET large)

target_link_libraries(noctalia-qs PRIVATE quickshell-wayland-backgroundeffectplugin)
