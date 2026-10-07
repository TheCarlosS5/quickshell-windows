qt_add_library(quickshell-win32 STATIC
	native.cpp
	panel_window.cpp
	toplevels.cpp
)

qt_add_qml_module(quickshell-win32
	URI Quickshell.Win32
	VERSION 0.1
	DEPENDENCIES QtQuick
)

install_qml_module(quickshell-win32)

add_library(quickshell-win32-init OBJECT init.cpp fonts.cpp)

target_link_libraries(quickshell-win32 PUBLIC Qt::Quick Qt6::QuickPrivate dwmapi shell32 user32 ole32 propsys wtsapi32)
target_link_libraries(quickshell-win32-init PRIVATE Qt::Quick Qt::Qml Qt6::QuickPrivate)

qs_module_pch(quickshell-win32 SET large)

target_link_libraries(quickshell PRIVATE quickshell-win32plugin quickshell-win32-init)

# Compatibility modules: the Wayland/Hyprland APIs shells use, with Windows behaviour.
qt_add_library(quickshell-win32-wayland STATIC compat/wayland.cpp)
qt_add_qml_module(quickshell-win32-wayland
	URI Quickshell.Wayland
	VERSION 0.1
	DEPENDENCIES QtQuick
)
install_qml_module(quickshell-win32-wayland)
target_include_directories(quickshell-win32-wayland PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat)
target_link_libraries(quickshell-win32-wayland PRIVATE Qt::Quick quickshell-win32)
target_link_libraries(quickshell PRIVATE quickshell-win32-waylandplugin)

qt_add_library(quickshell-win32-hyprland STATIC compat/hyprland.cpp)
qt_add_qml_module(quickshell-win32-hyprland
	URI Quickshell.Hyprland
	VERSION 0.1
	DEPENDENCIES QtQuick
)
install_qml_module(quickshell-win32-hyprland)
target_include_directories(quickshell-win32-hyprland PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat)
target_link_libraries(quickshell-win32-hyprland PRIVATE Qt::Quick quickshell-win32)
target_link_libraries(quickshell PRIVATE quickshell-win32-hyprlandplugin)

qt_add_library(quickshell-win32-pipewire STATIC services/pipewire.cpp)
qt_add_qml_module(quickshell-win32-pipewire
	URI Quickshell.Services.Pipewire
	VERSION 0.1
	DEPENDENCIES QtQuick
)
install_qml_module(quickshell-win32-pipewire)
target_include_directories(quickshell-win32-pipewire PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/services)
target_link_libraries(quickshell-win32-pipewire PRIVATE Qt::Quick ole32 version)
target_link_libraries(quickshell PRIVATE quickshell-win32-pipewireplugin)
