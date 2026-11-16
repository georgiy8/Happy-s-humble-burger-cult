# HHBC Mod

An unofficial mod for **Happy's Humble Burger Cult** (Steam, Windows x64, Unity IL2CPP) with an in-game "book" menu: ticket board, auto-accept, kitchen helpers, after-shift automation, extra player options, visual aids and debugging tools.

> **Status: alpha.** Made for single-player and private co-op sessions. Things can break after a game update. Use it at your own risk.

## Features

The menu is a book with bookmarks (tabs). Open it with **Insert**.

| Tab | What it does |
|---|---|
| **Player** | Infinite stamina, walk/run speed multiplier, jump height multiplier, infinite jump *(experimental, may not work yet)*, custom game speed |
| **Orders** | Auto-accept orders at the register. Accepted orders hang on the right page as **tickets** (normal / drive-thru / pickup): click a ticket and **ALPHA COOKING** builds exactly that order |
| **World** | Full dump of classes, kitchen, recipes and world to text files (for debugging and updating the mod) |
| **Kitchen** | Job lists: grill patties, cookie/pie trays, fries, nuggets, sodas, shakes, bagging. Each button starts an independent list |
| **Lab** | Experimental tools: **BETA CLOSEOUT** (after-shift duties), stock refill, cancel all jobs |
| **Visual** | Fullbright, ESP for enemies and objects |
| **Debug / Keys** | Spy log of your own interactions and appliance calls, marks, and key binds for the toggles |

**ALPHA COOKING** reads the order, opens a bag, cooks patties, builds each burger on the board, adds sides and drinks, closes the bag and hands it to the matching window. Failed steps are retried, and everything is logged.

Every action goes through the same path your own player would use: no NPC methods are called, and the mod does not edit game files.

## Requirements

- Windows 64-bit
- The Steam version of the game (the mod finds classes and fields by name at runtime, so small game updates usually keep working)
- Game built with Unity IL2CPP (tested with Unity 6000.5)

## Installation

1. Download `version.dll` from the [Releases](../../releases) page.
2. Copy it into the game folder, next to the game `.exe`
   (Steam: right click the game → Manage → Browse local files).
3. Start the game and press **Insert**.

**Uninstall:** delete `version.dll` from the game folder.

The mod works as a `version.dll` proxy: Windows loads it from the game folder instead of the system one, and it forwards all real calls to the original library. Only one mod using `version.dll` can be installed at a time.

Files the mod creates next to the game: `hhbc_mod.log` (log), `hhbc_binds.ini` (key binds), `hhbc_*.txt` (dumps from the World tab).

## Usage tips

- **Key binds:** Debug / Keys tab → click a row → press a key. Esc clears it.
- **Spy:** turn it ON in the Debug / Keys tab to write every step of the automation to `hhbc_mod.log` as `MOD >> ...`.
- Start jobs inside a shift. Stations near you are used (every job remembers where you stood when you started it).

## Reporting a bug

Open an [Issue](../../issues) and attach `hhbc_mod.log` (turn **Spy ON** first and reproduce the problem). Please include the game version, what you clicked, and what you expected.

## Building from source

Requirements: Visual Studio (C++ desktop workload, toolset **v145**, or retarget the project), [vcpkg](https://vcpkg.io/) with MinHook.

```bat
vcpkg install minhook:x64-windows minhook:x64-windows-static
```

The project looks for vcpkg in the `VCPKG_ROOT` environment variable, otherwise in `%USERPROFILE%\vcpkg`.

1. Open `HHBCMod.slnx` (or `dll hhbc.vcxproj`).
2. Select **Release | x64** and build.
3. The result is `version.dll`.

Source layout: `overlay.cpp` (book UI, tickets, visuals), `lab.cpp` (job engine: kitchen, ALPHA, BETA), `features.cpp` (player options, orders, key binds), `spy.cpp` (interaction log), `dump.cpp` (dumps), `il2cpp.*` (runtime lookups by name), `version_proxy.cpp` (version.dll forwarding).

## Disclaimer

This is a fan project and is **not affiliated with or endorsed by** the game's developers or publisher. *Happy's Humble Burger Cult* and its assets belong to their owners; no game files or game assets are included here (the book and ticket artwork are original). Use the mod in single-player or in private sessions with players who agree to it, and do not use it to disturb other people's games.

## License

[MIT](LICENSE). Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Release history: [CHANGELOG.txt](CHANGELOG.txt).
