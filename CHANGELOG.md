# Changelog / Список изменений

All players in one game need the same mod version. / У всех игроков в одной игре должна быть
одна версия мода.

## 0.1.3 — 2026-10-28

- Less CPU under Proton/Wine: `csemu.dll`'s render thread presents without vsync (one byte
  changed in memory, never on disk). Under wined3d the vsync'd flip made that thread spin at
  3–93 % of a core; now it uses 2–3 %. Frame rate and game speed are unchanged (csemu's own
  limiter paces the game). `BTWSTEAM_NOVSYNC=0` turns it off. Not applied on Windows.
- The game's own wait loops (multiplayer tick, single-player end of frame) sleep ~1 ms instead
  of spinning when a sleep surely ends before the deadline. It matters at slow game speeds (main
  thread 99 % -> 18 % of a core) and in network games with ping >= 50 ms; game speed and
  lockstep timing are unchanged. `BTWSTEAM_NOSPIN=0` turns it off.
- Меньше нагрузка на процессор под Proton/Wine (вывод кадров без ожидания вертикальной
  синхронизации, циклы ожидания засыпают вместо вращения). Отключение: `BTWSTEAM_NOVSYNC=0`,
  `BTWSTEAM_NOSPIN=0` в параметрах запуска.

## 0.1.2 — 2026-10-28

- Lower network latency: game data is taken from Steam on the game's own thread when the game
  polls its socket, instead of waiting for the mod's 10 ms worker loop. Room ping between two
  copies dropped from 28–30 ms to 17–19 ms (mock Steam); the mod's own added delay went from
  ~17 ms (p95) to ~0.02 ms. `BTWSTEAM_GAME_THREAD_RX=0` restores the old behaviour.
- `BTWSTEAM_TIMING=1` writes latency percentiles to `bin/btwsteam.log` every 10 s.
- Меньше сетевая задержка: пинг в комнате 17–19 мс вместо 28–30 мс (на имитации Steam).

## 0.1.1 — 2026-10-28

- In-game name comes from the Steam name (Cyrillic transliterated to Latin, other symbols
  dropped, up to 31 characters); `BTWSTEAM_NICK` still overrides it.
- Quieter log: Steam's own API call results are only counted.
- Builds are reproducible (byte-identical from the same sources).
- Ник в игре берётся из имени Steam (кириллица транслитерируется).

## 0.1.0 — 2026-10-28

- First release: multiplayer of Cossacks: Back to War (Steam, v1.35) over Steam lobbies and the
  Steam relay; the "GSC-Game.Net Internet Game" button creates or joins the room.
- Installers: `btwsteam-setup.exe` (Windows, also under Wine) and `install.sh` (Linux).
- Player guide in Russian, Ukrainian and English: [docs/PLAYERS.md](docs/PLAYERS.md).
- Tested on real Steam between two accounts (host on Linux/Proton).
- Первый выпуск: сетевая игра через лобби и ретрансляцию Steam, установщики, инструкция.
