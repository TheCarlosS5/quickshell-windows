#pragma once

#include <optional>

#include <qbytearray.h>
#include <qstring.h>

namespace qs::platform {

// Linux system files that shells read for status (uptime, memory, CPU, OS name), generated
// from the host OS where they do not exist. Returns nullopt for regular paths.
std::optional<QByteArray> virtualFile(const QString& path);

} // namespace qs::platform
