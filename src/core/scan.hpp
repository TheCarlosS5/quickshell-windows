#pragma once

#include <qbytearray.h>
#include <qcontainerfwd.h>
#include <qdir.h>
#include <qhash.h>
#include <qjsengine.h>
#include <qloggingcategory.h>
#include <qset.h>
#include <qvector.h>

#include "logcat.hpp"

QS_DECLARE_LOGGING_CATEGORY(logQmlScanner);

// expects canonical paths
class QmlScanner {
public:
	QmlScanner() = default;
	QmlScanner(const QDir& rootPath): rootPath(rootPath) {}

	void scanDir(const QDir& dir);
	void scanQmlRoot(const QString& path);

	// Absolute paths: QDir::operator== resolves canonical paths on the filesystem each time, which
	// on Windows (with antivirus hooks) made scanning ii take ~20 s.
	QSet<QString> scannedDirs;
	QVector<QString> scannedFiles;
	QHash<QString, QByteArray> fileHashes;
	QHash<QString, QString> fileIntercepts;

	struct ScanError {
		QString file;
		QString message;
		int line;
	};

	QVector<ScanError> scanErrors;

	bool readAndHashFile(const QString& path, QByteArray& data);
	[[nodiscard]] bool hasFileContentChanged(const QString& path) const;

private:
	QDir rootPath;

	bool scanQmlFile(const QString& path, bool& singleton, bool& internal);
	bool scanQmlJson(const QString& path);
	[[nodiscard]] static QPair<QString, QString> jsonToQml(const QJsonValue& value, int indent = 0);

	static QJSEngine* preprocEngine();
};
