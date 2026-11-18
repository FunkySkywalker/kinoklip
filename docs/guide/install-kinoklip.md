# Install and start Kinoklip

Kinoklip is one AppImage: a single file that holds the whole program. There is no installer and no
account.

1. Download **Kinoklip-x86_64.AppImage** from the
   [download page](https://github.com/SeniorMatt/kinoklip/releases/latest). On an ARM64 computer
   the file is called **Kinoklip-aarch64.AppImage**, where a release has one.
2. Make it runnable, in one of two ways:
   - **With Gear Lever (easiest):** install [Gear Lever](https://flathub.org/apps/it.mijorus.gearlever)
     from your software centre, then open the AppImage with it. Kinoklip appears in your
     application menu, and Gear Lever keeps it updated.
   - **By hand:** right-click the file, choose **Properties**, open **Permissions** and tick
     **Is executable**, or run `chmod +x Kinoklip-x86_64.AppImage` in a terminal.
3. Start Kinoklip from the application menu, or double-click the file.

The first window is the **Project Manager**, where you make your first project; the next article,
[Getting started](getting-started.md), takes it from there. The first time, a welcome window also
asks a few short questions; see [Getting started](getting-started.md#the-welcome-window).

## If it does not start

- Your system may be missing FUSE, a small helper for AppImages. Install `fuse3` (or `libfuse2` on
  older distributions), or run `./Kinoklip-x86_64.AppImage --appimage-extract-and-run` in a
  terminal.
- Kinoklip needs a graphics chip that supports OpenGL 3.3. Almost every computer from the last ten
  years has one, integrated graphics included.
- Still stuck? [Send a log file with a bug report](send-a-log-file-with-a-bug-report.md).

## Other ways to get it

Nix, the dev build for testers and building from source are described in
[Installing Kinoklip](../install.md). Kinoklip does not check for updates by itself; Gear Lever
updates the AppImage when you ask it to.

Kinoklip is still a beta version: save often and keep a copy of your original files.

Next: [Getting started](getting-started.md).
