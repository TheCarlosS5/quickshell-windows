#pragma once

#include <qdir.h>
#include <qstring.h>

class QmlScanner;

namespace qs::core {

// The config as real files: a mirror of the config folder in which the files Quickshell
// synthesizes (qmldir with singletons, `//@ if` preprocessed QML, .qml.json) are written to disk.
//
// QML loaded through Quickshell's qs: scheme is compiled from source on every start, because
// Qt only keeps compiled QML (the disk cache) for file: URLs. Loading the mirror with file: URLs
// lets Qt reuse it. Files are only rewritten when their content changes, so their timestamps,
// and with them the compiled cache, stay valid between starts.
//
// Returns the mirror's import root (which contains "qs/<config>"), or an empty string if the
// mirror could not be written (then the config loads the usual way).
QString materializeQmlTree(const QDir& configRoot, const QmlScanner& scanner, const QDir& cacheDir);

} // namespace qs::core
