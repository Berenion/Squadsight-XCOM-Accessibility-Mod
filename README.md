# Squadsight

An accessibility mod for XCOM: Enemy Unknown and XCOM: Enemy Within. It reads the
game's screens through your screen reader (NVDA, JAWS and others through Tolk, or
SAPI), plays audio cues for walls, doors, windows and units, and lets you play the
tactical map from the keyboard. Enemy Within is the main target.

## Installing

1. Download
   [Squadsight-Setup.exe](https://github.com/Berenion/Squadsight-XCOM-Accessibility-Mod/releases/latest/download/Squadsight-Setup.exe)
   and run it. Windows may warn that it is from an unknown publisher: choose
   **More info**, then **Run anyway**.
2. The setup downloads the latest release from GitHub and installs it in
   `%LOCALAPPDATA%\Programs\Squadsight`, with a Squadsight shortcut on the desktop
   and an entry in Windows' list of installed apps. No administrator rights are
   needed.
3. The launcher opens. Start the game from it (**Enemy Within** or **Enemy
   Unknown**), with Steam running. Later, start it from the desktop shortcut; it
   checks for updates each time.

To uninstall, use the launcher's **Uninstall** button or Windows' list of
installed apps.

## Keys

**Num Lock must be on.** With it off, Windows reports the numpad as the arrows,
Home, End, Page Up and Page Down, and the mod reads it wrong.

### In menus and at the base

| Key | What it does |
|---|---|
| `0` | Read the screen's buttons, with the key for each. It also opens them as a menu: arrows to move, Enter to run one, Escape or `0` to close. Use it on any screen to find out what you can do there. |
| `1`-`9` | Commands the game only gave a gamepad button, on the screens that have them; `0` names them. At the base, `1`-`5` pick a facility, `6` opens Mission Control and `7` the Gollop Chamber. |
| `Delete` | At the base: the date, resources and what is coming up. In the Situation Room it opens the room as a list, in Engineering the build queue, and while choosing an abduction site every country's panic. |
| `Insert` | The list of recent announcements (see below). |

### In a mission: moving around the map

| Key | What it does |
|---|---|
| Numpad `8` `2` `4` `6` | Step the cursor one tile north, south, west, east, and say what is there: cover, how far a move is, who sees it, flanks. |
| Numpad `7` `9` `1` `3` | Step diagonally. |
| Hold a direction | Glide across the map; the tile it stops on is described. The glide speed is in the options menu. |
| Numpad `5` | Where the cursor is, and what is there. |
| Numpad `0` | Confirm: move the soldier to the cursor, or fire an aimed ability at it. |
| `F` / `C` | Move the cursor up / down a storey (the game's own keys). |

### In a mission: the scanner

A list of everything worth knowing about on the map, nearest first. Every
distance is measured from the cursor while you are navigating, otherwise from the
soldier, so it is always the keys still to press.

| Key | What it does |
|---|---|
| `Page Down` / `Page Up` | Next / previous item. |
| `Ctrl` + `Page Down` / `Page Up` | Next / previous category: Everything, Squad, Enemies, Targets, Explosives, Civilians, Doors, Windows, Objectives, Meld, Interactables. |
| `Alt` + `Page Down` / `Page Up` | Next / previous storey, or all storeys. |
| `Home` | Put the cursor on the selected item. |
| `Shift` + `Home` | Put the cursor back on the soldier. |
| `End` | How far the selected item is, and which way. (End no longer ends the turn; Backspace still does.) |
| `Shift` + `End` | How far the soldier is from the cursor: the way back. |

### In a mission: the soldier and the squad

| Key | What it does |
|---|---|
| `Delete` | Everything about the selected soldier: HP, actions, weapon, ammo, status. |
| Numpad `-` | The squad at a glance: each soldier's actions, HP, panic, bleeding out. |
| Numpad `+` | The enemies in sight. |
| `M` | The mission's objectives, and the turn counters ("Turns until Air Strike, 8."). |
| Numpad `.` | The ability bar as a menu: numpad `8` / `2` or Up / Down to walk it, numpad `5` to say the entry again, Enter to use the ability, numpad `.` or Escape to close. |
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
| `F` / `C` | Move the cursor up / down a storey. |
| `Backspace` | End the turn. |
| `F1` | Target information (see above). |
| `W` `A` `S` `D`, `Q` / `E`, `T` / `G` / `Z` | Pan, rotate and zoom the camera. Nothing the mod says depends on the camera, so these can be left alone. |

Three of the game's keys are taken over in a mission: `Home` is the scanner's (it
only centred the camera), `End` is the scanner's (it was a second End Turn key;
`Backspace` still ends the turn), and `Delete` and `Insert` are the mod's (they
moved the camera).

### Sounds and options

| Key | What it does |
|---|---|
| Numpad `*` | Wall sound off / on. |
| Numpad `/` | The mod's options menu: numpad `8` / `2` to choose a setting, `4` / `6` to change it, `5` to switch it or start an entry, `/` to close. Settings are saved in `xcom_uihook_settings.ini` beside the game. |

The options menu has two places to learn the sounds:

- **Sound practice** plays the wall sound on its own. Numpad `8` `2` `4` `6` for a
  wall on one side, `7` `9` `1` `3` for corners, `5` for all four, `0` for
  silence; `+` / `-` to move it further or nearer; `*` / `.` for its volume;
  `/` to go back.
- **Hear the heartbeats** lists the ways allies, aliens, doors, windows and storey
  changes sound. Numpad `8` / `2` choose one, `5` plays it, `/` goes back.

### Without a numpad

| Key | Stands in for |
|---|---|
| Arrows | Numpad `8` `2` `4` `6` (no diagonals) |
| `Space` | Numpad `5` |
| The key right of `P` (`[` on a US layout) | Numpad `0` |
| The key two right of `P` (`]` on US) | Numpad `.` |
| The key right of `L` (`'` on US) | Numpad `+` |
| The key two right of `L` (`\` on US) | Numpad `-` |

These are read by their position, so they work on any keyboard layout. Enter
still opens the shot screen.
