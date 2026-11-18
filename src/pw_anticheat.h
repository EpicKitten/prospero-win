/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_ANTICHEAT_H
#define PW_ANTICHEAT_H
/*
 * Recognising a game that ships an anti-cheat prospero-win cannot run.
 *
 * BattlEye protects a game with a Windows kernel driver and a service
 * (BEService) that the game's launcher starts before the game itself. Wine
 * runs no kernel drivers, and the runtime BattlEye provides for Wine is a
 * Linux library the developer must enable per game, which the PS5 cannot
 * load. Such a game would hang or exit at start, or be kicked from its
 * servers; the launcher lists it as not supported instead of starting it.
 *
 * This only recognises the files BattlEye installs beside a game. It does
 * not touch, imitate or work around the anti-cheat.
 *
 * The markers, compared without regard to case:
 *   a name containing "battleye"   the BattlEye directory, arma3battleye.exe
 *   beservice.exe, beservice_x64.exe, beclient.dll, beclient_x64.dll
 *   a name ending in "_be.exe"     the protected start executable
 * They are looked for in the executable's directory and the two above it
 * (BattlEye sits at a game's root while the executable is often in
 * Binaries\Win64), never above the drive's root.
 */
#include <stddef.h>

typedef enum PwAntiCheat { PW_ANTICHEAT_NONE = 0, PW_ANTICHEAT_BATTLEYE } PwAntiCheat;

enum { PW_ANTICHEAT_LEVELS = 3 };

/* Lists directory, calling found(name, length, context) for each entry.
 * 0, or -1 when it cannot be listed (that directory then counts as clean). */
typedef int (*PwAntiCheatList)(const char *directory,
                               void (*found)(const char *name, size_t length, void *context),
                               void *context, void *list_context);

/* The anti-cheat a single directory entry's name marks, if any. */
PwAntiCheat pw_anticheat_name(const char *name, size_t length);

/* The anti-cheat installed with the game whose executable is the Windows
 * path executable, in the Wine prefix at prefix (as pw_wine_launch_host_dir
 * maps it). PW_ANTICHEAT_NONE when none is found or the path cannot be
 * mapped. */
PwAntiCheat pw_anticheat_scan(const char *prefix, const char *executable, PwAntiCheatList list,
                              void *list_context);

/* A short name for logs and the launcher: "battleye", or "none". */
const char *pw_anticheat_label(PwAntiCheat kind);
#endif
