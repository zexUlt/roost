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
- `agent.listSessions` и `agent.attachCommand` адаптер Claude отвечает по последнему запуску. До первого запуска запросы ждут его, после неудачного получают ошибку с причиной. Команда подключения к сессии из списка — `claude attach <id>`, к остальным — `null`. После каждого удачного запуска, кроме первого удачного, адаптер шлёт `session.stateChanged` о новых сессиях и сменивших состояние относительно предыдущего удачного запуска. Сессия, которая была в предыдущем удачном запуске и пропала из этого, — удалена: адаптер шлёт о ней `session.removed {agent, session}` после `session.stateChanged` того же запуска, в порядке предыдущего списка. Неудачный запуск ничего не удаляет; удаление за время неудачных запусков уходит после следующего удачного. Вернувшаяся сессия приходит как `session.stateChanged`. Цена: удаление видно за один опрос (по умолчанию 3 с; интервал — флаг адаптера). Подтверждения вторым запуском нет, и это решение с обменом: сессия, которую `claude agents` один раз пропустил, исчезает из окна и возвращается в конец списка (как `session.stateChanged`); правило «два запуска подряд» убрало бы это, но добавило бы к задержке ещё около одного опроса. После перезапуска бэкенда у адаптера нет предыдущего списка, поэтому удаления и смены состояния за время простоя доходят до окна только через `sessions.list`; само окно список не обновляет. Отвергнуто: исчезновение процесса сессии — выход процесса есть смена состояния (`done`/`failed`), сессия остаётся в списке, к ней можно подключиться или запустить её заново, поэтому с удалением это не совпадает; слежение за файлами `~/.claude/jobs` (внутренний формат Claude) и событие только на собственное `remove` из roost (не видит удаления снаружи). `claude agents` запускается в своей группе процессов; зависший запуск убивается вместе со всем, что он запустил.

| Агент | Сессии и состояние | Видимый TUI | Свой демон |
|---|---|---|---|
| Claude Code | `claude agents --json`, хуки | `claude attach` в PTY | да, `--bg` |
| Codex | `codex app-server`: `thread/list` и т. п. | `codex resume` в PTY `roostd` | нет |
| OpenCode | `opencode serve`: HTTP + SSE | TUI в PTY `roostd` | да |
| Gemini и др. | хуки, ACP | TUI в PTY `roostd` | нет |

Протоколы — prior-art §4. Установка хуков агента (`Notification` у Claude Code, `~/.codex/hooks.json`) меняет конфиг чужого инструмента, поэтому делается только по явному согласию в UI (ADR 012).
