# qt-libadwaita

The "Libadwaita (GNOME look)" widget style: a Qt style that follows GNOME's libadwaita design.

- Source: https://github.com/SeniorMatt/qt-libadwaita, commit 8ebfa9a
- Licence: GPL-2.0-or-later (COPYING); Kinoklip as a whole is GPL-3.0
- Copied: `src/` without the Qt plugin files (`adwaitastyleplugin.*`, `adwaita.json`), unchanged.
  Kinoklip creates `AdwaitaStyle` directly, so the AppImage and the Flatpak need no plugin path.
- Update: copy `src/` again from a newer commit and change the commit above.
