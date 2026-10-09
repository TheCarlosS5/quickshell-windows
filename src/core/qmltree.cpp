#include "qmltree.hpp"

#include <qdiriterator.h>
#include <qelapsedtimer.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qset.h>

#include "logcat.hpp"
#include "scan.hpp"

namespace qs::core {

namespace {
QS_LOGGING_CATEGORY(logQmlTree, "quickshell.qmltree", QtInfoMsg);

bool sameFile(const QFileInfo& a, const QFileInfo& b) {
	return b.exists() && a.size() == b.size() && a.lastModified() == b.lastModified();
}

// Copies keeping the modification time (it is what Qt's compiled cache checks).
bool copyFile(const QString& from, const QString& to) {
	QFile::remove(to);
	if (!QFile::copy(from, to)) return false;
	QFile out(to);
	if (out.open(QFile::ReadWrite)) {
		out.setFileTime(QFileInfo(from).lastModified(), QFileDevice::FileModificationTime);
		out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser);
	}
	return true;
}

// Writes only when the content differs, so an unchanged file keeps its timestamp.
bool writeIfChanged(const QString& path, const QByteArray& content) {
	QFile file(path);
	if (file.open(QFile::ReadOnly)) {
		if (file.readAll() == content) return true;
		file.close();
	}
	if (!file.open(QFile::WriteOnly | QFile::Truncate)) return false;
	return file.write(content) == content.size();
}
} // namespace

QString materializeQmlTree(const QDir& configRoot, const QmlScanner& scanner, const QDir& cacheDir) {
	QElapsedTimer timer;
	timer.start();

	auto importRoot = QDir(cacheDir.filePath("qmltree"));
	auto mirror = QDir(importRoot.filePath("qs"));
	if (!mirror.mkpath(".")) {
		qCWarning(logQmlTree) << "Could not create" << mirror.path();
		return {};
	}

	auto rootPath = configRoot.absolutePath();
	auto relative = [&](const QString& path) { return QDir(rootPath).relativeFilePath(path); };
	QSet<QString> expected;
	int copied = 0;
	int written = 0;

	// Every file of the config (QML, scripts, assets referenced by relative URLs).
	QDirIterator it(rootPath, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
	while (it.hasNext()) {
		auto source = QFileInfo(it.next());
		auto rel = relative(source.absoluteFilePath());
		if (rel.startsWith(".git/") || rel.endsWith(".qmlc") || rel == ".qmlls.ini") continue;
		if (scanner.fileIntercepts.contains(source.absoluteFilePath())) continue; // written below
		expected.insert(rel);
		auto target = mirror.filePath(rel);
		if (sameFile(source, QFileInfo(target))) continue;
		QDir().mkpath(QFileInfo(target).absolutePath());
		if (!copyFile(source.absoluteFilePath(), target)) {
			qCWarning(logQmlTree) << "Could not copy" << rel;
			return {};
		}
		++copied;
	}

	// What Quickshell synthesizes in memory: qmldir files, preprocessed QML, .qml.json.
	for (auto [path, content]: scanner.fileIntercepts.asKeyValueRange()) {
		if (!QFileInfo(path).absoluteFilePath().startsWith(rootPath)) continue;
		auto rel = relative(path);
		expected.insert(rel);
		auto target = mirror.filePath(rel);
		QDir().mkpath(QFileInfo(target).absolutePath());
		auto data = content.toUtf8();
		QFile existing(target);
		auto changed = !existing.open(QFile::ReadOnly) || existing.readAll() != data;
		existing.close();
		if (changed) {
			if (!writeIfChanged(target, data)) {
				qCWarning(logQmlTree) << "Could not write" << rel;
				return {};
			}
			++written;
		}
	}

	// Files removed from the config leave the mirror too.
	int removed = 0;
	QDirIterator stale(mirror.path(), QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
	while (stale.hasNext()) {
		auto path = stale.next();
		if (!expected.contains(mirror.relativeFilePath(path))) {
			QFile::remove(path);
			++removed;
		}
	}

	qCInfo(logQmlTree) << "QML tree ready in" << timer.elapsed() << "ms:" << copied << "copied," << written
	                   << "synthesized," << removed << "removed, at" << mirror.path();
	return importRoot.absolutePath();
}

} // namespace qs::core
