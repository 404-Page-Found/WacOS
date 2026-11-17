# WacOS

A macOS-style desktop shell for Windows 11. WacOS runs on top of Windows (same kernel, drivers and apps) and
replaces what you see and touch: the menu bar, the Dock, Finder, Spotlight, Control Center, Notification Center,
Mission Control, Launchpad, window title bars and a set of built-in apps. It supports the macOS Sequoia look and
the macOS Tahoe look with Liquid Glass.

Everything is native C++ (Win32, Direct2D, DirectComposition). No Electron, no web views.

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
- **Icon styles** (Default, Dark, Clear, Tinted), accent colors, light and dark appearance.
- **English and Portuguese** interface (English is the default; System Settings > General > Language).

## Install

1. Download `WacOS.zip` from the [latest release](../../releases/latest).
2. Extract it anywhere and run `instalar.cmd`.

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
