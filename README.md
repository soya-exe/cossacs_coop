# Cossacks: Back to War: multiplayer over Steam

Play Cossacks: Back to War (Steam app 4850, v1.35) multiplayer with friends through Steam:
lobby and invites through Steam, game traffic through Valve's relay, no port forwarding and no
IP addresses. The game's own lockstep netcode is unchanged; the mod replaces its dead internet
lobby with a Steam lobby and carries its UDP packets over Steam.

Status: tested on real Steam between two accounts (2026-10-28: lobby, invite, room, two
Deathmatch games over the Steam relay), and with two game copies on one PC over a mock Steam
(also with lag and loss). Player guide and installers: [docs/PLAYERS.md](docs/PLAYERS.md).

## How it works

- `ichat.dll` (replaces the game's own `bin/ichat.dll`, original kept as `ichat_orig.dll`):
  - patches one call so the "GSC-Game.Net" connection button opens instead of a "not supported"
    page;
  - implements the IChat lobby function: the Steam lobby owner creates the game room, members
    join it;
  - hooks the game's WS2_32 imports: the game's UDP socket on port 34000 sees a virtual LAN
    10.77.0.x, one address per Steam lobby member, carried by ISteamNetworkingMessages.
- `steam_api.dll`: Valve's 32-bit flat-API DLL (from Steamworks.NET Standalone 2025.164.1).

Details: [src/hook/README.md](src/hook/README.md), reverse-engineering notes:
[re/FINDINGS.md](re/FINDINGS.md), original plan: [DESIGN.md](DESIGN.md), tests:
[tools/README-testing.md](tools/README-testing.md), [tools/E2E-RESULTS.md](tools/E2E-RESULTS.md).

## Build

Needs `mingw-w64-gcc` (i686).

```
tools/build.sh        # build/ichat.dll
tools/build_mock.sh   # build/mock/steam_api.dll (tests only) + test_mock.exe
tools/test_mock.sh    # mock Steam self-test under wine
tools/test_nick.sh    # unit test of the nick sanitizer (host gcc)
tools/build_installer.sh  # build/btwsteam-setup.exe (Windows installer, DLLs embedded)
tools/package.sh      # dist/cossacks-steam-coop-<version>/ and .zip (installers + DLLs + PLAYERS.md)
tools/test_installers.sh  # installer tests: install.sh and setup.exe (wine) on a fake Steam tree
```

No game files are in this repository.
