#pragma once

#include <qlist.h>
#include <qpixmap.h>
#include <qsize.h>
#include <qstring.h>

struct ParsedDesktopEntryData;

namespace qs::platform {

// Windows: the applications the Start menu lists (shell:AppsFolder, desktop and Store apps)
// as desktop entries. Each entry's id is what Windows groups windows by (AppUserModelID),
// or the executable name for plain desktop apps, so window app ids find their entry.
// Must run on a thread that may initialize COM. Empty elsewhere.
QList<ParsedDesktopEntryData> scanInstalledApps();

// Icon names for Windows apps look like "winapp:<base64url parsing name>:<exe name>".
// They resolve to the icon theme when it has the app (icon pack first, as the user picks a
// pack for a uniform look) and otherwise to the application's own icon from the shell.
bool isAppIconName(const QString& name);
QString appIconName(const QString& parsingName, const QString& exeName);
QPixmap appIconPixmap(const QString& name, QSize size);

} // namespace qs::platform
