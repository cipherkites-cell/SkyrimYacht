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
- **Sound:** every contact the sim reports is synthesised from the die's material (bone, iron and steel, precious
  metal, stone, stalhrim) and what it struck (another die, the linen, the tray, a wooden or metal cup), as hard as
  the hit and panned to where it happened, with no room echo. Rolling and the rattle inside the cup are
  continuous layers that follow the dice.
- **Fus Ro Dah:** an easter egg modelled on Unrelenting Force: the three words appear as they're spoken, then the
  blast bends the air in a cone across the board, kicks the view and throws the dice back into the cup.

## In game

Playing pauses the world and blurs it behind the table, the way Skyrim's own menus do: the game is a menu with
`kPausesGame`, and it raises the menu blur with `RE::UIBlurManager::IncrementBlurCount()` on open and lowers it on
close. The preview stands in a blurred tavern for that world. The overlay follows Skyrim's interface: the compass
across the top (whose turn, the three rolls, the Thu'um recharging), parchment tally sheets, an inventory-style
panel for cups and dice, and key prompts in the corner (Z shout, H help, M sound, Tab forfeit).
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
