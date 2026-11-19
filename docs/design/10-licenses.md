# 10. Лицензии и зависимости

| Зависимость | Лицензия | Как подключена |
|---|---|---|
| Qt 6: Core, Gui, Qml, Quick, Quick Controls, Network | LGPLv3 | динамически, заменяемые библиотеки в `.app` и AppImage |
| Qt Test | LGPLv3 | только тесты |
| libghostty-vt | MIT | из исходников, статически |
| Zig | MIT | только сборка |
| ICU 73 (из Qt сборки) | ICU (MIT-подобная) | только AppImage, динамически, рядом с Qt; текст лицензии — `licenses/ICU-LICENSE.txt` (`icu4c/LICENSE` тега `release-73-2`), в образе `usr/share/licenses/roost/ICU-LICENSE.txt` |
| Библиотеки xcb в AppImage: xcb-util (cursor, icccm, image, keysyms, render-util, util), libxcb-shape, libxcb-xkb, libxkbcommon-x11 | MIT / X11 | только AppImage: `linuxdeploy` копирует с машины сборки то, чего может не быть на десктопе, и `copyright` пакета; динамически, заменяемые. Библиотеки, которые есть на любом десктопе (GLib, D-Bus, systemd, сжатие, Kerberos, libpng, X11), в образ не кладутся: список в `packaging/linux/system-libraries.txt` |
| Чужой код внутри Qt (HarfBuzz, PCRE2, double-conversion, md4c и др.) | BSD, MIT, Apache-2.0, FTL и др. | внутри бинарников Qt, оба пакета; список — раздел ниже |
| Рантайм AppImage: musl, libfuse 3, squashfuse, zstd, zlib, mimalloc | MIT, LGPL 2.1, BSD, Zlib | статически в рантайме, который стоит перед squashfs в каждом AppImage; раздел ниже |

Обязательства LGPLv3: динамическая линковка, библиотеки Qt заменяемы, тексты LGPLv3 и GPLv3 и ссылка на исходники Qt лежат рядом. Статической линковки Qt нет.

Модули только под GPLv3 не подключаются: Qt Canvas Painter, Qt CoAP, Qt Graphs, Qt GRPC, Qt HTTP Server, Qt Lottie Animation, Qt MQTT, Qt Network Authorization, Qt Qml Compiler, Qt Quick 3D, Qt Quick 3D Physics, Qt Quick Timeline, Qt Virtual Keyboard, Qt Wayland Compositor (prior-art §5.2). Отсюда же отказ от AOT-компиляции QML в v1: что именно входит в Qt Qml Compiler, не проверено.

## Чужой код внутри Qt и рантайма AppImage

Лицензии требуют, чтобы уведомления шли вместе с бинарниками. Их несут два раздела `THIRD_PARTY_LICENSES`: «Third-party code inside Qt» и «AppImage runtime». Отдельных файлов нет: `cmake/Install.cmake`, проверка раскладки и `tools/check_*.sh` остаются как есть, файл целиком лежит в обоих пакетах.

Состав раздела про Qt:

- Источник — `qt_attribution.json` в qtbase и qtdeclarative на теге Qt, с которым собирается CI (`QT_VERSION` в `package.yml`), и страница https://doc.qt.io/qt-6/licenses-used-in-qt.html той же версии. Каждая запись несёт блок `Copyright:` — поле `Copyright` файла дословно (у LibJPEG-turbo его нет) — и текст `LicenseFile` без правок, лицензии `OR` — с названием выбранной.
- Входит то, что скомпилировано в модули пакетов: Core, Network, DBus, Gui, платформенные плагины, Qml, Quick, Quick Controls. Pixman (только 32-битный ARM NEON), входные данные генераторов (реестр Vulkan), Wayland, Windows, Android, WebAssembly, тесты и сборочные утилиты не входят.
- Как Qt CI собран (свой zlib, PCRE2, FreeType, libpng или системные), он не публикует; в бинарниках этого не проверить без доступа к скачанному Qt. Поэтому компонент, который мог оказаться как внутри Qt, так и системным, перечислен. На Linux HarfBuzz, PCRE2 (16-бит), double-conversion и md4c, по всей видимости, внутри Qt: это выведено из того, что linuxdeploy не копирует их системных копий в образ, а не проверено по бинарникам Qt. Для macOS это принято по умолчанию Qt и не проверено.
- `.app` — это всё, что копирует `macdeployqt` (все стили Quick Controls, Qt Svg, плагин `qjpeg`); AppImage `tools/package_linux.sh` чистит. Список по `.app` шире и закрывает оба. В `.app` (проверено по `.dmg` прогона CI) из плагинов изображений лежат только `qgif`, `qico`, `qjpeg`, `qsvg`: модуля Qt Image Formats нет, libtiff и libwebp не входят; у Qt Svg в qtsvg v6.12.0 файла атрибуции нет. Остальные `qt_attribution.json` qtbase и qtdeclarative (Wayland, Windows, Android, SQLite-драйвер, Qt Test, сборочные файлы) относятся к тому, чего в пакетах нет.

Рантайм AppImage (`AppImage/type2-runtime`, тег из `tools/package_linux.sh`, один на архитектуру) статически слинкован с musl, libfuse 3, squashfuse, zstd, zlib и mimalloc. Состав — `src/runtime/Makefile` и `scripts/` тега, версии подтверждаются строками бинарника с тем же sha256, что в `package_linux.sh`. Лицензия libfuse (LGPL 2.1) требует возможности пересобрать: раздел называет исходники, патч и способ заменить рантайм (`--appimage-extract`, `appimagetool --runtime-file`).

Обновление при смене версии Qt или тега рантайма:

1. Взять `qt_attribution.json` нового тега, сравнить набор `Id` с разделом, добавить новые компоненты и убрать пропавшие, поправить версии и копирайт-строки.
2. Для рантайма: `strings` бинарника нового тега и `src/runtime/Makefile` — какие библиотеки и версии внутри, прочитать `LICENSE` тега.

Новая зависимость — только по согласованию, с записью в эту таблицу и в `THIRD_PARTY_LICENSES`.
