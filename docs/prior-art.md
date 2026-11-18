# Разбор существующих решений

Что уже сделано в этой области, что из этого работает, и на какие грабли наступили до нас. Документ — рабочий справочник при написании кода roost, а не обзор ради обзора.

Ссылки даны как `путь:строка` относительно корня соответствующего проекта. Изученные версии:

| Проект | Коммит | Дата | Роль в разборе |
|---|---|---|---|
| [cc-fleet](https://github.com/zexUlt/cc-fleet) | `a4d2e73` | 2026-11-12 | прямой предшественник, источник фич и тестов |
| [ghostty](https://github.com/ghostty-org/ghostty) (libghostty-vt) | `7fa26e8` | 2026-11-14 | эмулятор терминала roost |
| [agterm](https://github.com/umputun/agterm) | `c32ebec` | 2026-11-15 | ближайший по UX: сайдбар сессий, хуки статуса |
| [herdr](https://github.com/herdrdev/herdr) | `ee5bfb5` | 2026-10-29 | плагины, детект состояний, libghostty-vt в Rust |
| [codex](https://github.com/openai/codex) | `ee344ca` | 2026-11-15 | только `codex-rs/app-server/README.md` |
| [qtbase](https://github.com/qt/qtbase) | `d8133df` | 2026-11-15 | только платформа `offscreen` |
| [qtdeclarative](https://github.com/qt/qtdeclarative) | `aa3b2a9` | 2026-11-15 | только выбор бэкенда и цикла рендера |

Остальные проекты (Wave, Paradev, runner, sheprd, Zed, xterm.js, Dear ImGui, ImHex, Slint, RmlUi) не клонировались: сведения о них взяты из README, документации и issue по ссылкам ниже и помечены так, где это существенно.

---

## 1. cc-fleet и Claude Code

### 1.1 Как устроен cc-fleet

Python TUI, 2638 строк в `src/cc_fleet/`. Слева список сессий по категориям, справа живой TUI выбранной сессии.

- Сессии берутся из `claude agents --json --all` (`src/cc_fleet/fleet_data.py:42`), опрос по таймеру.
- Выбранная сессия — `claude attach <id>` в PTY (`src/cc_fleet/app.py:102-117`), экран держит свой эмулятор `vterm.py` (623 строки, `src/cc_fleet/vterm.py:48`, разбор — `:448`).
- Активная сессия одна: `self.session = Session(...)` (`src/cc_fleet/app.py:925`), переключение — новый attach.
- Состояние — JSON в `~/.config/cc-fleet/` (`src/cc_fleet/fleet_data.py:10-11`), путь переопределяется `CC_FLEET_CONFIG`, этим пользуются тесты.
- Уведомления через `osascript`, `terminal-notifier`, `notify-send` или свою команду (`src/cc_fleet/notify.py:51-66`); PR по worktree через `gh pr view --json` (`src/cc_fleet/pulls.py:62`); keep-awake через `caffeinate -is -w <pid>` (`src/cc_fleet/awake.py:15-16`).

### 1.2 Что сделано правильно

- Ничего не нужно регистрировать руками: всё, что запущено через `claude --bg`, появляется в списке само. Этого нет ни у agterm, ни у herdr (§3).
- Родной TUI Claude вместо своего чата: пользователь не теряет ни одной функции Claude Code.
- Всё с клавиатуры: категории, пины, архив, переходы.
- Тесты на каждую фичу (`tests/test_*.py`, 15 файлов) и изоляция от настоящего конфига через переменную окружения.

### 1.3 Где болит

- Холодный attach при каждом переключении. Замер на 13 фоновых сессиях, 160×45, ответы на запросы терминала через `vterm.py`, сессии без ввода:

```
первый прогон                      повторный
e1e73b0e  first=133  full=922 ms   first=133 full=136 ms
5eaf2a90  first=135  full=556 ms
6b1bf0c9  first=151  full=629 ms
13444260  first=138  full=807 ms   first=134 full=137 ms
d0d2ebbc  first=136  full=252 ms   first=152 full=155 ms
claude --version: 7–8 ms
```

`first` — первый байт после `fork`, `full` — последний байт перед 400 мс тишины. Вывод: 130 мс уходят на запуск attach и подключение к демону Claude, а не на отрисовку; лечится только держанием attach открытым.

- Память: 13 фоновых сессий Claude — 2161 МБ RSS (около 166 МБ на сессию), один `claude attach` — 59–126 МБ. Для сравнения, Electron-приложение (Obsidian) в простое — 149 МБ. Мерилось через `ps -o rss`.
- `vterm.py` на Python: разбор в том же потоке, что и UI, и собственный эмулятор с дырами в покрытии VT.
- TUI внутри терминала: нет панелей, нет плагинов, раскладка фиксирована.

### 1.4 Что забираем

Источник сессий (`claude agents --json`), модель категорий, пинов, архива и непрочитанных, тексты и переходы уведомлений, поиск PR, keep-awake, тесты как спецификацию. Не забираем `vterm.py` и холодный attach.

### 1.5 Attach при stop, respawn и rm

Проверено на живом `claude attach` в PTY, claude 2.1.325, macOS. Все три команды сами завершаются с кодом 0 за 0,2–0,6 с, stdout `stopped <id>`, `respawned <id>`, `removed <id>`. Живой attach выходит сам, примерно через 0,5 с после начала команды, независимо от её длительности: он замечает конец сессии, а не получает сигнал от команды.

| Команда | Код attach | stderr attach |
|---|---|---|
| `claude stop <id>` | 0 | ``Session <id> has exited — run `claude attach <id>` to resume it.`` |
| `claude respawn <id>` | 0 | то же, что после stop |
| `claude rm <id>` | 0 | `Session <id> has exited.` |

- `claude attach <id>` после `rm`: код 1, stderr `No job matching '<id>'. Run 'claude agents' to list running sessions.`
- `rm` живой сессии, занятой или простаивающей, не отказывает без `stop`: останавливает и удаляет. Каталог `~/.claude/jobs/<id>` исчезает, транскрипт остаётся.
- `stop`, `rm` и `respawn` на удалённом id: код 1, stdout пуст. stderr у `stop` — `No job matching '<id>'. Run 'claude agents' to list running sessions.`, у `rm` и `respawn` — `No job matching '<id>'`.
- `respawn` оставляет id и исходные флаги (`--model`, `--effort`, `-n`), меняются pid и `startedAt`. Сразу после него `status` равен `null`, `idle` — через ~3 с.
- `claude agents --json --all` после `stop`: сессия в списке, без `pid` и `status`, `state` равен `done`. Без `--all` её нет. После `rm` сессии нет и с `--all`.

Для roost:

- Окну и демону не нужно закрывать attach перед `stop` или `rm`: он выходит сам с кодом 0.
- Тёплая сессия `roostd` заканчивается по выходу attach; при `rm` ещё и по `session.removed` из `sessions.list`.
- Не проверено: `respawn --all`, Linux, отказ `rm` для сессии с worktree (`--discard-unpushed`, `--force-remove-worktree`), `stop` уже остановленной сессии.

---

## 2. libghostty-vt

### 2.1 Что даёт

C API к ядру эмулятора Ghostty: разбор VT, экран и scrollback, render state для рендерера, энкодеры клавиш и мыши, formatter, снимки. Рендера и PTY нет. Заголовки — `include/ghostty/vt/*.h`, сборка — Zig, `minimum_zig_version = "0.17.1"` (`build.zig.zon:7`).

### 2.2 Потоки

- Терминал не потокобезопасен: «This function is not thread-safe with other operations on the same terminal. You must serialize it with writes, rendering, searches, and other terminal access» (`include/ghostty/vt/terminal.h:3494-3496`, сказано о сжатии scrollback; общего заявления о потокобезопасности в заголовках не нашёл, вывод — сериализовать всё).
- Обновление render state двухфазное ровно под схему «поток ввода-вывода плюс поток рендера»: под локом нужна только `ghostty_render_state_begin_update`, а `end_update` «exclusively reads and writes memory owned by the render state, so it is safe to call while the terminal is being modified» (`include/ghostty/vt/render.h:668-678`). `ghostty_render_state_update` — обе фазы подряд (`render.h:645-654`).
- Колбэки ввода-вывода вызываются синхронно в вызывающем потоке (`include/ghostty/vt/io.h:57`).

### 2.3 Снимок

`include/ghostty/vt/snapshot.h` кодирует и восстанавливает полное состояние терминала в бинарном формате. Формат версионируется отдельно и «isn't covered by the ABI promise» (`snapshot.h:31-32`). Терминал кодируется без continuation tracking, только если VT-парсер и декодер UTF-8 в ground-состоянии; иначе нужен `GHOSTTY_TERMINAL_OPT_CONTINUATION_MAX_BYTES`, включённый до записи, или вызов вернёт `GHOSTTY_INVALID_VALUE` (`snapshot.h:400-403`). Вывод: снимок годится для передачи между процессами одной сборки, но не для хранения между версиями.

### 2.4 Как подключают другие

| Проект | Как | Где |
|---|---|---|
| herdr | исходники в `vendor/libghostty-vt`, коммит записан в `vendor/libghostty-vt.vendor.json`, свой Rust-крейт со сборкой через Zig | `crates/ghostty-vt/build.rs:35-46` |
| agterm | `GhosttyKit.xcframework` (весь libghostty с рендером, не только vt), мост адаптирован из macterm | `ARCHITECTURE.md:26` |

Вывод для roost: вендорить исходники, как herdr, надёжнее `FetchContent` с `GIT_TAG main` — сборка не зависит от сети и от движения ветки. Решение в design.md: FetchContent на зафиксированном коммите; вендоринг — запасной вариант, если CI упрётся в сеть.

### 2.5 Память терминала

Метод: `TerminalSession` 200×50, как в окне (терминал, render state, мьютекс); N штук в одном процессе, рост `phys_footprint` и RSS делится на N. Поток — раскрашенные строки (SGR truecolor и 256, bold/dim/italic, CJK, emoji), scrollback наполняется, пока размер не перестанет расти. Три отдельных запуска при N = 20, плюс N = 1 и N = 100.

| Состояние (N = 20) | footprint, МиБ | RSS, МиБ |
|---|---|---|
| создан | 0,03 | 0,05 |
| экран 200×50 текста со стилями | 0,17 | 0,60 |
| scrollback полон | 0,7–0,85 | 1,2 |
| после одного `begin_update`/`end_update` | 1,55 | 1,97 |

Цифра на сессию зависит от N: после первого update footprint на сессию 3,97 МиБ при N = 1, 1,55 при N = 20, 1,47 при N = 100 — в малых N сидит доля постоянных расходов процесса. Предельная цена ещё одной сессии — около 1,5 МиБ.

Лимит scrollback в roost не задан, действует умолчание библиотеки: `max_scrollback_bytes = 10_000` без лимита по строкам (`src/terminal/Terminal.zig:289`, `include/ghostty/vt/terminal.h`, `GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES`). История режется целыми страницами (`std_capacity` 215×215 ячеек, `src/terminal/page.zig:2038`), поэтому строк в терминале то больше, то меньше на страницу: от ~265 до ~475 вместе с экраном, то есть от ~215 до ~425 строк истории. Footprint в строке «scrollback полон» зависит от того, в какой точке этой пилы остановился поток (разброс ±0,06 МиБ при сдвиге потока на 250 строк). Рост лимита ведёт к росту памяти; насколько линейно, не мерилось.

Сборка: release, arm64, macOS 26.7, libghostty-vt `vt-1.0.1` (`cmake/ghostty.cmake`). Запуск: `cmake --build build/release --target terminal_memory && build/release/bin/terminal_memory <N>`, исходник `tests/bench/terminal_memory.cpp`, в ctest не входит.

Вывод: сам терминал стоит окну около 1,5 МиБ на тёплую сессию при умолчании scrollback; четыре тёплые — около 6 МиБ. Это только терминал и render state: память Qt, scene graph и GPU-ресурсов окна на сессию здесь не мерилась, и с attach в `roostd` (§1.3) сравнивать её напрямую нельзя.

Прирост на первом update (около 0,8 МиБ) остаётся, пока жива сессия; чем он занят, не разбиралось.

---

## 3. Соседи

### 3.1 agterm

Swift, MIT, только macOS 14+. Нативные панели на libghostty, сайдбар по рабочим пространствам с глифами active/blocked/completed и счётчиками непрочитанного.

- Что сделано правильно: модель отделена от UI — `agtermCore` не импортирует ни GhosttyKit, ни AppKit и тестируется без хоста (`ARCHITECTURE.md:9`). Установщик хуков идемпотентен и делает `.bak` чужих конфигов (`agterm/agterm/AgentHooksInstaller.swift:3-13`).
- Где болит для нас: только macOS; сессии добавляются вручную, фоновые сессии Claude сами не подхватываются; хоткеев для переключения и раскладки по категориям не хватает (по опыту использования, не по коду).
- Хуки: пять хуков Claude Code в `~/.claude/settings.json`, шесть хуков Codex в `$CODEX_HOME/config.toml`, расширения Pi и OpenCode, плюс строка `source` в `~/.zshrc`/`~/.bashrc`/fish (`AgentHooksInstaller.swift:3-10`). Это ровно то вмешательство в чужие конфиги, которое roost делает только по согласию.

### 3.2 herdr

Rust, Apache-2.0, TUI-мультиплексор с сервером, переживающим клиента. Тоже на libghostty-vt (§2.4).

- Состояние агента — по тексту внизу экрана: «Agent state detection via terminal tail pattern matching» (`src/detect/mod.rs:1-4`), состояния `Idle`, `Working`, `Blocked`, `Unknown` (`src/detect/mod.rs:11-20`), шаблоны на агента в `src/detect/manifests/*.toml` (claude, codex, gemini, cursor и др.).
- Плагины: каталог с `herdr-plugin.toml` (`src/app/api/plugins/manifest.rs:124`), управляемые установки из GitHub (`src/plugin_paths.rs:5-13`).
- Что забираем: детект по экрану как запасной источник состояния для агентов без API и хуков. У Claude есть `claude agents --json`, ему это не нужно.
- Чего нет: UI плагинов (это TUI), десктопного окна.

### 3.3 Остальные

Сведения из README и документации, код не читался.

| Проект | Стек, лицензия | Чем полезен | Почему не форк |
|---|---|---|---|
| Wave Terminal | Go + Electron + xterm.js, Apache-2.0 | блоки и вкладки, SSH, бейджи от хуков Claude через `wsh badge` | плагинов нет, только JSON-настройка встроенных блоков (https://docs.waveterm.dev/customwidgets) |
| Paradev (бывш. Superset) | Electron/React, MIT | worktree и терминал на задачу, 20+ агентов | Linux экспериментальный, система плагинов не проверена |
| runner | Rust, GPUI, MIT | демон `runnerd` держит агентов в PTY на `alacritty_terminal` | Linux в бете, о плагинах в README ничего |
| sheprd | Rust, GPUI, Apache-2.0 | GPUI-клиент к демону herdr, расширения на Luau | привязан к herdr |
| Nimbalyst | Electron/React, MIT | контракт `EditorHost` для панелей-расширений | свой UI вместо TUI агента |
| Claude Squad | Go, tmux, AGPL-3.0 | worktree на сессию | TUI, AGPL |
| opcode | Tauri, AGPL-3.0 | GUI над Claude Code | только Claude, AGPL, последний релиз 2025-08 |

Ни у одного нет одновременно: автоматического подхвата `claude --bg`, плагинов с UI-панелями поверх настоящего TUI и связей между панелями.

---

## 4. Протоколы агентов

| Агент | Фоновые сессии | Статус | Структурный канал | TUI извне |
|---|---|---|---|---|
| Claude Code | `claude --bg`, `attach`, `agents --json [--all]` (проверено `claude --help`, 2.1.286) | поле `state`: working, blocked, done, failed, stopped; хуки `Notification`, `PermissionRequest`, `Stop` и др. (https://code.claude.com/docs/en/hooks) | `-p --output-format stream-json` | `claude attach` |
| Codex | своего демона сессий нет; `codex app-server` | `item/commandExecution/requestApproval` (`codex-rs/app-server/README.md:9`), `thread/list` (`:28`, `:38`); `thread/status/changed` — не проверено | app-server JSON-RPC | `codex resume` в своём PTY |
| OpenCode | `opencode serve` | SSE `/event`, ответы на разрешения через HTTP (https://opencode.ai/docs/server/) | HTTP + SSE | TUI-клиент к серверу |
| Gemini CLI | нет | хуки (https://geminicli.com/docs/hooks/) | `--output-format stream-json` | только свой PTY |
| ACP | — | `session/list`, запросы разрешений (https://agentclientprotocol.com) | JSON-RPC | не поддерживается: ACP заменяет TUI клиентским UI |

Вывод: ACP как единственный интерфейс не подходит — он не показывает родной TUI и не знает отсоединённых сессий. Годится запасным адаптером для агентов без API богаче.

Открыто: можно ли ответить на запрос разрешения сессии `--bg`, не подключаясь к ней, — не проверено.

---

## 5. Стек UI

### 5.1 Qt Quick: потоки

«The threaded renderer is currently used by default on Windows with Direct3D 11 …, Linux excluding Mesa llvmpipe, macOS with Metal, mobile platforms, … and with Vulkan regardless of the platform» (https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html). Пока поток рендера рисует, GUI-поток свободен для событий и анимаций. JavaScript QML выполняется в GUI-потоке.

Платформа `offscreen`, на которой идут тесты в CI, не объявляет `RhiBasedRendering` (`src/plugins/platforms/offscreen/qoffscreenintegration.cpp:358`, qtbase `d8133df`), и Qt Quick тогда выбирает программный бэкенд `software` (`src/quick/scenegraph/qsgcontextplugin.cpp:117-119`, qtdeclarative `aa3b2a9`). У программного бэкенда поток рендера включается только переменной `QSG_RENDER_LOOP=threaded` (`src/quick/scenegraph/adaptations/software/qsgsoftwareadaptation.cpp:40-50`). Вывод: без этой переменной тест кадра на `offscreen` мерит GUI-поток вместе с рендером, а гонки между ними не возникают вовсе.

### 5.2 Qt: лицензии

Только под GPLv3: «Qt Canvas Painter, Qt CoAP, Qt Graphs, Qt GRPC, Qt HTTP Server, Qt Lottie Animation, Qt MQTT, Qt Network Authorization, Qt Qml Compiler, Qt Quick 3D, Qt Quick 3D Physics, Qt Quick Timeline, Qt Virtual Keyboard, Qt Wayland Compositor» (https://doc.qt.io/qt-6/licensing.html). Остальное, включая Core, Gui, Qml и Quick, — LGPLv3. Что именно входит в Qt Qml Compiler и касается ли это `qmlcachegen`, на странице не сказано.

### 5.3 xterm.js и Electron

- Разбор и рендер в главном потоке renderer'а; вынос в worker закрыт как непрактичный: «multiple terminals in one page will slow down each other» (https://github.com/xtermjs/xterm.js/issues/3368).
- Вывод обрабатывается порциями короче кадра (16 мс), пропускная способность «5 - 35 MB/s» (https://xtermjs.org/docs/guides/flowcontrol/).
- Около 16 WebGL-контекстов на страницу, сверх лимита гаснет самый старый терминал (https://github.com/xtermjs/xterm.js/issues/6068).
- Мейнтейнер: парсер «hit some hard limits around what's possible in JS», изучают переход на libghostty (https://github.com/xtermjs/xterm.js/issues/5686).
- Задержка нажатия: на macOS первоисточника с цифрами для xterm.js нет. Linux, 2024, Typometer, среднее: xterm 5,3 мс, alacritty 6,9 мс, Hyper (Electron + xterm.js) 39,8 мс (https://beuke.org/terminal-latency/).

### 5.4 Прочие фреймворки

| Фреймворк | Что выяснено | Источник |
|---|---|---|
| Tauri | на Linux системный WebKitGTK с известными проблемами графики; WebGL-рендер xterm.js под ним | https://v2.tauri.app/develop/debug/linux-graphics/; https://github.com/xtermjs/xterm.js/issues/5847 |
| GPUI | gpui 0.2.2 (2026-10-30), API до 1.0; у расширений Zed нет своих панелей | crates.io; https://github.com/zed-industries/zed/issues/18496 |
| Dear ImGui | v1.93.0 (2026-10-14); простоя без ввода из коробки нет (https://github.com/ocornut/imgui/issues/9444); HarfBuzz нет (#9518); ImHex отказывается грузить плагин чужой версии | https://github.com/WerWolv/ImHex/blob/master/lib/libimhex/source/api/plugin_manager.cpp |
| Slint | лицензия без отчислений «does not permit the distribution of Application that exposes the APIs» | https://github.com/slint-ui/slint/blob/master/LICENSES/LicenseRef-Slint-Royalty-free-2.0.md |
| RmlUi | на Wayland без буфера обмена, HiDPI и IME, на X11 без HiDPI и IME; полноценно через SDL | README https://github.com/mikke89/RmlUi |
| egui, iced | терминальные виджеты на `alacritty_terminal` есть, UI плагинов на лету нет | README egui_term, iced_term |

Размер релиза под macOS arm64 по GitHub Releases: Tracy (ImGui) — 25 МБ zip, ImHex (ImGui) — 45 МБ dmg, runner (GPUI) — 44 МБ dmg, Nheko (Qt Quick) — 69 МБ dmg, Wave (Electron) — 190 МБ dmg. Отсюда оценка «Qt добавляет 25–45 МБ» в design.md: это разница между приложениями разного объёма, а не замер одного приложения на двух стеках.
