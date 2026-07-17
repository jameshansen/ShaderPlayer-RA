#include "appdata.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

QString dataDir()
{
    static const QString dir = [] {
        QString base = QDir::fromNativeSeparators(qEnvironmentVariable("ProgramData"));
        if (base.isEmpty()) // no ProgramData (or a sandbox): fall back rather than fail
            base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString d = base + QStringLiteral("/ShaderPlayer-RA");
        if (!QDir().mkpath(d))
            qWarning("cannot create data dir %s; settings will not persist", qUtf8Printable(d));
        return d;
    }();
    return dir;
}

QSettings *settings()
{
    static QSettings s(dataDir() + QStringLiteral("/settings.ini"), QSettings::IniFormat);
    return &s;
}

QString playlistPath()
{
    return dataDir() + QStringLiteral("/playlist.m3u8");
}
