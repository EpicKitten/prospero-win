/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_anticheat.h"
#include "pw_wine_launch.h"
#include <string.h>

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* name[0..length) equals the lower-case word, ignoring case. */
static int equals(const char *name, size_t length, const char *word)
{
    size_t n = strlen(word);

    if (length != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (lower(name[i]) != word[i]) return 0;
    return 1;
}

static int contains(const char *name, size_t length, const char *word)
{
    size_t n = strlen(word);

    for (size_t at = 0; at + n <= length; at++) {
        size_t i = 0;
        while (i < n && lower(name[at + i]) == word[i]) i++;
        if (i == n) return 1;
    }
    return 0;
}

static int ends_with(const char *name, size_t length, const char *word)
{
    size_t n = strlen(word);

    return length > n && equals(name + length - n, n, word);
}

PwAntiCheat pw_anticheat_name(const char *name, size_t length)
{
    static const char *const files[] = {
        "beservice.exe", "beservice_x64.exe", "beclient.dll", "beclient_x64.dll",
    };

    if (!name) return PW_ANTICHEAT_NONE;
    if (contains(name, length, "battleye") || ends_with(name, length, "_be.exe"))
        return PW_ANTICHEAT_BATTLEYE;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        if (equals(name, length, files[i])) return PW_ANTICHEAT_BATTLEYE;
    return PW_ANTICHEAT_NONE;
}

static void found_name(const char *name, size_t length, void *context)
{
    PwAntiCheat *kind = context;

    if (*kind == PW_ANTICHEAT_NONE) *kind = pw_anticheat_name(name, length);
}

PwAntiCheat pw_anticheat_scan(const char *prefix, const char *executable, PwAntiCheatList list,
                              void *list_context)
{
    char directory[PW_WINE_LAUNCH_PATH_MAX + 256], root[PW_WINE_LAUNCH_PATH_MAX + 256];
    char drive[4] = { 0, ':', '\\', 0 };
    PwAntiCheat kind = PW_ANTICHEAT_NONE;
    const char *base;
    size_t root_length;

    if (!prefix || !executable || !list || strlen(executable) < 3) return PW_ANTICHEAT_NONE;
    base = strrchr(executable, '\\');
    base = base ? base + 1 : executable;
    if ((kind = pw_anticheat_name(base, strlen(base))) != PW_ANTICHEAT_NONE) return kind;

    drive[0] = executable[0];
    if (pw_wine_launch_host_dir(prefix, executable, directory, sizeof(directory)) ||
        pw_wine_launch_host_dir(prefix, drive, root, sizeof(root)))
        return PW_ANTICHEAT_NONE;
    root_length = strlen(root);
    for (int level = 0; level < PW_ANTICHEAT_LEVELS && kind == PW_ANTICHEAT_NONE; level++) {
        /* Up one: the executable's directory first, then its parents. */
        char *slash = strrchr(directory, '/');
        if (!slash || (size_t)(slash - directory) < root_length) break;
        if (slash == directory) slash[1] = 0;
        else *slash = 0;
        (void)list(directory, found_name, &kind, list_context);
        if (strlen(directory) <= root_length) break;
    }
    return kind;
}

const char *pw_anticheat_label(PwAntiCheat kind)
{
    return kind == PW_ANTICHEAT_BATTLEYE ? "battleye" : "none";
}
