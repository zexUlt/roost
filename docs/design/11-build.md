# 11. Сборка, тесты, упаковка

- CMake с пресетами, Qt 6 (в CI одна версия, ставится через aqtinstall), C++23, clang, Zig для libghostty-vt.
- Тесты: Qt Test для демона и моделей; Qt Quick Test для QML с `QT_QPA_PLATFORM=offscreen`; адаптеры — на записанном выводе `claude agents --json` плюс тест на настоящем `claude`, который включается явно.
- CI на GitHub Actions: сборка и тесты на macOS arm64 и Linux x64, гейты format и tidy.
- Упаковка: macOS — `.app` через `macdeployqt` в `.dmg`; Linux — AppImage через `linuxdeploy-plugin-qt`.
- SDK: `roost/plugin.h` (C), `roost/plugin.hpp` (C++-обёртка, только заголовки), `RoostSdkConfig.cmake` с `roost_add_plugin(...)`: скрытая видимость, экспорт только `roost_plugin_*`, структура каталога плагина. Архив SDK на каждый релиз, подключается через `find_package(RoostSdk)`.
- Пример и шаблоны: `samples/session-stats/` (нативный плагин на C++ и QML), `templates/plugin-qml/` и `templates/plugin-native/`. В основную сборку они не входят и собираются в CI отдельно, как сторонний плагин: только против установленного SDK и Qt. Так проверяется, что SDK и шаги из README шаблона достаточны, а пример не опирается на внутренности roost.
- Проверки ABI в CI: на Linux `abidiff` между библиотекой с публичными типами SDK, собранной из HEAD и из последнего тега `sdk-v*`, (дописывание в конец структуры подавлено); на обеих ОС тест размеров и смещений — `sizeof`, `offsetof`, `alignof` публичных структур против файла в репозитории; плагин-фикстура, собранная против прошлого SDK, грузится текущим roost, а фикстуры с чужим major и слишком новыми SDK или Qt отвергаются. В 0.x `abidiff` только сообщает, остальное блокирует.
