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
target_link_libraries(quickshell-win32-wayland PRIVATE Qt::Quick quickshell-win32 dwmapi)
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

qt_add_library(quickshell-win32-mpris STATIC services/mpris.cpp)
qt_add_qml_module(quickshell-win32-mpris
	URI Quickshell.Services.Mpris
	VERSION 0.1
	DEPENDENCIES QtQuick
)
install_qml_module(quickshell-win32-mpris)
target_include_directories(quickshell-win32-mpris PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/services)
# C++/WinRT: exceptions on, coroutine-free (async results are taken with .get() off the GUI thread)
target_compile_options(quickshell-win32-mpris PRIVATE /EHsc)
target_link_libraries(quickshell-win32-mpris PRIVATE Qt::Quick windowsapp)
target_link_libraries(quickshell PRIVATE quickshell-win32-mprisplugin)

qt_add_library(quickshell-win32-notifications STATIC services/notifications.cpp)
qt_add_qml_module(quickshell-win32-notifications
	URI Quickshell.Services.Notifications
	VERSION 0.1
	DEPENDENCIES QtQuick
)
install_qml_module(quickshell-win32-notifications)
target_include_directories(quickshell-win32-notifications PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/services)
target_compile_options(quickshell-win32-notifications PRIVATE /EHsc)
target_link_libraries(quickshell-win32-notifications PRIVATE Qt::Quick windowsapp user32)
target_link_libraries(quickshell PRIVATE quickshell-win32-notificationsplugin)
