qt_add_library(quickshell-win32 STATIC
	native.cpp
	panel_window.cpp
)

qt_add_qml_module(quickshell-win32
	URI Quickshell.Win32
	VERSION 0.1
	DEPENDENCIES QtQuick
)

install_qml_module(quickshell-win32)

add_library(quickshell-win32-init OBJECT init.cpp)

target_link_libraries(quickshell-win32 PRIVATE Qt::Quick Qt6::QuickPrivate dwmapi shell32 user32)
target_link_libraries(quickshell-win32-init PRIVATE Qt::Quick Qt::Qml Qt6::QuickPrivate)

qs_module_pch(quickshell-win32 SET large)

target_link_libraries(quickshell PRIVATE quickshell-win32plugin quickshell-win32-init)
