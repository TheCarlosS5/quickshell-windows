#include "winapps.hpp"

#include "desktopentry.hpp"

#ifdef _WIN32
#include <qcryptographichash.h>
#include <qdatetime.h>
#include <qdir.h>
#include <qfileinfo.h>
#include <qhash.h>
#include <qicon.h>
#include <qimage.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qregularexpression.h>

#include <windows.h>
// clang-format off
#include <objbase.h>
#include <propkey.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
// clang-format on

#include "logcat.hpp"

using Microsoft::WRL::ComPtr;

namespace qs::platform {

QS_LOGGING_CATEGORY(logWinApps, "quickshell.winapps", QtInfoMsg);

namespace {

QString itemString(IShellItem* item, SIGDN kind) {
	LPWSTR value = nullptr;
	if (FAILED(item->GetDisplayName(kind, &value))) return {};
	auto result = QString::fromWCharArray(value);
	CoTaskMemFree(value);
	return result;
}

QString itemProperty(IShellItem* item, const PROPERTYKEY& key) {
	ComPtr<IShellItem2> item2;
	if (FAILED(item->QueryInterface(IID_PPV_ARGS(&item2)))) return {};
	LPWSTR value = nullptr;
	if (FAILED(item2->GetString(key, &value))) return {};
	auto result = QString::fromWCharArray(value);
	CoTaskMemFree(value);
	return result;
}

// "{6D809377-...}\Foo\foo.exe" -> "C:\Program Files\Foo\foo.exe"
QString expandKnownFolder(const QString& path) {
	static const QRegularExpression re(R"(^\{([0-9A-Fa-f-]{36})\}\\(.*)$)");
	auto match = re.match(path);
	if (!match.hasMatch()) return path;
	GUID guid;
	auto guidText = ('{' + match.captured(1) + '}').toStdWString();
	if (FAILED(CLSIDFromString(guidText.c_str(), &guid))) return path;
	PWSTR base = nullptr;
	if (FAILED(SHGetKnownFolderPath(guid, 0, nullptr, &base))) return path;
	auto result = QString::fromWCharArray(base) + '\\' + match.captured(2);
	CoTaskMemFree(base);
	return result;
}

// Common apps -> freedesktop names present in breeze / breeze-plus.
const QHash<QString, QString>& themeIconMap() {
	static const QHash<QString, QString> map = {
	    {"explorer", "system-file-manager"},
	    {"msedge", "microsoft-edge"},
	    {"chrome", "google-chrome"},
	    {"firefox", "firefox"},
	    {"code", "visual-studio-code"},
	    {"windowsterminal", "utilities-terminal"},
	    {"wt", "utilities-terminal"},
	    {"cmd", "utilities-terminal"},
	    {"powershell", "utilities-terminal"},
	    {"pwsh", "utilities-terminal"},
	    {"notepad", "accessories-text-editor"},
	    {"taskmgr", "utilities-system-monitor"},
	    {"calculatorapp", "accessories-calculator"},
	    {"calc", "accessories-calculator"},
	    {"mspaint", "kolourpaint"},
	    {"systemsettings", "preferences-system"},
	    {"control", "preferences-system"},
	    {"snippingtool", "spectacle"},
	    {"photos", "gwenview"},
	    {"obs64", "com.obsproject.Studio"},
	    {"spotify", "spotify"},
	    {"discord", "discord"},
	    {"steam", "steam"},
	    {"vlc", "vlc"},
	    {"telegram", "telegram"},
	    {"whatsapp", "whatsapp"},
	    {"blender", "blender"},
	    {"gimp", "gimp"},
	    {"thunderbird", "thunderbird"},
	    {"teamviewer", "teamviewer"},
	    {"githubdesktop", "github-desktop"},
	    {"studio64", "android-studio"},
	    {"minecraftlauncher", "minecraft-launcher"},
	    // Store apps by package family prefix
	    {"microsoft.windowscalculator", "accessories-calculator"},
	    {"microsoft.windowsterminal", "utilities-terminal"},
	    {"microsoft.windowsnotepad", "accessories-text-editor"},
	    {"microsoft.paint", "kolourpaint"},
	    {"microsoft.screensketch", "spectacle"},
	    {"microsoft.windows.photos", "gwenview"},
	    {"windows.immersivecontrolpanel", "preferences-system"},
	    {"microsoft.windows.explorer", "system-file-manager"},
	};
	return map;
}

QString themeIconFor(const QString& key) {
	auto lower = key.toLower();
	auto prefix = lower.section('_', 0, 0).section('!', 0, 0);
	for (const auto& candidate: {lower, prefix}) {
		if (auto mapped = themeIconMap().value(candidate); !mapped.isEmpty() && QIcon::hasThemeIcon(mapped)) {
			return mapped;
		}
	}
	if (!lower.isEmpty() && QIcon::hasThemeIcon(lower)) return lower;
	return {};
}

// Large enough for every place the shell shows app icons; scaled down on use.
constexpr int CACHED_ICON_SIZE = 128;

QString iconCachePath(const QString& parsingName) {
	static const auto dir = [] {
		auto d = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("ii-windows/cache/appicons");
		QDir().mkpath(d);
		return d;
	}();
	auto hash = QCryptographicHash::hash(parsingName.toUtf8(), QCryptographicHash::Sha1).toHex();
	return QDir(dir).filePath(QString::fromLatin1(hash) + ".png");
}

// Keep Shell's premultiplied BGRA pixels, including alpha. In the deployed Qt
// build, fromHBITMAP followed by reinterpretAsFormat still made corners opaque.
QImage imageFromShellBitmap(HBITMAP bitmap) {
	BITMAP source {};
	if (!GetObjectW(bitmap, sizeof(source), &source) || source.bmWidth <= 0 || source.bmHeight <= 0)
		return {};
	QImage image(source.bmWidth, source.bmHeight, QImage::Format_ARGB32_Premultiplied);
	if (image.isNull()) return {};
	BITMAPINFO format {};
	format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	format.bmiHeader.biWidth = source.bmWidth;
	format.bmiHeader.biHeight = -source.bmHeight; // top-down, like QImage
	format.bmiHeader.biPlanes = 1;
	format.bmiHeader.biBitCount = 32;
	format.bmiHeader.biCompression = BI_RGB;
	auto dc = GetDC(nullptr);
	if (!dc) return {};
	auto rows = GetDIBits(dc, bitmap, 0, source.bmHeight, image.bits(), &format, DIB_RGB_COLORS);
	ReleaseDC(nullptr, dc);
	if (rows != source.bmHeight) return {};
	if (source.bmBitsPixel < 32) {
		// A legacy bitmap has no alpha channel, so its pixels must be opaque.
		for (auto y = 0; y < image.height(); ++y) {
			auto* pixels = reinterpret_cast<QRgb*>(image.scanLine(y));
			for (auto x = 0; x < image.width(); ++x) pixels[x] |= 0xff000000U;
		}
	}
	return image;
}

// QImage, not QPixmap: this also runs on the scanner thread.
QImage shellIcon(const QString& parsingName, QSize size, bool iconOnly = true) {
	ComPtr<IShellItem> item;
	auto path = parsingName.startsWith("shell:") || QFileInfo(parsingName).isAbsolute()
	              ? parsingName
	              : "shell:AppsFolder\\" + parsingName;
	auto wide = path.toStdWString();
	if (FAILED(SHCreateItemFromParsingName(wide.c_str(), nullptr, IID_PPV_ARGS(&item)))) return {};
	ComPtr<IShellItemImageFactory> factory;
	if (FAILED(item.As(&factory))) return {};


	HBITMAP bitmap = nullptr;
	SIZE want {std::max(size.width(), 16), std::max(size.height(), 16)};
	auto flags = static_cast<SIIGBF>(SIIGBF_RESIZETOFIT | (iconOnly ? SIIGBF_ICONONLY : 0));
	if (FAILED(factory->GetImage(want, flags, &bitmap))) {
		// No thumbnail (or it failed): the file type's icon.
		if (iconOnly || FAILED(factory->GetImage(want, SIIGBF_RESIZETOFIT | SIIGBF_ICONONLY, &bitmap))) return {};
	}
	auto image = imageFromShellBitmap(bitmap);
	DeleteObject(bitmap);
	if (image.isNull()) return {};
	return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

} // namespace

QString appIconName(const QString& parsingName, const QString& exeName) {
	auto encoded = QString::fromLatin1(
	    parsingName.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)
	);
	return "winapp:" + encoded + ':' + exeName;
}

bool isAppIconName(const QString& name) { return name.startsWith("winapp:") || name.startsWith("winfile:"); }

// "winfile:<path or base64>": what Explorer shows for that file (thumbnail for images and videos, the
// target's icon for shortcuts, the type's icon otherwise).
QPixmap fileIconPixmap(const QString& path, QSize size) {
	static QHash<QString, QPixmap> memo;
	auto info = QFileInfo(path);
	auto memoKey = info.absoluteFilePath() + '@' + QString::number(info.lastModified().toMSecsSinceEpoch()) + '@'
	             + QString::number(size.width());
	if (auto it = memo.constFind(memoKey); it != memo.constEnd()) return *it;

	auto native = QDir::toNativeSeparators(info.absoluteFilePath());
	auto image = shellIcon(native, size, false);
	auto pixmap = image.isNull() ? QPixmap() : QPixmap::fromImage(image.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	if (memo.size() > 512) memo.clear();
	memo.insert(memoKey, pixmap);
	return pixmap;
}

QPixmap appIconPixmap(const QString& name, QSize size) {
	if (name.startsWith("winfile:")) {
		// A raw path ("C:/..."), or the path's UTF-8 in base64 (safe in image:// URLs).
		auto payload = name.sliced(8);
		auto path = payload.contains(':') ? payload : QString::fromUtf8(QByteArray::fromBase64(payload.toLatin1()));
		return fileIconPixmap(path, size);
	}

	auto parts = name.sliced(7).split(':');
	auto parsingName = QString::fromUtf8(
	    QByteArray::fromBase64(parts.value(0).toLatin1(), QByteArray::Base64UrlEncoding)
	);
	auto exeName = parts.value(1);

	// Resolved icons are reused: search results request them on every keystroke.
	static QHash<QString, QPixmap> memo;
	auto memoKey = name + '@' + QString::number(size.width()) + 'x' + QString::number(size.height());
	if (auto it = memo.constFind(memoKey); it != memo.constEnd()) return *it;

	QPixmap pixmap;
	for (const auto& key: {exeName, parsingName}) {
		if (auto theme = themeIconFor(key); !theme.isEmpty()) {
			pixmap = QIcon::fromTheme(theme).pixmap(size);
			break;
		}
	}

	if (pixmap.isNull()) {
		// Normally already extracted by the background app scan.
		auto cached = iconCachePath(parsingName);
		auto image = QImage(cached);
		if (image.isNull()) {
			image = shellIcon(parsingName, QSize(CACHED_ICON_SIZE, CACHED_ICON_SIZE));
			if (!image.isNull()) image.save(cached);
		}
		if (!image.isNull()) {
			pixmap = QPixmap::fromImage(image.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
		}
	}

	if (!pixmap.isNull()) memo.insert(memoKey, pixmap);
	return pixmap;
}

QList<ParsedDesktopEntryData> scanInstalledApps() {
	QList<ParsedDesktopEntryData> entries;
	auto coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	{
		ComPtr<IShellItem> folder;
		ComPtr<IEnumShellItems> items;
		if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder)))
		    || FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items))))
		{
			qCWarning(logWinApps) << "Cannot enumerate shell:AppsFolder";
		} else {
			static const QRegularExpression uninstaller(R"(^unins|uninstall|desinstal)", QRegularExpression::CaseInsensitiveOption);

			ComPtr<IShellItem> item;
			while (items->Next(1, &item, nullptr) == S_OK) {
				auto name = itemString(item.Get(), SIGDN_NORMALDISPLAY);
				auto parsing = itemString(item.Get(), SIGDN_PARENTRELATIVEPARSING);
				auto target = expandKnownFolder(itemProperty(item.Get(), PKEY_Link_TargetParsingPath));
				if (target.isEmpty() && parsing.contains('\\')) target = expandKnownFolder(parsing);
				item.Reset();
				if (name.isEmpty() || parsing.isEmpty()) continue;

				auto exe = target.endsWith(".exe", Qt::CaseInsensitive) ? QFileInfo(target).completeBaseName() : QString();
				if (uninstaller.match(exe).hasMatch() || uninstaller.match(name).hasMatch()) continue;

				// Desktop apps without an AppUserModelID have path-like parsing names; their
				// windows report the executable name, so that becomes the id.
				auto pathLike = parsing.contains('\\') || parsing.contains(":/");
				auto id = pathLike ? (exe.isEmpty() ? name : exe) : parsing;

				ParsedDesktopEntryData data;
				data.id = id;
				data.name = name;
				data.startupClass = exe.isEmpty() ? parsing.section('!', 0, 0).section('_', 0, 0) : exe;
				data.icon = appIconName(parsing, exe);
				data.command = {"explorer.exe", "shell:AppsFolder\\" + parsing};
				data.execString = "explorer.exe \"shell:AppsFolder\\" + parsing + '"';
				data.workingDirectory = QDir::homePath();
				if (!exe.isEmpty()) data.keywords = {exe};
				entries.append(data);

				// Extract the shell icon off the GUI thread once; the provider reads the cache.
				auto cached = iconCachePath(parsing);
				if (!QFileInfo::exists(cached)) {
					auto image = shellIcon(parsing, QSize(CACHED_ICON_SIZE, CACHED_ICON_SIZE));
					if (!image.isNull()) image.save(cached);
				}
			}
		}
	}

	if (SUCCEEDED(coInit)) CoUninitialize();
	qCInfo(logWinApps) << "Indexed" << entries.size() << "applications";
	return entries;
}

} // namespace qs::platform

#else

namespace qs::platform {
QList<ParsedDesktopEntryData> scanInstalledApps() { return {}; }
bool isAppIconName(const QString& /*name*/) { return false; }
QString appIconName(const QString& /*parsingName*/, const QString& /*exeName*/) { return {}; }
QPixmap appIconPixmap(const QString& /*name*/, QSize /*size*/) { return {}; }
} // namespace qs::platform

#endif
