#include "pdf/HighlightExport.h"
#include "pdf/PopplerBridge.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <cstdio>

namespace reader {
namespace {

QString num(double v) {
    return QString::number(v, 'f', 2);
}

} // namespace

QByteArray buildHighlightOverlay(const std::vector<QSizeF>& pageSizes,
                                 const std::vector<HighlightRow>& rows) {
    if (pageSizes.empty()) return {};
    std::vector<std::vector<QRectF>> byPage(pageSizes.size());
    for (const auto& row : rows) {
        if (row.page < 0 || row.page >= (int)pageSizes.size()) continue;
        const QSizeF& size = pageSizes[row.page];
        if (size.isEmpty() || row.rect.isEmpty()) continue;
        // Content space is bottom-origin: flip y.
        const double x = row.rect.x();
        const double w = row.rect.width();
        const double h = row.rect.height();
        const double y = size.height() - row.rect.y() - h;
        byPage[row.page].push_back(QRectF(x, y, w, h));
    }
    bool any = false;
    for (const auto& list : byPage) any = any || !list.empty();
    if (!any) return {};

    const int n = (int)pageSizes.size();
    std::vector<QByteArray> objs;
    objs.push_back("1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj");
    QByteArray kids;
    for (int i = 0; i < n; ++i) {
        if (i) kids += " ";
        kids += QByteArray::number(3 + i) + " 0 R";
    }
    objs.push_back("2 0 obj << /Type /Pages /Kids [" + kids + "] /Count " +
                   QByteArray::number(n) + " >> endobj");
    for (int i = 0; i < n; ++i)
        objs.push_back(QByteArray::number(3 + i) + " 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 " +
                       num(pageSizes[i].width()).toLatin1() + " " +
                       num(pageSizes[i].height()).toLatin1() + "] /Contents " +
                       QByteArray::number(3 + n + i) + " 0 R /Resources << /ExtGState << /GS0 << /ca 0.45 >> >> >> >> endobj");
    for (int i = 0; i < n; ++i) {
        QByteArray ops = "q /GS0 gs 1 0.88 0.4 rg\n";
        for (const QRectF& r : byPage[i])
            ops += num(r.x()).toLatin1() + " " + num(r.y()).toLatin1() + " " +
                   num(r.width()).toLatin1() + " " + num(r.height()).toLatin1() + " re f\n";
        ops += "Q";
        objs.push_back(QByteArray::number(3 + n + i) + " 0 obj << /Length " +
                       QByteArray::number(ops.size()) + " >> stream\n" + ops +
                       "\nendstream endobj");
    }
    QByteArray out = "%PDF-1.4\n";
    std::vector<int> offs;
    for (const auto& o : objs) {
        offs.push_back(out.size());
        out += o + "\n";
    }
    const int xref = out.size();
    out += "xref\n0 " + QByteArray::number((int)objs.size() + 1) +
           "\n0000000000 65535 f \n";
    char buf[24];
    for (int o : offs) {
        std::snprintf(buf, sizeof buf, "%010d 00000 n \n", o);
        out += buf;
    }
    out += "trailer << /Size " + QByteArray::number((int)objs.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF";
    return out;
}

QString highlightExportName(const QString& sourcePath) {
    const QFileInfo info(sourcePath);
    return info.absolutePath() + "/" + info.completeBaseName() + " - highlighted.pdf";
}

QString embedHighlights(const HighlightExportJob& job) {
    const QString qpdf = QStandardPaths::findExecutable("qpdf");
    if (qpdf.isEmpty()) return "qpdf not found — install qpdf to embed highlights.";
    const QString pdfinfo = QStandardPaths::findExecutable("pdfinfo");
    if (pdfinfo.isEmpty()) return "pdfinfo not found — install poppler-utils to embed highlights.";
    if (!QFile::exists(job.srcPath)) return "Source PDF is gone.";
    // Rotated pages would misplace the overlay: refuse instead of corrupting.
    QSet<int> needed;
    for (const auto& row : job.rows) needed.insert(row.page);
    {
        QProcess info;
        info.start(pdfinfo, {"-f", "1", "-l", QString::number(job.pageSizes.size()),
                             job.srcPath});
        if (!info.waitForFinished(15000)) return "pdfinfo timed out reading the paper.";
        if (info.exitCode() != 0) return "pdfinfo could not read the paper.";
        const QString out = QString::fromLocal8Bit(info.readAllStandardOutput());
        QRegularExpression re("Page\\s+(\\d+)\\s+rot:\\s+(\\d+)");
        for (const auto& line : out.split('\n')) {
            const auto match = re.match(line);
            if (!match.hasMatch()) continue;
            if (needed.contains(match.captured(1).toInt() - 1) &&
                match.captured(2).toInt() != 0)
                return "a highlighted page is rotated — unsupported, nothing written.";
        }
    }
    const QByteArray overlay = buildHighlightOverlay(job.pageSizes, job.rows);
    if (overlay.isEmpty()) return "Nothing to embed (no highlight geometry).";
    const QString tag = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    // Stage next to the destination: same filesystem, so replacing the
    // original is an atomic rename after verification.
    const QString swapDir = QFileInfo(job.destPath).absolutePath();
    const QString overlayPath = swapDir + "/.reader-hl-" + tag + "-overlay.pdf";
    const QString outPath = swapDir + "/.reader-hl-" + tag + "-out.pdf";
    auto cleanup = [&] {
        QFile::remove(overlayPath);
        QFile::remove(outPath);
    };
    {
        QFile f(overlayPath);
        if (!f.open(QIODevice::WriteOnly) || f.write(overlay) != overlay.size()) {
            cleanup();
            return "Could not stage the highlight layer.";
        }
    }
    QProcess qpdfProc;
    qpdfProc.start(qpdf, {job.srcPath, "--overlay", overlayPath, "--", outPath});
    if (!qpdfProc.waitForStarted(10000) || !qpdfProc.waitForFinished(30000)) {
        qpdfProc.kill();
        qpdfProc.waitForFinished(5000);
        cleanup();
        return "qpdf timed out merging the highlights.";
    }
    if (qpdfProc.exitCode() != 0 || !QFile::exists(outPath)) {
        cleanup();
        return "qpdf could not merge the highlights.";
    }
    {
        PopplerBridge check;
        if (!check.open(outPath) || check.pageCount() != (int)job.pageSizes.size()) {
            cleanup();
            return "merged PDF failed verification — nothing written.";
        }
    }
    // Replace destPath via POSIX rename: it swaps the directory entry
    // atomically, so there is never a moment when the paper is missing
    // (QFile::rename refuses to overwrite, which is what forced the old
    // remove-first dance — a failed second step there destroyed the
    // original). If the platform refuses rename-over-existing, fall back
    // to a backup swap that can always restore the original.
    const auto replaceInPlace = [&]() -> QString {
        if (std::rename(QFile::encodeName(outPath).constData(),
                        QFile::encodeName(job.destPath).constData()) == 0)
            return "";
        if (!QFile::exists(job.destPath))
            return "Could not move the merged file into place.";
        const QString backup = swapDir + "/.reader-hl-" + tag + "-backup.pdf";
        if (!QFile::rename(job.destPath, backup))
            return "Could not stage the previous version.";
        if (!QFile::rename(outPath, job.destPath)) {
            QFile::rename(backup, job.destPath); // restore; original survives
            return "Could not write " + job.destPath;
        }
        QFile::remove(backup);
        return "";
    };
    const QString replaceError = replaceInPlace();
    if (!replaceError.isEmpty()) {
        cleanup();
        return replaceError;
    }
    cleanup();
    return "";
}

} // namespace reader
