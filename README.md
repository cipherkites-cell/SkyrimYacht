# Skyrim Yacht

A tavern dice game for Skyrim SE, AE and GOG: shake a real dice cup, tip five dice onto a felt tray, keep the ones
you like and re-roll the rest, then score them on a Yacht scorecard.

Status: **physics test**. The dice simulation and the scoring rules are done and tested; the SKSE plugin comes next.

## How it works

Like Playable Pool, the dice never touch the game's own physics. `src/sim/Dice` is a small rigid-body simulation
(plain C++, no game or standard-library code) that moves five rounded dice, the tray and a kinematic cup the player
shakes and tips. The plugin will spawn collision-free dice and a cup and set their transforms from the simulation
every frame, so dice can't be flung off the table or wedge in level geometry.

- **Dice:** rounded boxes (corner and edge spheres), impulse solver with friction and restitution, fixed 1/960 s
  substeps. A die that settles leaning on another is nudged until it lies flat. Faces: +x 1, -x 6, +y 2, -y 5,
  +z 3, -z 4. `Drag` slides a die across the felt by hand without turning it over, shoving others aside.
- **Cup:** inside wall, base, outside wall and underside; 1.9 units inside radius, 4 deep. While shaking, the player's palm covers the mouth (`SetLid`); releasing winds up and flings it forward while tipping, so the dice tumble out. `AddToCup` drops a die back in through the mouth.
- **Table:** 25.3 x 17.2 inside, invisible walls on all four sides (dice in the cup pass over them; `barrierLeft` and friends can open a side, and `OffTable` reports a die that fell off).
- **Keepers:** dice left on the pad are frozen (`SetFrozen`) while the rest roll, so a throw can't knock them over. Fairness is checked with a chi-square test over 400 throws.
  (`SetLid`), so dice can't jump out; tipping it pours them onto the tray.
- **Opponent:** `src/sim/Ai` picks which dice to keep (every keep choice tried against simulated rerolls) and which box to score; it averages about 233 a game in `tests/ai_test.cpp`. In the preview it plays its turns on the table: its cup on the left, its rack across the far side.
- **Dice sets:** fifteen cosmetic sets (same physics). Innkeepers sell a new one every ten wins: Bone (start),
  Iron (10), Steel (20), Corundum (30), Dwemer (40), Orichalcum (50), Silver (60), Quicksilver (70), Moonstone (80),
  Gold (90), Malachite (100), Ebony (110), Stalhrim (120), Dragonbone (130), Daedric (140). See `docs/dice-sets.png`.
  Your cup tier follows your last finished game's score.
- **Sound:** the dice are real recordings: single hits cut by `tools/make_dice_hits.py` from Kenney's CC0 dice
  recordings (`assets/sounds/kenney`, kenney.nl): sharp clicks of dice in a cup for dice on dice and in the cup,
  knocks of dice landing on a table for the linen and the tray. Every contact the sim reports plays one at random,
  pitched for the dice's material, as loud as the impact and darker on the linen.
  Metal dice are heavier: pitched down, low end lifted, a touch of clank around 2 kHz, the top taken out so
  they're never tinny; in the cup a table knock sits under each click so they clunk.
  While shaking, each cup adds its own short, low metal clang (tankard pewter, goblet iron, Dwemer bronze lowest,
  silver brightest, gold longest), kept under 3.6 kHz so it never sounds like tin or glass.
- **Never stuck:** a die resting on another die, or cocked against a wall, is knocked over a few times and then set
  down flat on its nearest face at the closest clear spot; any die still loose six seconds after leaving the cup is
  set down the same way, so a throw always ends (`TestNeverStuck`).
- **Fus Ro Dah:** an easter egg modelled on Unrelenting Force: the three words appear as they're spoken, then the
  blast bends the air in a cone across the board, kicks the view and throws the dice back into the cup.

## In game (SKSE plugin, test build 0.1.0)

- **Who plays:** anyone sitting near a table. Every second the DLL puts seated NPCs within `TableReach` of a table
  (any reference whose mesh path has "table" in it) into the hidden faction `YA_TableFaction`, and takes them out
  again when they get up; the topic "Care for a Yacht game?" only shows for them. If you carry less than 25
  septims they turn you away. When their "Pull up a chair" line ends, the table opens.
- **The wager comes first:** 25 to 100 septims (`SkyrimYacht.ini`), taken when you place it. A win pays double, a
  draw gives it back, a loss or a forfeit keeps it. Leaving before the bet costs nothing.
- **The table:** a menu that pauses the game and blurs the world (`kPausesGame`, `UIBlurManager`). The tray
  (`YA_Tray`, `meshes\SkyrimYacht\Tray.nif`) is loaded into the game's inventory 3D preview; the dice
  (`Die_<set>.nif`) and the vanilla cups (found through the game's own items, so nothing of Bethesda's ships) hang
  under it as our own nodes. Table space is physics space, so the sim's positions go straight onto the nodes. The
  table is tilted toward you and scaled to the screen (`[Table]` in the ini); picking projects through the
  inventory scene's view frustum and inverts it for the felt and the carrying heights.
- **The game:** the same rules, physics and opponent as the preview (`src/sim`), driven from the menu's frame
  update while the world is paused. Records per opponent, your last score (your cup) and your wins (your dice:
  a new set every ten) are kept in the SKSE co-save.
- **Every version:** CommonLibSSE-NG 3.7 built for SE and AE with every address from Address Library, declared
  version-independent, so one DLL loads on 1.5.97, every 1.6 release (1.6.1170 included) and later ones Address
  Library covers, Steam and GOG. The ESP is written with header 1.70 / form version 44 so 1.5.97 can load it.
- **Built by CI:** `.github/workflows/build.yml` builds the DLL on Windows and pushes the ready-to-install package
  to the `dist` branch (and its logs to `ci-logs`). `tools/make_esp.py` writes the ESP; `tools/make_meshes.py`
  writes the meshes and textures from the preview's own art (`__yacht.exportTextures()`).

The browser preview stands in a blurred tavern for the paused world, and has the fuller interface (compass,
parchment tally sheets, inventory-style panel, key prompts) the plugin will grow into.
- **Scoring:** `src/sim/Yacht` has the thirteen boxes, the 35-point upper bonus at 63, 100-point bonuses for extra
  five-of-a-kinds and the joker rule.

## Layout

| Path | |
| --- | --- |
| `src/sim/` | Dice physics and scoring (shared by the plugin, the tests and the preview) |
| `tests/` | Native tests: 200 random throws must all land flat inside the tray, faces evenly spread |
| `preview/` | The sim compiled to WebAssembly in a three.js page: `preview/build.sh` makes `yacht-preview.html` |

## Building the tests and preview

```
g++ -std=c++20 -O2 -o build/dice_test tests/dice_test.cpp src/sim/Dice.cpp && ./build/dice_test
g++ -std=c++20 -O2 -o build/yacht_test tests/yacht_test.cpp src/sim/Yacht.cpp && ./build/yacht_test
./preview/build.sh   # needs clang with the wasm32 target and wasm-ld
```

## The game's own cups (local only)

`tools/nif2json.py` reads Skyrim SE meshes (BSTriShape) and `tools/make_local_cups.py` turns a folder of the four
cups' `.nif` and `.dds` files into `local-assets/cups.js`. `LOCAL=1 ./preview/build.sh` then writes
`preview/yacht-local.html`, which shows the real Tankard, Glazed Goblet, Dwemer Cup, Silver Goblet and Thieves Guild Golden Goblet. Both outputs
hold Bethesda's assets, so they are git-ignored and never published; the plugin uses the vanilla meshes from the
player's own game.

## Credits

- Music: the five tavern tracks from "Around the Fire" (Nexus Mods, Skyrim SE mod 36144). Which tracks play where is set in
  `Music/Music.ini`.
- Ogg decoding: stb_vorbis by Sean Barrett (public domain / MIT).
- Settings page: SKSE Menu Framework header (MIT).
