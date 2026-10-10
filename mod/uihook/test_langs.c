// Offline check of the translations' part of an install (install_langs):
// from a build's lang\ folder into a release's flat lang_<CODE>.txt, and from
// those into an install's lang\, with a translation the new version dropped
// removed. All in a folder under %TEMP%; nothing of a real install is touched.
//
//   test_langs.exe  (not test_install: Windows takes "install" in a name to
//   mean an installer and asks for administrator rights)

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "install.h"

void launcher_log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  log: ");
    vprintf(fmt, ap);
    va_end(ap);
}

static int g_fail;

static void check(int ok, const char* what)
{
    printf("%-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) g_fail = 1;
}

static void put(const char* path, const char* text)
{
    FILE* f = NULL;
    if (fopen_s(&f, path, "wb") || !f) return;
    fputs(text, f);
    fclose(f);
}

static int exists(const char* path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static int holds(const char* path, const char* text)
{
    char buf[256] = "";
    FILE* f = NULL;
    if (fopen_s(&f, path, "rb") || !f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strcmp(buf, text) == 0;
}

int main(void)
{
    char root[MAX_PATH], build[MAX_PATH], mod[MAX_PATH], dist[MAX_PATH], inst[MAX_PATH];
    char path[MAX_PATH], err[512];
    GetTempPathA(sizeof root, root);
    strcat_s(root, sizeof root, "squadsight_install_test");
    install_delete_tree(root);
    sprintf_s(mod, sizeof mod, "%s\\uihook", root);
    sprintf_s(build, sizeof build, "%s\\uihook\\build", root);
    sprintf_s(dist, sizeof dist, "%s\\dist", root);
    sprintf_s(inst, sizeof inst, "%s\\install", root);
    CreateDirectoryA(root, NULL);
    CreateDirectoryA(mod, NULL);
    CreateDirectoryA(build, NULL);
    CreateDirectoryA(dist, NULL);
    sprintf_s(path, sizeof path, "%s\\lang", build);
    CreateDirectoryA(path, NULL);
    sprintf_s(path, sizeof path, "%s\\lang", mod);
    CreateDirectoryA(path, NULL);

    // The build has DEU and RUS; mod\uihook a stale DEU (the build's wins)
    // and FRA; and files that are no translation.
    sprintf_s(path, sizeof path, "%s\\lang\\DEU.txt", build);  put(path, "deu build");
    sprintf_s(path, sizeof path, "%s\\lang\\rus.txt", build);  put(path, "rus");
    sprintf_s(path, sizeof path, "%s\\lang\\DEU.txt", mod);    put(path, "deu stale");
    sprintf_s(path, sizeof path, "%s\\lang\\FRA.txt", mod);    put(path, "fra");
    sprintf_s(path, sizeof path, "%s\\lang\\notes.txt", build); put(path, "not a code");
    sprintf_s(path, sizeof path, "%s\\lang\\D1.txt", build);   put(path, "not a code");

    const char* stage_dirs[] = { build, mod };
    int n = install_langs(stage_dirs, 2, dist, TRUE, err, sizeof err);
    check(n == 3, "stage: three translations");
    sprintf_s(path, sizeof path, "%s\\lang_DEU.txt", dist);
    check(holds(path, "deu build"), "stage: the build's DEU, flat, as a release holds it");
    sprintf_s(path, sizeof path, "%s\\lang_RUS.txt", dist);
    check(holds(path, "rus"), "stage: the code in capitals");
    sprintf_s(path, sizeof path, "%s\\lang_FRA.txt", dist);
    check(holds(path, "fra"), "stage: one from the folder above");
    sprintf_s(path, sizeof path, "%s\\lang_NOTES.txt", dist);
    check(!exists(path), "stage: a name that is no code is left out");

    // An older install with a translation this version no longer has.
    sprintf_s(path, sizeof path, "%s\\lang", inst);
    CreateDirectoryA(inst, NULL);
    CreateDirectoryA(path, NULL);
    sprintf_s(path, sizeof path, "%s\\lang\\POL.txt", inst);  put(path, "old pol");
    sprintf_s(path, sizeof path, "%s\\lang\\DEU.txt", inst);  put(path, "old deu");

    // Installing from the downloaded release: the flat files.
    const char* inst_dirs[] = { dist };
    n = install_langs(inst_dirs, 1, inst, FALSE, err, sizeof err);
    check(n == 3, "install: three translations");
    sprintf_s(path, sizeof path, "%s\\lang\\DEU.txt", inst);
    check(holds(path, "deu build"), "install: lang\\DEU.txt replaced");
    sprintf_s(path, sizeof path, "%s\\lang\\RUS.txt", inst);
    check(holds(path, "rus"), "install: lang\\RUS.txt");
    sprintf_s(path, sizeof path, "%s\\lang\\POL.txt", inst);
    check(!exists(path), "install: a translation the version dropped is removed");

    // A version with none leaves no lang folder behind.
    char empty[MAX_PATH];
    sprintf_s(empty, sizeof empty, "%s\\empty", root);
    CreateDirectoryA(empty, NULL);
    const char* none_dirs[] = { empty };
    n = install_langs(none_dirs, 1, inst, FALSE, err, sizeof err);
    sprintf_s(path, sizeof path, "%s\\lang", inst);
    check(n == 0 && !exists(path), "install: none, and the empty folder goes");

    install_delete_tree(root);
    printf(g_fail ? "FAIL\n" : "PASS\n");
    return g_fail;
}
