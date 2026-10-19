/*
 * portty — Taskbar identity shared by both Windows entry points
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef PORTTY_APPMODEL_H
#define PORTTY_APPMODEL_H

/* The taskbar groups windows, shortcuts and pinned buttons by AppUserModelID,
 * and the shortcut Windows invents when the user pins a running window records
 * the ID the window carries.  Every surface that can carry the identity
 * therefore has to carry the same one: the app process (portty.exe, which owns
 * the window), the launcher (porttyw.exe, the pin's relaunch command and the
 * Start Menu shortcut's target), and the Start Menu shortcut's
 * System.AppUserModel.ID property.
 *
 * The value is the documented CompanyName.ProductName form. */
#define PORTTY_APP_USER_MODEL_ID L"Chestso.Portty"

/* The name shown for the app's taskbar button and its pin. */
#define PORTTY_APP_DISPLAY_NAME L"portty"

/* The two Windows entry points. portty.exe is the console-subsystem app that
 * shells and scripts run; porttyw.exe is the GUI-subsystem launcher that
 * Explorer, the Start Menu and a taskbar pin run so no console appears. */
#define PORTTY_APP_EXE      L"portty.exe"
#define PORTTY_LAUNCHER_EXE L"porttyw.exe"

/* Long enough for a \\?\ extended-length module path. */
#define PORTTY_MODULE_PATH_MAX 32768

#endif /* PORTTY_APPMODEL_H */
