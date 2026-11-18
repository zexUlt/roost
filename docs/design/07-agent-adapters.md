# 7. Адаптеры агентов

```cpp
struct AgentAdapter {
    virtual Capabilities GetCapabilities() const = 0;   // backgroundDaemon, nativeTuiAttach, structuredStream
    virtual QFuture<std::vector<Session>> ListSessions() = 0;
    virtual QFuture<SessionRef> Start(const StartOptions&) = 0;
    virtual QFuture<void> Stop(const SessionRef&) = 0;
    virtual QFuture<std::optional<PtySpec>> AttachCommand(const SessionRef&) = 0;  // roostd запускает в PTY
    virtual QFuture<void> Send(const SessionRef&, const QString& text) = 0;
    virtual QFuture<void> AnswerPermission(const SessionRef&, const QString& requestId, const PermissionAnswer&) = 0;
    // события: StateChanged, Removed, PermissionRequested, TitleChanged, Usage
};
```

- Асинхронность — `QFuture<T>` из QtCore (`QPromise`, `then`): без корутин и без новой зависимости.
- Внешние адаптеры работают как бэкенды плагинов; `roostd` оборачивает их мостом, который реализует этот интерфейс поверх JSON-RPC. Адаптер Claude — встроенный плагин на том же протоколе.
- Мост — `src/roostd/adapter_bridge.hpp`, по одному на агента. Демон зовёт бэкенд `agent.listSessions {agent}` → `[{id, state, title?, cwd?, startedAt?}]` и `agent.attachCommand {agent, session}` → `{program, args?, cwd?, env?}` или `null`, если к сессии не подключиться; `env` — объект `{ИМЯ: значение}`. Бэкенд сам шлёт уведомления `session.stateChanged {agent, session}` и `session.removed {agent, session}` (`session` — строка с id) о своих агентах. Ответ не по схеме — `InternalError` с причиной; уведомление о чужом агенте или не по схеме демон пишет в лог и отбрасывает. Пока реализованы только эти три части интерфейса. Ошибка адаптера без `message` попадает в `unavailable` как `error <code>`. Агент, объявленный плагином дважды, получает один мост.
- Общие состояния: `working`, `blocked` (ждёт человека), `idle`, `done`, `failed`.
- Адаптер Claude — бэкенд `claude-adapter` плагина `plugins/claude/` (`plugins/claude/backend/adapter.hpp`). Он запускает `claude agents --json --all` через 3 с после конца предыдущего запуска, по одному за раз; запуск дольше 20 с убивается. Берёт только сессии с `kind: background`, повтор id отбрасывает. Заголовок — `name`, без него — id. Состояние — из `state`: `needs_input` — `blocked`, `done` при `status: busy` — `working`, неизвестное — `idle` с записью в лог.
- `agent.listSessions` и `agent.attachCommand` адаптер Claude отвечает по последнему запуску. До первого запуска запросы ждут его, после неудачного получают ошибку с причиной. Команда подключения к сессии из списка — `claude attach <id>`, к остальным — `null`. После каждого удачного запуска, кроме первого удачного, адаптер шлёт `session.stateChanged` о новых сессиях и сменивших состояние относительно предыдущего удачного запуска. Сессия, которая была в предыдущем удачном запуске и пропала из этого, — удалена: адаптер шлёт о ней `session.removed {agent, session}` после `session.stateChanged` того же запуска, в порядке предыдущего списка. Неудачный запуск ничего не удаляет; удаление за время неудачных запусков уходит после следующего удачного. Вернувшаяся сессия приходит как `session.stateChanged`. Цена: удаление видно за один опрос (по умолчанию 3 с; интервал — флаг адаптера). Подтверждения вторым запуском нет, и это решение с обменом: сессия, которую `claude agents` один раз пропустил, исчезает из окна и возвращается в конец списка (как `session.stateChanged`); правило «два запуска подряд» убрало бы это, но добавило бы к задержке ещё около одного опроса. После перезапуска бэкенда у адаптера нет предыдущего списка, поэтому удаления и смены состояния за время простоя событиями не приходят. Петля замыкается демоном и окном: демон публикует `session.adapterStarted {plugin, agents}` (§4), окно заново читает `sessions.list`. Контракт адаптера: после старта `agent.listSessions` отвечает только по списку собственного процесса и ждёт первого, чтобы перечитанный список не оказался старым. Отвергнуто: исчезновение процесса сессии — выход процесса есть смена состояния (`done`/`failed`), сессия остаётся в списке, к ней можно подключиться или запустить её заново, поэтому с удалением это не совпадает; слежение за файлами `~/.claude/jobs` (внутренний формат Claude) и событие только на собственное `remove` из roost (не видит удаления снаружи). `claude agents` запускается в своей группе процессов; зависший запуск убивается вместе со всем, что он запустил.

- Команды адаптера Claude — вклады плагина `contributes.commands`; окно зовёт `Roost.commands.run(id, args, cb)`, демон пересылает бэкенду `commands.run {command, args?}` (§5). Идентификатор — `<агент>.<глагол>`: общий список по `agent` строки находит команду и проверяет её наличие в `plugins.list`.

  | Команда | `args` | Запускает |
  |---|---|---|
  | `claude.stop` | `{session}` | `claude stop <session>` |
  | `claude.remove` | `{session}` | `claude rm <session>` |
  | `claude.respawn` | `{session}` | `claude respawn <session>` |
  | `claude.respawnAll` | нет или `{}` | `claude respawn --all` |

  - Результат успеха — `null`. `session` — id из букв, цифр, `_` и `-`, не начинается с `-`: иначе `claude.respawn {session: "--all"}` перезапустил бы все сессии. Остальное в `args` игнорируется. Неверные `args` и неизвестная команда — InvalidParams, ничего не запускается.
  - Выход `claude` с кодом не 0 — `InternalError`; `message` — первая непустая строка stderr, кроме строк-предупреждений (начинаются с `warn:` или `warning:`, без учёта регистра), иначе такая же строка stdout, иначе `claude <глагол> exited with code N`; `data` — `{exitCode, stdout, stderr}`, каждое поле обрезано до 4 КиБ. Не запустился — `cannot run claude <глагол>: …`, упал — `claude <глагол> crashed`. Таймауты: `stop` и `rm` 60 с, `respawn` и `respawn --all` 300 с; после таймаута группа процессов убивается, ответ — `claude <глагол> did not finish in N ms`. Флаг адаптера `--command-timeout-ms` задаёт один таймаут для всех команд. У запроса от бэкенда нет таймаута на стороне демона (§5), поэтому таймаут нужен самому адаптеру.
  - Каждая команда — свой процесс в своей группе, параллельно опросу и другим командам. Закрытый stdin убивает все идущие команды, их запросы получают `InternalError`.
  - После конца каждой команды, удачной или нет, адаптер сразу запускает `claude agents`; если запуск уже идёт, второй стартует сразу после него: идущий мог начаться до команды и не знать о её действии. Удаление приходит событием `session.removed` после следующего успешного запуска `claude agents`, а не через опрос; неудачный или просроченный запуск откладывает его.
  - `claude rm` сам останавливает живую сессию, поэтому `stop` перед ним не нужен. Живой `claude attach` после `stop`, `respawn` и `rm` завершается сам кодом 0 примерно через 0,5 с (prior-art §1.5): окно этого не ждёт и не закрывает подключение первым. После `stop` и `rm` окно не открывает сессию заново, иначе `attach` возобновит остановленную; после `respawn` открывает заново, когда панель закончилась (`TerminalItem.reopen()`).
  - Отвергнуто: метод протокола, который выгоняет тёплое подключение до `stop` и `rm` (подключение закрывается само, протокол менять ради 0,5 с старого экрана незачем); методы `Start` и `Stop` интерфейса адаптера в демоне за методами `session.*` (потребовало бы менять демон и протокол; второй агент следует соглашению об именах команд).

| Агент | Сессии и состояние | Видимый TUI | Свой демон |
|---|---|---|---|
| Claude Code | `claude agents --json`, хуки | `claude attach` в PTY | да, `--bg` |
| Codex | `codex app-server`: `thread/list` и т. п. | `codex resume` в PTY `roostd` | нет |
| OpenCode | `opencode serve`: HTTP + SSE | TUI в PTY `roostd` | да |
| Gemini и др. | хуки, ACP | TUI в PTY `roostd` | нет |

Протоколы — prior-art §4. Установка хуков агента (`Notification` у Claude Code, `~/.codex/hooks.json`) меняет конфиг чужого инструмента, поэтому делается только по явному согласию в UI (ADR 012).
