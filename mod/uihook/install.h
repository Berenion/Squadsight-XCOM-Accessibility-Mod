// Installing, updating and removing the mod.
//
// An install is a folder of its own, %LOCALAPPDATA%\Programs\Squadsight (no
// administrator rights needed), a desktop shortcut to the launcher, and an entry
// in Windows' installed apps list whose Uninstall runs `launcher.exe
// /uninstall`.  The game's folder is never written by the install; the mod's
// settings and log live there because the DLL puts them beside the game's exe,
// and only an uninstall that is asked to touches them.
//
// An update is an install of the new release's own files: the old launcher
// downloads and unpacks it, and the new launcher copies itself in.  So the list
// of files below is always the one that belongs to the version being installed.
#ifndef INSTALL_H
#define INSTALL_H

#include <windows.h>
#include "gamepaths.h"

// Where the mod is installed (from the uninstall entry), or where it would be.
void install_dir(char* out, size_t out_sz);

// TRUE when the uninstall entry exists and its launcher.exe is there.
// version gets the installed version ("" if not installed).
BOOL install_present(char* version, size_t version_sz);

// TRUE when this launcher is the installed one.
BOOL install_running_installed(void);

// Copies the mod's files from src_dir into the install folder, writes the
// uninstall entry and the desktop shortcut.
BOOL install_from(const char* src_dir, char* err, size_t err_sz);

// Whether every file an install needs is in dir.  If not, missing gets the
// first one that is not.  A launcher.exe downloaded on its own from the
// release page has none of them (the 2026-10-09 report from a second
// machine: "xcom_uihook.dll is missing from C:\Users\...\Downloads").
BOOL install_files_beside(const char* dir, char* missing, size_t missing_sz);

// Copies the files a release holds into dir, looking beside src_dir's
// launcher and then one folder up (the NVDA client sits in mod\uihook, the
// build in mod\uihook\build).  For package.bat.
BOOL install_stage(const char* src_dir, const char* dir, char* err, size_t err_sz);

// The translations (strings.h): lang\<CODE>.txt beside the DLL once
// installed, CODE the game's language (DEU, RUS, ...). A release's files are
// GitHub assets, which have no folders, so there each is lang_<CODE>.txt;
// both forms are looked for in each of dirs, the first of a code wins.
// `flat` writes them as a release holds them (lang_<CODE>.txt in dst, for
// install_stage); otherwise into dst\lang, after removing the translations
// there that this version does not have. The count copied, or -1 with err.
int install_langs(const char* const* dirs, int n, const char* dst, BOOL flat,
                  char* err, size_t err_sz);

// Removes the install folder, the shortcut and the uninstall entry.  With
// settings, also the mod's settings, learnt data and logs in the game's
// folders.  When the running launcher is the installed one, its own file and
// the folder are removed by a cmd.exe left behind, a moment after it exits.
BOOL install_remove(const GamePaths* game, BOOL settings, char* err, size_t err_sz);

// The name of a running XCOM process, or NULL.  Neither install nor uninstall
// can replace or remove a DLL the game has loaded.
const char* install_game_running(void);

// Deletes a folder and everything in it.  Missing is not a failure.
BOOL install_delete_tree(const char* dir);

// launcher.log, in the install folder.  Defined in launcher.c.
void launcher_log(const char* fmt, ...);

#endif
