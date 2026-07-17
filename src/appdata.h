#pragma once

class QSettings;
class QString;

// Everything the app persists lives under %ProgramData%/ShaderPlayer-RA/.
// ponytail: ProgramData is machine-wide, so a non-admin user may not be able to write here and
// the MSIX/Store build will get redirected. Move to %APPDATA% if either bites.
QString dataDir();

// settings.ini in dataDir(). Shared instance; do not delete.
QSettings *settings();

// The saved playlist, as m3u8 so mpv can load it directly with `loadlist`.
QString playlistPath();
