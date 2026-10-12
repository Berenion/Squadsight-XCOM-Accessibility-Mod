# Squadsight

An accessibility mod for XCOM: Enemy Unknown and XCOM: Enemy Within. It reads the
game's screens through your screen reader (NVDA, JAWS and others through Tolk, or
SAPI), and shows every line on your braille display as well. It plays audio cues
for walls, doors, windows and units, and lets you play the tactical map from the
keyboard.

## Installing

The mod works with the **Steam** versions of the game and with **GOG**'s
*XCOM: Enemy Unknown Complete Pack*, which has the same executables. It has been tested
mostly with **Enemy Within**. Enemy Unknown is supported too, but it may still
have bugs; please report any you find, as they are usually easy to fix once
known.

1. Download
   [Squadsight-Setup.exe](https://github.com/Berenion/Squadsight-XCOM-Accessibility-Mod/releases/latest/download/Squadsight-Setup.exe)
   and run it. Windows may warn that it is from an unknown publisher: choose
   **More info**, then **Run anyway**.
2. The setup downloads the latest release from GitHub and installs it in
   `%LOCALAPPDATA%\Programs\Squadsight`, with a Squadsight shortcut on the desktop
   and an entry in Windows' list of installed apps. No administrator rights are
   needed.
3. The launcher opens. Start the game from it (**Enemy Within** or **Enemy
   Unknown**), with Steam running if you have the Steam version. Later, start it from the desktop shortcut; it
   checks for updates each time.

To uninstall, use the launcher's **Uninstall** button or Windows' list of
installed apps.

### Manual install

The setup is optional. The launcher runs from any folder and uses the mod files
beside it:

1. From the
   [latest release](https://github.com/Berenion/Squadsight-XCOM-Accessibility-Mod/releases/latest),
   download `Squadsight-<version>.zip` and unpack it into one folder of your
   choice. The same files are also attached one by one, if you would rather
   download them separately: `launcher.exe`, `xcom_uihook.dll`, `inject.exe`,
   `ekgbeep.wav`, `alienbeat.wav`, `doorsound.wav`, `windowsound.wav`,
   `CREDITS.md`, `nvdaControllerClient32.dll`, `Tolk.dll`, `SAAPI32.dll` and
   `dolapi32.dll`.
2. With Steam running (not needed for GOG), start `launcher.exe` and choose the game. To skip that
   choice, start it with `/ew` (Enemy Within) or `/eu` (Enemy Unknown).

Nothing is copied into the game's folder: the mod is loaded into the game each
time the launcher starts it, so starting the game straight from Steam or GOG
runs it without the mod.

- The launcher still checks for updates. To turn that off, start it with
  `/noupdate`, or put a `launcher.ini` beside it containing
  `[launcher]` and, on the next line, `CheckUpdates=0`.
- If the launcher cannot find the game, put the game's folder in that
  `launcher.ini` as `GameRoot=D:\path\to\XCom-Enemy-Unknown`, under
  `[launcher]`.
- The launcher's **Install** button turns a manual copy into a normal install,
  with the desktop shortcut and the entry in Windows' list of installed apps.
- The translations, `lang_<CODE>.txt`, are read from beside the mod as they
  are; a `lang\<CODE>.txt` there instead (what the setup installs) is read first.

## Language

The game has no language option of its own: it plays in the language Steam has
for it (right-click the game in the Steam library, Properties, Language), or
the one GOG installed it in. The launcher reads that setting and starts the game
in it; a language Steam has not
downloaded yet falls back to English.

The mod follows the language the game is set to. Words it reads from the game
(ranks, classes, button labels and the like) are always in that language. The
mod's own lines are in English unless a translation for the game's language is
installed; a translation is a `lang\<CODE>.txt` file beside the mod. The mod
comes with translations for every language the game has: German, French,
Italian, Spanish, Polish, Russian, Japanese, Korean and Traditional Chinese.
They use the game's own terms for its mechanics, but have not been checked by
native speakers yet; corrections are welcome.

## Keys

**Num Lock must be on.** With it off, Windows reports the numpad as the arrows,
Home, End, Page Up and Page Down, and the mod reads it wrong.

**Without a numpad**, a mission's numpad keys have stand-ins, given next to each
key below: the arrows, `Space`, and four keys beside `P` and `L`. Those four are
found by their position, not by what they type, so they work on any keyboard
layout:

- the key right of `P` (`[` on a US layout) for numpad `0`;
- the key two right of `P` (`]` on US) for numpad `.`;
- the key right of `L` (`'` on US) for numpad `+`;
- the key two right of `L` (`\` on US) for numpad `-`.

The arrows only move straight; diagonal steps need the numpad. Enter still opens
the shot screen.

### In menus and at the base

| Key | What it does |
|---|---|
| `0` | Read the screen's buttons, with the key for each. It also opens them as a menu: arrows to move, Enter to run one, Escape or `0` to close. Use it on any screen to find out what you can do there. |
| `1`-`9` | Commands the game only gave a gamepad button, on the screens that have them; `0` names them. At the base, `1`-`5` pick a facility, `6` opens Mission Control and `7` the Gollop Chamber. |
| `Delete` | At the base: the date, resources and what is coming up. In the Situation Room it opens the room as a list, and while choosing an abduction site every country's panic. In Engineering and Research it starts with your scientists, engineers, elerium, alloys and weapon fragments, then Engineering's build queue. |
| `Insert` | The list of recent announcements (see below). |

### In a mission: moving around the map

| Key | What it does |
|---|---|
| Numpad `8` `2` `4` `6`, or the arrows | Step the cursor one tile north, south, west, east, and say what is there: cover, how far a move is, who sees it, flanks. |
| Numpad `7` `9` `1` `3` | Step diagonally. |
| Hold a direction | Glide across the map; the tile it stops on is described. The glide speed is in the options menu. |
| Numpad `5`, or `Space` | Where the cursor is, and what is there. |
| Numpad `0`, or the key right of `P` | Confirm: move the soldier to the cursor. An aimed ability is fired with `Enter`, not with this key: while aiming, it cancels the aim. |
| `F` / `C` | Move the cursor up / down a storey (the game's own keys). For a flying soldier, raise / lower the flight height instead; at the limit it says "Highest flight level." or "Lowest flight level." |

**Flying.** Once a soldier in Archangel armour has used Toggle Flight, the
cursor moves through the air at the flight height: a tile says "In the air, 2
storeys up." A move in flight has no dash, so a tile further than one move says
"2 turns". A move that ends in the air says how high the soldier is hovering,
and a confirm without enough fuel to hover there is refused with "Not enough fuel
to hover there."

### In a mission: the scanner

A list of everything worth knowing about on the map, nearest first. Every
distance is measured from the cursor while you are navigating, otherwise from the
soldier, so it is always the keys still to press.

| Key | What it does |
|---|---|
| `Page Down` / `Page Up` | Next / previous item. |
| `Ctrl` + `Page Down` / `Page Up` | Next / previous category: Everything, Squad, Enemies, Targets, Explosives, Civilians, Doors, Windows, Objectives, Meld, Interactables, High cover. |
| `Alt` + `Page Down` / `Page Up` | Next / previous storey, or all storeys. |
| `Home` | Put the cursor on the selected item. |
| `Shift` + `Home` | Put the cursor back on the soldier. |
| `End` | How far the selected item is, and which way. (End no longer ends the turn; Backspace still does.) |
| `Shift` + `End` | How far the soldier is from the cursor: the way back. |

### In a mission: the soldier and the squad

| Key | What it does |
|---|---|
| `Delete` | Everything about the selected soldier: HP, actions, weapon, ammo, status. |
| Numpad `-`, or the key two right of `L` | The squad at a glance: each soldier's actions, HP, panic, bleeding out. |
| Numpad `+`, or the key right of `L` | The enemies in sight. |
| `M` | The mission's objectives, and the turn counters ("Turns until Air Strike, 8."). |
| Numpad `.`, or the key two right of `P` | The ability bar as a menu: numpad `8` / `2` or Up / Down to walk it, numpad `5` to say the entry again, Enter or `Space` to use the ability, the same key again or Escape to close. |
| `F1` | The target's hit-chance breakdown (the game's screen), read out. Numpad `8` / `2` or Up / Down walk it, numpad `5` says the line again. |
| `Insert` | The list of recent announcements: opens on the newest, numpad `8` / `2` or Up / Down to walk it, numpad `5` to say one again, `Insert` or Escape to close. |

### In a mission: the game's own keys

These are XCOM's keys and work as they always have; the mod speaks what they
change.

| Key | What it does |
|---|---|
| `Tab` / `Left Shift` | Next / previous soldier. While aiming, next / previous target; the target is named with the chance to hit. |
| `F2`-`F9` | Select soldier 1 to 8 directly. |
| `Enter` or `Space` | Open the shot screen with the soldier's abilities; in it, Enter fires or uses the chosen one. |
| `1`-`9`, `0` | Pick ability 1 to 10 from the ability bar (numpad `.` lists them with their numbers). |
| `Escape` | Cancel aiming or close a screen; with nothing open, the pause menu. |
| `Y` | Overwatch. |
| `R` | Reload. |
| `K` | Hunker down: chooses it, then Enter confirms. |
| `X` | Switch weapon; the new weapon and its ammo are named. |
| `V` | Open a door or window, or use a panel or the comm array, next to the soldier. |
| `F` / `C` | Move the cursor up / down a storey, or change the flight height. |
| `Backspace` | End the turn. |
| `F1` | Target information (see above). |
| `W` `A` `S` `D`, `Q` / `E`, `T` / `G` / `Z` | Pan, rotate and zoom the camera. Nothing the mod says depends on the camera, so these can be left alone. |

Some of the game's keys are taken over in a mission: `Home` is the scanner's (it
only centred the camera), `End` is the scanner's (it was a second End Turn key;
`Backspace` still ends the turn), and `Delete` and `Insert` are the mod's (they
moved the camera). While you are navigating with the cursor, the arrows and
`Space` are the cursor's, not the game's.

### Sounds and options

| Key | What it does |
|---|---|
| Numpad `*` | Wall sound off / on. |
| Numpad `/` | The mod's options menu: numpad `8` / `2` to choose a setting, `4` / `6` to change it, `5` to switch it or start an entry, `/` to close. Settings are saved in `xcom_uihook_settings.ini` beside the game. |

These two need the numpad.

The options menu has two places to learn the sounds:

- **Sound practice** plays the wall sound on its own. Numpad `8` `2` `4` `6` for a
  wall on one side, `7` `9` `1` `3` for corners, `5` for all four, `0` for
  silence; `+` / `-` to move it further or nearer; `*` / `.` for its volume;
  `/` to go back.
- **Hear the heartbeats** lists the ways allies, aliens, doors, windows and storey
  changes sound. Numpad `8` / `2` choose one, `5` plays it, `/` goes back.
