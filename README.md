# How Many Dudes? Multiplayer Mod

Turns the single-player run into a **two-player duel**. You both play your own
run as normal; every 20 rounds the boss fight is replaced by a fight against
your friend's army. Lose the fight, lose a life. First to zero is out.

Start a run and your friend gets pulled into one too. In the menu you can see
who you're connected to, and during a run you can see which round they're on.

**Both players need to do everything on this page.** It takes about five
minutes. If you get stuck, jump to [Troubleshooting](#troubleshooting) - the
symptom you're seeing is probably listed.

---

## What you need

- **Windows** (64-bit). The mod can't run on Mac or Linux, including Steam Deck
  and Proton.
- **How Many Dudes?** installed through Steam.
- A friend with both of the above.

You do **not** need a powerful PC. The mod is a few hundred kilobytes and one
network connection - if your machine runs the game, it runs the mod. You also
don't need Visual Studio, or any source code. Just the built file.

---

## Step 1 - Install this mod

- **Download `HowManyDudesMultiplayer.dll`** from the
  [Releases page](https://github.com/IMBRADY/HowManyDudesMultiplayer/releases)
- Put it in this folder, next to `install.bat`
- **Double-click `install.bat`**
- Save the **AuriePatcher.exe "C:\..."** line for later

Alternatively, you can run this in terminal:
- **Find your game folder:** in Steam, right-click *How Many Dudes?* → **Manage** → **Browse local files**.
```
install.bat "C:\Program Files (x86)\Steam\steamapps\common\How Many Dudes" 
```
(The file location you just found)

---

## Step 2 - Install the mod framework

This mod needs two free tools underneath it: **Aurie**  and **YYToolkit**

### 2a. AurieCore.dll

- From the [latest Aurie release](https://github.com/AurieFramework/Aurie/releases/latest),
download **`AurieCore.dll`**, **not** `AurieCore-x86.dll`
- Put it in: `...\How Many Dudes\mods\Native\` **same directory as the steam folder that you were in earlier**
- Download **`AuriePatcher.exe`**. Leave it anywhere for now

### 2b. YYToolkit.dll

- From the [YYToolkit **v5.0.0c** release](https://github.com/AurieFramework/YYToolkit/releases/tag/v5.0.0c),
download **`YYToolkit.dll`**.
- Put it in: `...\How Many Dudes\mods\Aurie\`

### 2c. Patch the game

Open a terminal **in the directory that you saved AuriePatcher.exe**. Run the AuriePatcher line you saved:
```
AuriePatcher.exe "C:\Program Files (x86)\Steam\steamapps\common\How Many Dudes\HowManyDudes.exe" "C:\Program Files (x86)\Steam\steamapps\common\How Many Dudes\mods\Native\AurieCore.dll" install
```
Note: This directory may be different depending on where your steamapps folder is located.

If you ever use Steam's *Verify integrity of game files*, that undoes it - just
run the command again.

> **Prefer a wizard?** `AurieInstaller.exe` does all of 2a–2c for you, but it's
> only in the [v2.0.0b release](https://github.com/AurieFramework/Aurie/releases/tag/v2.0.0b),
> not the latest one, and it needs the
> [.NET Desktop Runtime 10 (x64)](https://dotnet.microsoft.com/en-us/download/dotnet/10.0).
> The three manual steps above need nothing extra.

When you're done, the game folder should look like this:

```
How Many Dudes\
├── HowManyDudes.exe          (patched in step 2c)
└── mods\
    ├── Native\
    │   └── AurieCore.dll
    └── Aurie\
        ├── YYToolkit.dll
        └── HowManyDudesMultiplayer.dll    (put there by install.bat)
```

---

## Step 3 - Check that it worked

Run install.bat

Anything showing `[--] NOT found` points at the matching part of Step 2. The mod
itself is already installed, so you only ever need to redo the framework bits.

Then **launch the game**. If everything is working you'll see a console window
alongside the game with a green line like:

```
[HMD-MP] ready. F9 host | F10 join | F11 disconnect | F8 status | F6 diagnostics
```

That line means you're done. No console window at all means Aurie isn't
loading - see [Troubleshooting](#troubleshooting).

---

## Step 4 - Play together

One of you invites, the other accepts. **Press F7 twice:**

- **First F7** - opens a Steam lobby. Wait for `steam: lobby open` in the console
- **Second F7** - opens Steam's invite dialog. Pick your friend

Your friend clicks the invite and you're connected. No ports, no IP addresses,
works over the internet.

> Shift+Tab on its own won't show an "Invite to Game" option for this mod - you
> have to open the dialog with F7. Press **F8** any time to check the link.

On a local network you can skip Steam entirely: one player presses **F9**, the
other presses **F10**.

### What you'll see once you're connected

- **In the menu** - your friend's Steam name and a dude head in the top-right
  corner, so you can tell at a glance that the link is live.
- **Starting a run** - whoever presses start pulls the other in. If one of you
  is already in a run, they're left where they are.
- **During a run** - a dude head sits above your friend's current round on the
  round track at the top of the screen, so you can see how far ahead or behind
  they are.
- **Messages** - connecting, duels, lives lost and the final result all appear
  on screen through the game's own message stream.

### The duel

Play normally. Every **20 rounds** - the round that would normally be a boss
fight - the mod takes over instead:

1. **WAIT** - if you got there first, the arena is held empty behind a
   `WAITING FOR <friend>` banner until they reach the same round. No boss
   spawns; you're not fighting anything while you wait.
2. **SERIALIZE** - once you're both there, each of you sends your army to the
   other.
3. **INJECT** - your friend's army is spawned as your opponents.
4. **RESOLVE** - whoever's army falls loses a life. At zero lives, that run ends.

Then the run carries on to round 21 and it happens again at round 40.

If your friend never turns up, the duel is called off after **5 minutes**, the
round is rebuilt with its normal boss, and you play it as usual. If their
connection actually drops, the match ends and your run carries on unmodded - a
dropped connection can't ruin your run.

You can change the 20 to anything you like with `duel_interval`, and turn off
the synced start with `sync_run_start`. Both players should use the same
`duel_interval`.

---

## Troubleshooting

| What you're seeing | What it means | What to do |
|---|---|---|
| `install.bat` says it can't find the game | Non-standard install location | Drag your game folder onto `install.bat`, or pass the path as an argument |
| `[--] Aurie Framework NOT found` | `AurieCore.dll` missing or misplaced | Step 2a. It goes in `mods\Native\`, not `mods\Aurie\` |
| `[--] YYToolkit NOT found` | `YYToolkit.dll` missing or misplaced | Step 2b. It goes in `mods\Aurie\`, not `mods\Native\` |
| `[--] executable is NOT patched` | Step 2c not done, or Steam undid it | Re-run the `AuriePatcher.exe … install` command |
| No console window when the game starts | Aurie isn't loading | Usually step 2c. Run `install.bat /detect` and check all three lines |
| Console appears, but no `[HMD-MP]` lines | Aurie is loading, this mod isn't | Re-run `install.bat`; check the DLL is in `mods\Aurie\` |
| Worked yesterday, broken today | Steam updated or verified the game | Re-run step 2c, then `install.bat` |
| No "Invite to Game" when you press Shift+Tab | Expected - the mod doesn't advertise to the friends list | Press **F7 twice** instead. The second press opens Steam's invite dialog |
| **F7** twice shows no dialog | Steam overlay isn't loading | Steam → Settings → In Game → enable the overlay, and check the same box in the game's Properties |
| **F10** does nothing / "no host answered" | Discovery blocked, or host isn't hosting | Confirm the other player pressed **F9** first. Allow the game through Windows Firewall on both PCs. Both on the same network? |
| Connects, then immediately drops | Mismatched builds, or mismatched passphrase | You must both run the **same version** of the DLL. Check `session_key` matches exactly |
| "peer presented the wrong session key" | Passphrase mismatch | Make them identical on both machines, including spaces |
| Round 20 passes normally, no duel | Not connected, or the round number didn't resolve | Press **F8** for `link=connected`, then **F6** and look for `round=` and `tracking=` |
| Game crashes on launch | Framework version mismatch | Make sure YYToolkit is **5.0.0c**. v5 breaks all v4 mods, including this one |
| No name/head in the menu corner | The draw hook didn't install | Press **F6**; look for `draw hook installed` and `head sprite resolved` |
| No head on the round track | Same, or your friend's round is unknown | Press **F6**; check `run-map cell(s) tracked` is not 0 |
| Nothing appears on screen, but the log looks fine | The game's message stream didn't take | Press **F6** and check `info stream`. Everything still works, it's just quiet |

**Still stuck?** Press **F8** in-game for a status line, or **F6** for a full
diagnostic dump. Every line the mod prints starts with `[HMD-MP]`, and it says
exactly which stage failed.

---

## Settings reference

The file is at `mods\Aurie\HowManyDudesMultiplayer.ini`, created automatically.
Every setting is optional - the defaults work as-is on a local network. Your
edits are never overwritten.

| Setting | Default | What it's for |
|---|---|---|
| `peer_address` | `auto` | `auto` finds the host on your network. Or put an IP here for internet play |
| `port` | `47801` | Both players must match. Discovery uses the next port up (`47802`) |
| `session_key` | *(empty)* | Shared passphrase. Both players must match. Strongly recommended if you forward a port |
| `enable_discovery` | `true` | Set `false` to disable network discovery and use addresses only |
| `auto_host` | `false` | Host automatically at launch instead of pressing F9 |
| `auto_join` | `false` | Join automatically at launch instead of pressing F10 |
| `duel_interval` | `20` | Rounds between duels. Both players should match |
| `sync_run_start` | `true` | One player pressing start also starts the other's run |
| `on_screen_messages` | `true` | Show the mod's messages in-game. Turn off if they misbehave; the log still has them |

---

## A few honest notes

**This is a fresh mod that hasn't been played yet.** It builds cleanly and
passes its full offline test suite (63/63), but end-to-end play needs two real
clients and that hasn't happened. Treat your first session as a test run. If
something misbehaves, the console tells you what - and the
`--- members of o_dude ---` list it prints on the first run is the information
needed to fix stat handling.

**This mod doesn't touch your game files.** No modification to `data.win` or
your saves - it loads alongside the game and works entirely in memory.
Uninstalling it is deleting one file from `mods\Aurie\`.

Aurie itself is a different matter: step 2c edits `HowManyDudes.exe` so the
framework loads with the game. That's how Aurie works, it's reversible
(`AuriePatcher.exe … uninstall`, or Steam's *Verify integrity of game files*),
and it's a normal thing for a mod loader to do - but you should know it's
happening.

**There's no anti-cheat.** Stats coming from your opponent are bounds-checked 
so nobody can spawn something unkillable, and a peer has to pass a handshake 
before anything is exchanged - but a determined person could still cheat. 
Play with people you know.

**Both players must run the same build of the DLL.** Different versions refuse
to connect rather than misbehave. If you update, send your friend the new file.

---

## For developers

You don't need this to play - it's only for building the DLL yourself.

Requires Visual Studio 2022 (any edition, or Build Tools) with the **Desktop
development with C++** workload. `build.bat` finds it for you.

```
.\build.bat            REM -> build\HowManyDudesMultiplayer.dll
.\tests\run_tests.bat  REM offline suite: JSON, sanitiser, transport, handshake, discovery
```

- `discovered_mappings.json` - the game internals this mod hooks into
- `src/Sanitize.cpp` - everything the mod refuses to trust from a peer

Licensed under **AGPL-3.0** - see `LICENSE` and `NOTICE`. This is required
rather than chosen: the DLL compiles in code from YYToolkit and Aurie, both
AGPL-3.0.
