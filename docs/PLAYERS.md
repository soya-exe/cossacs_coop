# Cossacks: Back to War — сетевая игра через Steam (btwsteam)

[Українською нижче](#ukrainian) · [English below](#english)

## Что делает мод

Мод позволяет играть в Cossacks: Back to War (Steam, версия 1.35) по сети с друзьями из списка
друзей Steam. Не нужны проброс портов, IP-адреса и Hamachi/VPN: друзья заходят в ваше лобби Steam,
а игровые пакеты идут через серверы-ретрансляторы Valve. Сетевой код самой игры не меняется: мод
заменяет мёртвый интернет-лобби GSC-Game.Net лобби Steam.

Мод состоит из двух файлов в папке игры `bin\`:

- `ichat.dll` — наш файл вместо файла игры (оригинал сохраняется как `ichat_orig.dll`);
- `steam_api.dll` — стандартная библиотека Steam от Valve.

Нужно: Steam запущен и вы вошли в аккаунт; у всех игроков одна и та же версия игры из Steam
(v1.35, сборка 2684) и одна версия мода.

## Установка на Windows

1. Закройте игру.
2. Распакуйте архив `cossacks-steam-coop-<версия>.zip` в любую папку.
3. Дважды щёлкните `btwsteam-setup.exe`. Установщик сам найдёт игру в библиотеках Steam, покажет,
   что сейчас установлено, и спросит, что делать: **Enter** — установить, **U** + Enter — удалить,
   **Q** + Enter — выйти.
   - Если игра не нашлась, положите `btwsteam-setup.exe` в папку игры и запустите его там, или
     перетащите папку игры на `btwsteam-setup.exe`.
   - Если игра стоит в `C:\Program Files (x86)` и установщик пишет об отказе в доступе, запустите его
     через правый клик → «Запуск от имени администратора».
   - Windows может предупредить о неизвестном издателе (SmartScreen): «Подробнее» → «Выполнить в
     любом случае».
4. Установщик проверяет, что `dmcr.exe` — именно поддерживаемая версия 1.35, и отказывается
   работать с другой версией (мод правит код игры по фиксированным адресам) или пока игра запущена.

Из командной строки: `btwsteam-setup.exe ["D:\путь\к\Cossacks Back to War"] [--install | --uninstall | --status] [--force]`.
Коды выхода: 0 — успех, 1 — ошибка, 2 — неверные аргументы, 3 — игра не найдена, 4 — не та версия
`dmcr.exe`, 5 — игра запущена. `--force` ставит мод на другую версию `dmcr.exe` на ваш риск.

## Установка на Linux (Steam + Proton)

1. Закройте игру.
2. Распакуйте архив и в терминале выполните в распакованной папке:

   ```sh
   bash install.sh
   ```

   Скрипт ищет Steam в `~/.local/share/Steam`, `~/.steam/steam` и во Flatpak-версии
   (`~/.var/app/com.valvesoftware.Steam/.local/share/Steam`), читает `libraryfolders.vdf` и
   `appmanifest_4850.acf`. Если Steam в другом месте: `STEAM_ROOT=/путь/к/Steam bash install.sh`,
   или укажите папку игры: `bash install.sh "/путь/к/Cossacks Back to War"`.
3. Нужны `bash` и `sha256sum` (есть почти везде). Wine не нужен. Файлы `ichat.dll` и
   `steam_api.dll` берутся из той же папки, где лежит `install.sh`.
4. Запускайте игру из Steam как обычно (через Proton), параметры запуска не нужны.

Те же параметры: `--status`, `--uninstall`, `--force`, те же коды выхода. Оба установщика ведут
одну и ту же запись `bin/btwsteam-installed.txt` (версия, дата, файлы и их SHA-256), поэтому
установленное одним можно удалить другим.

## Как играть

При запуске игры мод сам создаёт лобби Steam «только для друзей» (до 7 игроков), его владелец —
вы. Кто владелец лобби, тот и хост.

**Хост:**

1. Запустите игру из Steam.
2. Multiplayer → Deathmatch (или Historical battle) → **GSC-Game.Net Internet Game** → **Create**.
   Мод создаёт игровую комнату (название «<ник>'s game») и отмечает в лобби Steam, что комната
   готова.
3. Пригласите друзей: в окне друзей Steam правый клик по другу → «Пригласить в игру». Друзья могут и
   сами: правый клик по вам → «Присоединиться к игре». Если оверлей Steam (Shift+Tab) в игре не
   открывается, переключитесь в окно Steam (Alt+Tab).

**Друзья (порядок важен):**

1. Запустите игру и оставайтесь в главном меню.
2. **Сначала** войдите в лобби хоста: примите приглашение или выберите «Присоединиться к игре» в
   списке друзей. Мод выходит из своего лобби и входит в лобби хоста.
3. **Потом** нажмите ту же кнопку: Multiplayer → Deathmatch (или Historical battle) →
   **GSC-Game.Net Internet Game** → **Create**. Для участника чужого лобби эта кнопка не создаёт
   комнату, а входит в комнату хоста. Если хост ещё не создал комнату, игра ждёт её; **Esc** отменяет
   ожидание.
4. Если нажать Create раньше, чем войти в лобби хоста, вы останетесь владельцем своего лобби и
   создадите свою комнату. Тогда выйдите из комнаты, примите приглашение и нажмите Create снова.

**В комнате:** у присоединившегося «готов» — это кнопка **Start** (в строке состояния: «Click here
when you are ready to play the network game»). Щелчок по крестику в колонке Ready ничего не делает.
Когда у всех стоят галочки, хост нажимает **Start**.

Ник в игре — `Player<номер>` (номер места в лобби: у хоста 1), если не задана переменная окружения
`BTWSTEAM_NICK` (только латиница, до 31 символа):

- Linux/Proton: в Steam → свойства игры → параметры запуска: `BTWSTEAM_NICK=Ivan %command%`;
- Windows: «Изменение переменных среды для своей учётной записи» → создать `BTWSTEAM_NICK`, затем
  перезапустить Steam.

## После «Проверить целостность файлов игры»

Проверка целостности в Steam (и иногда обновление игры) возвращает оригинальный `ichat.dll`, и мод
перестаёт работать (`--status` пишет «НЕ активен»). Просто запустите установщик ещё раз: резервная
копия `ichat_orig.dll` при этом не портится.

## Удаление

- Windows: `btwsteam-setup.exe` → **U** + Enter, или `btwsteam-setup.exe --uninstall`.
- Linux: `bash install.sh --uninstall`.

Возвращаются оригинальные `ichat.dll` и (если был) прежний `steam_api.dll`; удаляются наши
`steam_api.dll`, `steam_appid.txt` (его пишет мод при первом запуске) и
`bin/btwsteam-installed.txt`. Журналы остаются.

## Журнал

`bin/btwsteam.log` рядом с `dmcr.exe`, журнал прошлого запуска — `bin/btwsteam.prev.log`. Если
что-то не работает, приложите эти файлы к сообщению об ошибке. Состояние установки:
`btwsteam-setup.exe --status` или `bash install.sh --status`.

## Известные ограничения

- Мод пока **не проверен в настоящем Steam между двумя аккаунтами**. Проверено: две копии игры на
  одном ПК с имитацией Steam (в том числе с задержкой и потерей пакетов).
- Ник — `Player<номер>`, если не задан `BTWSTEAM_NICK`.
- Проверен только режим Deathmatch. Historical battle ведёт на тот же экран подключения, но с модом
  не проверялся.
- Только Steam-версия 1.35 (`dmcr.exe` SHA-256
  `3923b0ff3ae8da0584f29ae2bbf2e2deb030e57df11d37fb6fb55fa5ceb4acd6`).
- Запуск игры сразу по приглашению (когда игра у друга ещё не запущена) не проверен: лучше сначала
  запустить игру, потом принять приглашение.

---

<a id="ukrainian"></a>
# Cossacks: Back to War — мережева гра через Steam (btwsteam)

## Що робить мод

Мод дає змогу грати в Cossacks: Back to War (Steam, версія 1.35) по мережі з друзями зі списку
друзів Steam. Не потрібні прокидання портів, IP-адреси та Hamachi/VPN: друзі заходять у ваше лобі
Steam, а ігрові пакети йдуть через сервери-ретранслятори Valve. Мережевий код самої гри не
змінюється: мод замінює мертве інтернет-лобі GSC-Game.Net лобі Steam.

Мод складається з двох файлів у теці гри `bin\`:

- `ichat.dll` — наш файл замість файлу гри (оригінал зберігається як `ichat_orig.dll`);
- `steam_api.dll` — стандартна бібліотека Steam від Valve.

Потрібно: Steam запущено і ви увійшли в обліковий запис; у всіх гравців та сама версія гри зі Steam
(v1.35, збірка 2684) і та сама версія мода.

## Встановлення у Windows

1. Закрийте гру.
2. Розпакуйте архів `cossacks-steam-coop-<версія>.zip` у будь-яку теку.
3. Двічі клацніть `btwsteam-setup.exe`. Інсталятор сам знайде гру в бібліотеках Steam, покаже, що
   зараз встановлено, і запитає, що робити: **Enter** — встановити, **U** + Enter — видалити,
   **Q** + Enter — вийти.
   - Якщо гру не знайдено, покладіть `btwsteam-setup.exe` у теку гри й запустіть його там, або
     перетягніть теку гри на `btwsteam-setup.exe`.
   - Якщо гра стоїть у `C:\Program Files (x86)` і інсталятор пише про відмову в доступі, запустіть
     його через правий клік → «Запуск від імені адміністратора».
   - Windows може попередити про невідомого видавця (SmartScreen): «Докладніше» → «Усе одно
     запустити».
4. Інсталятор перевіряє, що `dmcr.exe` — саме підтримувана версія 1.35, і відмовляється працювати з
   іншою версією (мод змінює код гри за фіксованими адресами) або поки гра запущена.

З командного рядка: `btwsteam-setup.exe ["D:\шлях\до\Cossacks Back to War"] [--install | --uninstall | --status] [--force]`.
Коди виходу: 0 — успіх, 1 — помилка, 2 — неправильні аргументи, 3 — гру не знайдено, 4 — не та
версія `dmcr.exe`, 5 — гра запущена. `--force` встановлює мод на іншу версію `dmcr.exe` на ваш
ризик.

## Встановлення в Linux (Steam + Proton)

1. Закрийте гру.
2. Розпакуйте архів і в терміналі виконайте в розпакованій теці:

   ```sh
   bash install.sh
   ```

   Скрипт шукає Steam у `~/.local/share/Steam`, `~/.steam/steam` і у Flatpak-версії
   (`~/.var/app/com.valvesoftware.Steam/.local/share/Steam`), читає `libraryfolders.vdf` і
   `appmanifest_4850.acf`. Якщо Steam в іншому місці: `STEAM_ROOT=/шлях/до/Steam bash install.sh`,
   або вкажіть теку гри: `bash install.sh "/шлях/до/Cossacks Back to War"`.
3. Потрібні `bash` і `sha256sum` (є майже всюди). Wine не потрібен. Файли `ichat.dll` і
   `steam_api.dll` беруться з тієї ж теки, де лежить `install.sh`.
4. Запускайте гру зі Steam як звичайно (через Proton), параметри запуску не потрібні.

Ті самі параметри: `--status`, `--uninstall`, `--force`, ті самі коди виходу. Обидва інсталятори
ведуть той самий запис `bin/btwsteam-installed.txt` (версія, дата, файли та їхні SHA-256), тому
встановлене одним можна видалити іншим.

## Як грати

Під час запуску гри мод сам створює лобі Steam «лише для друзів» (до 7 гравців), його власник — ви.
Хто власник лобі, той і хост.

**Хост:**

1. Запустіть гру зі Steam.
2. Multiplayer → Deathmatch (або Historical battle) → **GSC-Game.Net Internet Game** → **Create**.
   Мод створює ігрову кімнату (назва «<нік>'s game») і позначає в лобі Steam, що кімната готова.
3. Запросіть друзів: у вікні друзів Steam правий клік по другові → «Запросити в гру». Друзі можуть і
   самі: правий клік по вас → «Приєднатися до гри». Якщо оверлей Steam (Shift+Tab) у грі не
   відкривається, перейдіть у вікно Steam (Alt+Tab).

**Друзі (порядок важливий):**

1. Запустіть гру й залишайтеся в головному меню.
2. **Спочатку** увійдіть у лобі хоста: прийміть запрошення або виберіть «Приєднатися до гри» у
   списку друзів. Мод виходить зі свого лобі й входить у лобі хоста.
3. **Потім** натисніть ту саму кнопку: Multiplayer → Deathmatch (або Historical battle) →
   **GSC-Game.Net Internet Game** → **Create**. Для учасника чужого лобі ця кнопка не створює
   кімнату, а входить у кімнату хоста. Якщо хост ще не створив кімнату, гра чекає на неї; **Esc**
   скасовує очікування.
4. Якщо натиснути Create раніше, ніж увійти в лобі хоста, ви залишитеся власником свого лобі й
   створите свою кімнату. Тоді вийдіть із кімнати, прийміть запрошення й натисніть Create знову.

**У кімнаті:** для того, хто приєднався, «готовий» — це кнопка **Start** (у рядку стану: «Click here
when you are ready to play the network game»). Клацання по хрестику в стовпці Ready нічого не
робить. Коли в усіх стоять галочки, хост натискає **Start**.

Нік у грі — `Player<номер>` (номер місця в лобі: у хоста 1), якщо не задано змінну середовища
`BTWSTEAM_NICK` (лише латиниця, до 31 символу):

- Linux/Proton: у Steam → властивості гри → параметри запуску: `BTWSTEAM_NICK=Ivan %command%`;
- Windows: «Редагування змінних середовища для вашого облікового запису» → створити
  `BTWSTEAM_NICK`, потім перезапустити Steam.

## Після «Перевірити цілісність файлів гри»

Перевірка цілісності в Steam (а іноді й оновлення гри) повертає оригінальний `ichat.dll`, і мод
перестає працювати (`--status` пише «НЕ активен» / «NOT active»). Просто запустіть інсталятор ще раз: резервна
копія `ichat_orig.dll` при цьому не псується.

## Видалення

- Windows: `btwsteam-setup.exe` → **U** + Enter, або `btwsteam-setup.exe --uninstall`.
- Linux: `bash install.sh --uninstall`.

Повертаються оригінальний `ichat.dll` і (якщо був) попередній `steam_api.dll`; видаляються наші
`steam_api.dll`, `steam_appid.txt` (його записує мод під час першого запуску) і
`bin/btwsteam-installed.txt`. Журнали залишаються.

## Журнал

`bin/btwsteam.log` поруч із `dmcr.exe`, журнал попереднього запуску — `bin/btwsteam.prev.log`.
Якщо щось не працює, додайте ці файли до повідомлення про помилку. Стан встановлення:
`btwsteam-setup.exe --status` або `bash install.sh --status`.

## Відомі обмеження

- Мод поки **не перевірено в справжньому Steam між двома обліковими записами**. Перевірено: дві
  копії гри на одному ПК з імітацією Steam (зокрема із затримкою та втратою пакетів).
- Нік — `Player<номер>`, якщо не задано `BTWSTEAM_NICK`.
- Перевірено лише режим Deathmatch. Historical battle веде на той самий екран підключення, але з
  модом не перевірявся.
- Лише Steam-версія 1.35 (`dmcr.exe` SHA-256
  `3923b0ff3ae8da0584f29ae2bbf2e2deb030e57df11d37fb6fb55fa5ceb4acd6`).
- Запуск гри одразу за запрошенням (коли гра в друга ще не запущена) не перевірено: краще спочатку
  запустити гру, потім прийняти запрошення.

---

<a id="english"></a>
# Cossacks: Back to War — multiplayer over Steam (btwsteam)

## What the mod does

The mod lets you play Cossacks: Back to War (Steam, version 1.35) online with friends from your
Steam friends list. No port forwarding, no IP addresses, no Hamachi/VPN: friends join your Steam
lobby, and game packets travel through Valve's relay servers. The game's own netcode is unchanged:
the mod replaces the dead GSC-Game.Net internet lobby with a Steam lobby.

The mod is two files in the game's `bin\` folder:

- `ichat.dll`: ours, in place of the game's file (the original is kept as `ichat_orig.dll`);
- `steam_api.dll`: Valve's standard Steam library.

You need: Steam running and logged in; every player on the same Steam version of the game (v1.35,
build 2684) and the same mod version.

## Installing on Windows

1. Quit the game.
2. Unpack `cossacks-steam-coop-<version>.zip` anywhere.
3. Double-click `btwsteam-setup.exe`. It finds the game in your Steam libraries, shows what is
   installed, and asks what to do: **Enter** installs, **U** + Enter uninstalls, **Q** + Enter quits.
   - If the game is not found, put `btwsteam-setup.exe` into the game folder and run it there, or
     drag the game folder onto `btwsteam-setup.exe`.
   - If the game is under `C:\Program Files (x86)` and the installer reports "access denied", run it
     with right-click → "Run as administrator".
   - Windows may warn about an unknown publisher (SmartScreen): "More info" → "Run anyway".
4. The installer checks that `dmcr.exe` is exactly the supported version 1.35. It refuses another
   version (the mod patches the game's code at fixed addresses) and refuses while the game runs.

Command line: `btwsteam-setup.exe ["D:\path\to\Cossacks Back to War"] [--install | --uninstall | --status] [--force]`.
Exit codes: 0 success, 1 error, 2 bad arguments, 3 game not found, 4 wrong `dmcr.exe`, 5 game
running. `--force` installs on another `dmcr.exe` at your own risk.

## Installing on Linux (Steam + Proton)

1. Quit the game.
2. Unpack the zip and, in a terminal, run this in the unpacked folder:

   ```sh
   bash install.sh
   ```

   It looks for Steam in `~/.local/share/Steam`, `~/.steam/steam` and the Flatpak Steam
   (`~/.var/app/com.valvesoftware.Steam/.local/share/Steam`), then reads `libraryfolders.vdf` and
   `appmanifest_4850.acf`. Steam elsewhere: `STEAM_ROOT=/path/to/Steam bash install.sh`, or give the
   game folder: `bash install.sh "/path/to/Cossacks Back to War"`.
3. It needs `bash` and `sha256sum` (present almost everywhere), not Wine. It takes `ichat.dll` and
   `steam_api.dll` from the folder `install.sh` is in.
4. Start the game from Steam as usual (through Proton). No launch options are needed.

Same options: `--status`, `--uninstall`, `--force`, same exit codes. Both installers keep the same
record, `bin/btwsteam-installed.txt` (version, date, files and their SHA-256), so either one can
remove what the other installed.

## How to play

When the game starts, the mod creates a friends-only Steam lobby (up to 7 players) that you own.
Whoever owns the lobby is the host.

**Host:**

1. Start the game from Steam.
2. Multiplayer → Deathmatch (or Historical battle) → **GSC-Game.Net Internet Game** → **Create**.
   The mod creates the game room (named "<nick>'s game") and marks the room as ready in the Steam
   lobby.
3. Invite friends: in the Steam friends window, right-click a friend → "Invite to game". Friends can
   also right-click you → "Join game". If the Steam overlay (Shift+Tab) does not open in the game,
   switch to the Steam window (Alt+Tab).

**Friends (the order matters):**

1. Start the game and stay in the main menu.
2. **First** join the host's lobby: accept the invite, or pick "Join game" in the friends list. The
   mod leaves its own lobby and joins the host's.
3. **Then** click the same button: Multiplayer → Deathmatch (or Historical battle) →
   **GSC-Game.Net Internet Game** → **Create**. For a member of someone else's lobby this button
   does not create a room, it joins the host's room. If the host has not created the room yet, the
   game waits for it; **Esc** cancels the wait.
4. If you click Create before joining the host's lobby, you are still the owner of your own lobby
   and create your own room. Leave the room, accept the invite, and click Create again.

**In the room:** for the joiner, "ready" is the **Start** button (the status bar says "Click here
when you are ready to play the network game"). Clicking the cross in the Ready column does nothing.
Once everyone has a check mark, the host clicks **Start**.

Your in-game name is `Player<number>` (your slot in the lobby; the host is 1) unless the environment
variable `BTWSTEAM_NICK` is set (ASCII only, up to 31 characters):

- Linux/Proton: Steam → game Properties → Launch options: `BTWSTEAM_NICK=Ivan %command%`;
- Windows: "Edit environment variables for your account" → add `BTWSTEAM_NICK`, then restart Steam.

## After "Verify integrity of game files"

Steam's "Verify integrity of game files" (and sometimes a game update) puts the original
`ichat.dll` back, and the mod stops working (`--status` says "NOT active"). Run the installer again;
the backup `ichat_orig.dll` stays intact.

## Uninstalling

- Windows: `btwsteam-setup.exe` → **U** + Enter, or `btwsteam-setup.exe --uninstall`.
- Linux: `bash install.sh --uninstall`.

This restores the original `ichat.dll` and the previous `steam_api.dll` (if there was one), and
removes our `steam_api.dll`, `steam_appid.txt` (the mod writes it at first start) and
`bin/btwsteam-installed.txt`. Logs are kept.

## Log

`bin/btwsteam.log` next to `dmcr.exe`; the previous run is `bin/btwsteam.prev.log`. If something
does not work, attach these files to your bug report. Install state: `btwsteam-setup.exe --status`
or `bash install.sh --status`.

## Known limitations

- **Not yet tested on real Steam between two accounts.** Tested: two copies of the game on one PC
  with a simulated Steam (also with lag and packet loss).
- The name is `Player<number>` unless `BTWSTEAM_NICK` is set.
- Only Deathmatch was tested. Historical battle leads to the same connection screen but has not been
  tested with the mod.
- Steam version 1.35 only (`dmcr.exe` SHA-256
  `3923b0ff3ae8da0584f29ae2bbf2e2deb030e57df11d37fb6fb55fa5ceb4acd6`).
- Starting the game straight from an invite (when the friend's game is not running yet) is untested:
  start the game first, then accept the invite.
