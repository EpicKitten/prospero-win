/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_anticheat.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A fake file system: directories and the names each lists. */
typedef struct FakeDir { const char *path; const char *const *names; } FakeDir;
typedef struct FakeFs { const FakeDir *dirs; size_t count; char listed[8][320]; size_t calls; } FakeFs;

static int fake_list(const char *directory, void (*found)(const char *, size_t, void *),
                     void *context, void *list_context)
{
    FakeFs *fs = list_context;

    assert(fs->calls < 8);
    snprintf(fs->listed[fs->calls++], sizeof(fs->listed[0]), "%s", directory);
    for (size_t i = 0; i < fs->count; i++) {
        if (strcmp(fs->dirs[i].path, directory)) continue;
        for (const char *const *name = fs->dirs[i].names; *name; name++)
            found(*name, strlen(*name), context);
        return 0;
    }
    return -1;
}

static void test_names(void)
{
    static const char *const marked[] = {
        "BattlEye", "battleye", "BATTLEYE", "arma3battleye.exe", "BEService.exe",
        "beservice_x64.exe", "BEClient.dll", "BEClient_x64.DLL", "DayZ_BE.exe", "TslGame_BE.EXE",
    };
    static const char *const clean[] = {
        "hl2.exe", "_be.exe", "be.exe", "BEService.exe.bak", "Beclient.dl", "battle.net",
        "steam_api64.dll", "DayZ_x64.exe", "",
    };

    for (size_t i = 0; i < sizeof(marked) / sizeof(marked[0]); i++)
        assert(pw_anticheat_name(marked[i], strlen(marked[i])) == PW_ANTICHEAT_BATTLEYE);
    for (size_t i = 0; i < sizeof(clean) / sizeof(clean[0]); i++)
        assert(pw_anticheat_name(clean[i], strlen(clean[i])) == PW_ANTICHEAT_NONE);
    /* The length bounds the name: no reading past it. */
    assert(pw_anticheat_name("BattlEyeX", 6) == PW_ANTICHEAT_NONE);
    assert(pw_anticheat_name(NULL, 0) == PW_ANTICHEAT_NONE);
    assert(!strcmp(pw_anticheat_label(PW_ANTICHEAT_BATTLEYE), "battleye"));
    assert(!strcmp(pw_anticheat_label(PW_ANTICHEAT_NONE), "none"));
}

static void test_scan(void)
{
    static const char *const game_root[] = { "BattlEye", "Engine", "TslGame", NULL };
    static const char *const binaries[] = { "TslGame.exe", "steam_api64.dll", NULL };
    static const char *const hl2[] = { "hl2.exe", "bin", NULL };
    static const char *const drive_root[] = { "BattlEye", NULL };
    static const char *const win64[] = { "Win64", NULL };
    static const char *const games[] = { "HL2", "PUBG", NULL };
    static const char *const games_only[] = { "Games", NULL };
    static const FakeDir dirs[] = {
        { "/p/drive_c/Games/PUBG/TslGame/Binaries/Win64", binaries },
        { "/p/drive_c/Games/PUBG/TslGame/Binaries", win64 },
        { "/p/drive_c/Games/PUBG/TslGame", game_root },
        { "/p/drive_c/Games/HL2", hl2 },
        { "/p/drive_c/Games", games },
        { "/p/drive_c", games_only },
        { "/q/drive_c", drive_root },
    };
    FakeFs fs = { dirs, sizeof(dirs) / sizeof(dirs[0]), { { 0 } }, 0 };

    /* BattlEye two levels above the executable. */
    assert(pw_anticheat_scan("/p", "C:\\Games\\PUBG\\TslGame\\Binaries\\Win64\\TslGame.exe",
                             fake_list, &fs) == PW_ANTICHEAT_BATTLEYE);
    assert(fs.calls == 3 && !strcmp(fs.listed[2], "/p/drive_c/Games/PUBG/TslGame"));

    /* A clean game: three levels, the last C:\ itself, nothing above it. */
    fs.calls = 0;
    assert(pw_anticheat_scan("/p", "C:\\Games\\HL2\\hl2.exe", fake_list, &fs) == PW_ANTICHEAT_NONE);
    assert(fs.calls == 3 && !strcmp(fs.listed[0], "/p/drive_c/Games/HL2") &&
           !strcmp(fs.listed[1], "/p/drive_c/Games") && !strcmp(fs.listed[2], "/p/drive_c"));

    /* A BattlEye directory at the drive's root counts for a game there. */
    fs.calls = 0;
    assert(pw_anticheat_scan("/q", "C:\\game.exe", fake_list, &fs) == PW_ANTICHEAT_BATTLEYE);
    assert(fs.calls == 1 && !strcmp(fs.listed[0], "/q/drive_c"));

    /* The executable's own name is enough; nothing is listed. */
    fs.calls = 0;
    assert(pw_anticheat_scan("/p", "C:\\Games\\DayZ\\DayZ_BE.exe", fake_list, &fs) ==
           PW_ANTICHEAT_BATTLEYE);
    assert(fs.calls == 0);

    /* Z: is the host root, which is never listed. */
    fs.calls = 0;
    assert(pw_anticheat_scan("/p", "Z:\\game.exe", fake_list, &fs) == PW_ANTICHEAT_NONE);
    assert(fs.calls == 0);
    fs.calls = 0;
    assert(pw_anticheat_scan("/p", "Z:\\mnt\\usb\\game.exe", fake_list, &fs) == PW_ANTICHEAT_NONE);
    assert(fs.calls == 2 && !strcmp(fs.listed[0], "/mnt/usb") && !strcmp(fs.listed[1], "/mnt"));

    /* Unlistable directories, unmapped drives and bad arguments are clean. */
    fs.calls = 0;
    assert(pw_anticheat_scan("/p", "C:\\Missing\\Deep\\Dir\\x.exe", fake_list, &fs) ==
           PW_ANTICHEAT_NONE);
    assert(fs.calls == 3);
    assert(pw_anticheat_scan("/p", "D:\\x.exe", fake_list, &fs) == PW_ANTICHEAT_NONE);
    assert(pw_anticheat_scan(NULL, "C:\\x.exe", fake_list, &fs) == PW_ANTICHEAT_NONE);
    assert(pw_anticheat_scan("/p", NULL, fake_list, &fs) == PW_ANTICHEAT_NONE);
    assert(pw_anticheat_scan("/p", "C:\\x.exe", NULL, &fs) == PW_ANTICHEAT_NONE);
    assert(pw_anticheat_scan("/p", "C:", fake_list, &fs) == PW_ANTICHEAT_NONE);
}

int main(void)
{
    test_names();
    test_scan();
    puts("test_pw_anticheat: ok");
    return 0;
}
