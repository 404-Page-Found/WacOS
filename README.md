# WacOS

A macOS-style desktop shell for Windows 11. WacOS runs on top of Windows (same kernel, drivers and apps) and
replaces what you see and touch: the menu bar, the Dock, Finder, Spotlight, Control Center, Notification Center,
Mission Control, Launchpad, window title bars and a set of built-in apps. It supports the macOS Sequoia look,
the macOS Tahoe look with Liquid Glass and the new macOS 27 **Golden Gate** look.

Everything is native C++ (Win32, Direct2D, DirectComposition). No Electron, no web views.

![WacOS desktop: menu bar, Dock and widgets](docs/screenshots/desktop.png)

## Screenshots

| | |
|---|---|
| ![Control Center in Liquid Glass](docs/screenshots/control-center.png) **Control Center** in Liquid Glass, with real refraction and chromatic aberration | ![Notification Center](docs/screenshots/notification-center.png) **Notification Center** with widgets |
| ![Spotlight](docs/screenshots/spotlight.png) **Spotlight** | ![Spotlight Applications mode](docs/screenshots/spotlight-apps.png) **Applications** (Spotlight's Apps mode, with categories) |
| ![Finder](docs/screenshots/finder.png) **Finder** | ![System Settings](docs/screenshots/system-settings.png) **System Settings** |
| ![Activity Monitor](docs/screenshots/activity-monitor.png) **Activity Monitor** | ![Force Quit Applications](docs/screenshots/force-quit.png) **Force Quit Applications** (Ctrl+Alt+Esc) |
| ![macOS alert](docs/screenshots/alert.png) Windows message boxes become **macOS alerts** | ![Dock icon styles](docs/screenshots/dock-icon-styles.png) **Dock icon styles**: Default, Dark, Clear and Tinted |

### macOS 27 Golden Gate theme (new)

The newest look, matched to Apple's macOS 27 screenshots: frosted Liquid Glass on windows, menus and the Dock,
unified toolbars, the macOS 27 icons and the Golden Gate wallpaper. Pick it in System Settings > Appearance; switching
themes never changes your own wallpaper.

![WacOS desktop in the Golden Gate theme](docs/screenshots/golden-gate-desktop.png)

| | |
|---|---|
| ![Quick Look, Golden Gate](docs/screenshots/golden-gate-quick-look.png) **Quick Look** plays video and music, with a frosted header and an animated open/close | ![Photos, Golden Gate](docs/screenshots/golden-gate-photos.png) **Photos** with search |
| ![Finder, Golden Gate](docs/screenshots/golden-gate-finder.png) **Finder** | ![System Settings, Golden Gate](docs/screenshots/golden-gate-system-settings.png) **System Settings** > Appearance, for every theme |
| ![Notes, Golden Gate](docs/screenshots/golden-gate-notes.png) **Notes** | ![Calendar, Golden Gate](docs/screenshots/golden-gate-calendar.png) **Calendar** |
| ![Reminders, Golden Gate](docs/screenshots/golden-gate-reminders.png) **Reminders** | ![Music, Golden Gate](docs/screenshots/golden-gate-music.png) **Music** with search |
| ![Clock, Golden Gate](docs/screenshots/golden-gate-clock.png) **Clock** with sunrise and sunset times | ![Weather, Golden Gate](docs/screenshots/golden-gate-weather.png) **Weather** |
| ![Activity Monitor, Golden Gate](docs/screenshots/golden-gate-activity-monitor.png) **Activity Monitor** | ![Preview, Golden Gate](docs/screenshots/golden-gate-preview.png) **Preview** |
| ![Calculator, Golden Gate](docs/screenshots/golden-gate-calculator.png) **Calculator** | ![Terminal, Golden Gate](docs/screenshots/golden-gate-terminal.png) **Terminal** |
| ![TextEdit, Golden Gate](docs/screenshots/golden-gate-textedit.png) **TextEdit** | |

Also new with this theme: QuickTime Player with a frosted title bar over the video, the option to hide desktop icons,
**Edit Widgets…** on the desktop menu, clicking the empty strip beside the Dock to show the desktop, Win+D handled by
the shell, grouped notifications that open on click, and smoother glass over animated wallpapers.

**Coming next: a Mac OS X Leopard theme** (Aqua, the glossy 3D Dock with reflections, the translucent menu bar and Stacks).

### macOS Sequoia theme

The same shell with the macOS 15 look (System Settings > Appearance > Theme).

![WacOS desktop in the Sequoia theme](docs/screenshots/sequoia-desktop.png)

| | |
|---|---|
| ![Control Center, Sequoia](docs/screenshots/sequoia-control-center.png) **Control Center** | ![Notification Center, Sequoia](docs/screenshots/sequoia-notification-center.png) **Notification Center** |
| ![Launchpad](docs/screenshots/sequoia-launchpad.png) **Launchpad** | ![Spotlight, Sequoia](docs/screenshots/sequoia-spotlight.png) **Spotlight** |
| ![Finder, Sequoia](docs/screenshots/sequoia-finder.png) **Finder** | ![System Settings, Sequoia](docs/screenshots/sequoia-system-settings.png) **System Settings** with the Sequoia wallpapers |

## Features

- **Menu bar** with the active app's real menus, the Apple menu, status icons and the system tray.
- **Dock** with magnification, running indicators, stacks (Downloads), minimized windows and the Trash.
- **Finder** (file manager), **Spotlight** (search with Applications, Files, Actions and Clipboard modes),
  **Control Center**, **Notification Center**, **Mission Control**, **Launchpad / Apps**, **Quick Look**,
  **Stage Manager** and **Hot Corners**.
- **Mac title bars** (traffic lights, rounded corners, shadows) on other apps' windows.
- **macOS alerts** in place of Windows message boxes, and the Mac Open/Save panel through Finder.
- **Built-in apps**: System Settings, Activity Monitor (with memory pressure), Terminal, Calculator, Notes,
  Reminders, Calendar, TextEdit, Preview, Photos, Music, QuickTime Player, Clock, Weather, Contacts,
  Time Machine, Screenshot and Force Quit.
- **Memory management like macOS**: memory pressure, App Nap and purgeable memory.
- **Themes**: macOS 27 Golden Gate, macOS Tahoe and macOS Sequoia.
- **Icon styles** (Default, Dark, Clear, Tinted), accent colors, light and dark appearance.
- **English and Portuguese** interface (English is the default; System Settings > General > Language).

## Install

1. Download `WacOS.zip` from the [latest release](../../releases/latest).
2. Extract it anywhere and run `instalar.cmd`.

Apple's fonts, sounds and cursors can't be shipped in this repository, so the installer offers to fetch them on
your own PC, from where their owners publish them:

- **SF Pro and SF Mono** from [developer.apple.com/fonts](https://developer.apple.com/fonts/) (Apple's license applies).
  The `.dmg` is opened with 7-Zip, installed through `winget` if missing.
- **macOS alert sounds** from [extratone/macOSsystemsounds](https://github.com/extratone/macOSsystemsounds), converted to WAV.
- **macOS Tahoe cursor** by [jimmyxd2 on DeviantArt](https://www.deviantart.com/jimmyxd2/art/1208233550): the installer opens
  the page, you download it, and it picks the file up from your Downloads folder.

WacOS installs for your user only (no administrator rights) in `%LOCALAPPDATA%\Programs\WacOS`.
Everything it changes is logged in `instalacao.log` there, and `desinstalar.cmd` undoes it.

- Quit WacOS: `Ctrl+Alt+Shift+F12`. Emergency exit (brings Explorer back): `Ctrl+Alt+Shift+F11`.

## Updates

Every commit to `main` is built by GitHub Actions and published as the latest release, with its build number
in `bin\versao.txt`. In **System Settings > General > Software Update**, WacOS checks GitHub on its own, shows the
installed version, the latest one and the commits you don't have yet, and **Update Now** downloads and installs it
(only programs, icons and sounds are replaced; your settings stay).

## Build

Requires Visual Studio 2022 (or Build Tools 2022) with the C++ workload.

```
build.cmd            everything
build.cmd shell      the shell only (WacOSShell.exe: menu bar, Dock, Spotlight...)
build.cmd finder     Finder only
build.cmd janelas    Mac title bars for other apps (WacOSWindow.dll, 32 and 64 bit)
tools\empacotar.ps1  the installer package (dist\WacOS-<date>.zip)
```

## Layout

- `src/shell`: the shell (menu bar, Dock, Spotlight, Control Center, Mission Control, Liquid Glass...).
- `src/finder`, `src/ajustes` (System Settings), `src/monitor` (Activity Monitor) and the other apps in `src/*`.
- `src/janelas`: Mac title bars injected into other apps' windows.
- `src/menu`: context menus on top of the real Windows shell extensions.
- `src/comum`: shared code (names, language, glass, shadows).
- `instalador`: install, update and uninstall scripts. `tools`: build and packaging tools.
- `icones`: icon packs (Sequoia, Tahoe) and SF Symbols.

## Note

Apple, macOS, Finder and related names and icons belong to Apple Inc. This is a personal, non-commercial project
and is not affiliated with Apple. The bundled icons, fonts and sounds are for personal use only.
