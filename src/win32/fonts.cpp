#include "fonts.hpp"

#include <qcoreapplication.h>
#include <qdir.h>
#include <qfont.h>
#include <qfontdatabase.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qstandardpaths.h>

#include "../core/logcat.hpp"

namespace qs::win32 {

QS_LOGGING_CATEGORY(logFonts, "quickshell.win32.fonts", QtInfoMsg);

void loadBundledFonts() {
	// Fonts ship next to the executable and can be added per user, so nothing has to be
	// installed system-wide for the shell itself to look right.
	auto dirs = QStringList {
	    QDir(QCoreApplication::applicationDirPath()).filePath("fonts"),
	    QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("ii-windows/fonts"),
	};

	auto loaded = 0;
	for (const auto& dir: dirs) {
		for (const auto& file: QDir(dir).entryInfoList({"*.ttf", "*.otf", "*.ttc"}, QDir::Files)) {
			auto id = QFontDatabase::addApplicationFont(file.absoluteFilePath());
			if (id < 0) {
				qCWarning(logFonts) << "Could not load font" << file.absoluteFilePath();
				continue;
			}
			++loaded;
			qCDebug(logFonts) << file.fileName() << "->" << QFontDatabase::applicationFontFamilies(id);
		}
	}

	// fontconfig resolves these aliases on Linux; Windows needs them spelled out.
	QFont::insertSubstitution("JetBrains Mono NF", "JetBrainsMono Nerd Font");
	QFont::insertSubstitution("JetBrainsMono NF", "JetBrainsMono Nerd Font");
	QFont::insertSubstitution("JetBrains Mono", "JetBrainsMono Nerd Font");
	QFont::insertSubstitution("monospace", "JetBrainsMono Nerd Font");
	QFont::insertSubstitution("sans-serif", "Google Sans Flex");
	QFont::insertSubstitution("Noto Color Emoji", "Segoe UI Emoji");
	QFont::insertSubstitution("Twemoji", "Segoe UI Emoji");

	qCInfo(logFonts) << "Loaded" << loaded << "bundled fonts";
}

} // namespace qs::win32
