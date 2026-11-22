# 17. Обновления

Окно узнаёт, что вышел релиз новее его самого, и говорит об этом одной строкой. Установки пока нет: ставит релиз пользователь сам, по ссылке на страницу релиза (шаги — #250 и #251 в плане #252).

## Проверка

`roost::Updates` (`src/qml_api/updates.hpp`, `Roost.updates`) живёт в окне, не в `roostd`. Первая проверка идёт, когда окно подключилось и прочитало документ `settings`; дальше таймер каждые 24 часа. Запрос — `GET https://api.github.com/repos/zexUlt/roost/releases/latest` через `QNetworkAccessManager` (`Accept: application/vnd.github+json`, `User-Agent: roost/<версия>`), таймаут передачи 30 с, повторов нет: следующая попытка — по таймеру. Второе окно проверяет само.

- Из ответа берутся `tag_name` (`vX.Y.Z`) и `html_url`. Версии сравниваются числами по частям (`0.10.0` новее `0.9.0`), а не текстом; версия окна — начало `VersionString()` (`src/core/version.hpp`). Тег не вида `vX.Y.Z` (суффикс, две части, нет `v`) не предлагается и не ошибка. Предлагается только версия новее версии окна; `draft: true` и `prerelease: true` не предлагаются.
- Сбой (ошибка сети, HTTP не 200, ответ не объект JSON, нет строк `tag_name` и `html_url`) ничего не показывает и пишется в лог одной строкой `qWarning` на причину: пока та же причина повторяется, строка не повторяется; хороший ответ сбрасывает счёт. Причина — `HTTP <код>`, `network error: <текст Qt>`, `the answer is not a JSON object`, `the answer has no tag_name and html_url`.
- HTTPS идёт через TLS-бэкенд Qt: в AppImage `usr/plugins/tls/libqopensslbackend.so` (libssl3 он берёт из системы во время работы), в `.app` `Contents/PlugIns/tls/libqsecuretransportbackend.dylib` (TLS системы). Оба файла проверяют `tools/check_appimage.sh` и `tools/check_bundle.sh` (11-build.md). Без бэкенда проверка — сбой `network error`.
- Адрес запроса для тестов подменяет переменная окружения `ROOST_TEST_UPDATES_URL`; в работе её не задают. Пустая, но заданная — запросов нет вообще: так её ставят бинарники тестов (`tests/helpers/test_environment.cpp`), чтобы ни один тест не ходил в GitHub.

## Настройка

`settings.updates.check` (05-plugins.md, общий раздел настроек): булево, по умолчанию `true`. `false` отменяет идущий запрос и снимает таймер; запросов нет, пока ключ `false`. Правка `settings` действует в окне сразу (`store.changed`): `true` запускает проверку сразу. Не булево значение ключа и не объект в `updates` — значение по умолчанию (проверка включена), строка в журнал и строка в полосе проблем: `settings.updates.check must be true or false; the check is on`, `settings.updates must be an object with check; the check is on` (как проблемы шрифтов, `Roost.updates.problems`).

## Строка в окне

`Roost.updates` (только чтение): `available` — версия без `v` или пустая строка, `url` — страница релиза, `enabled` — проверка включена, `problems` — проблемы настройки; один сигнал `changed`. `Main.qml` показывает строку `updateStrip` между раскладкой и полосой проблем: `roost v<версия> is out`, кнопка «Release page» (`Qt.openUrlExternally(url)`) и «Dismiss». Это предложение, а не проблема: цвета — токены `controlBackground` и `windowText`, в `shownProblems` её нет. Строка одна на версию.

Dismiss прячет предложение, пока не выйдет версия новее отклонённой. Отклонённая версия лежит в документе окна `updates` (`updates.json`, 08-state.md): `{"dismissed": "X.Y.Z"}`, пишется одним `store.patch`. Пока документ не прочитан, строки нет: отклонённая версия не мелькает при старте. Окна следят за документом (`store.changed`), поэтому отказ в одном окне прячет строку и в другом. Не прочитался документ — как пустой.

## Окно настроек

Общий раздел: переключатель «Check for updates» пишет `updates.check` одним `store.patch` одного ключа (`{updates: {check: false}}`), как поля шрифтов; не удалась запись — переключатель возвращается к хранимому значению, причина под полями.

## Тесты

`tests/roost/updates_test.cpp`: настоящее окно, настоящий `roostd`, настоящее хранилище; GitHub заменён сервером на loopback с готовым ответом (новее, то же и старее, draft и prerelease, плохой тег, HTTP 500 и 404, не JSON, соединение отвергнуто, `updates.check: false`, Dismiss и перезапуск окна). Переключатель — `tests/roost/settings_window_test.cpp`.
