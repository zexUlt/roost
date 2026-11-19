# 10. Лицензии и зависимости

| Зависимость | Лицензия | Как подключена |
|---|---|---|
| Qt 6: Core, Gui, Qml, Quick, Quick Controls, Network | LGPLv3 | динамически, заменяемые библиотеки в `.app` и AppImage |
| Qt Test | LGPLv3 | только тесты |
| libghostty-vt | MIT | из исходников, статически |
| Zig | MIT | только сборка |
| ICU 73 (из Qt сборки) | ICU (MIT-подобная) | только AppImage, динамически, рядом с Qt; текст лицензии — `licenses/ICU-LICENSE.txt` (`icu4c/LICENSE` тега `release-73-2`), в образе `usr/share/licenses/roost/ICU-LICENSE.txt` |
| Библиотеки xcb в AppImage: xcb-util (cursor, icccm, image, keysyms, render-util, util), libxcb-shape, libxcb-xkb, libxkbcommon-x11 | MIT / X11 | только AppImage: `linuxdeploy` копирует с машины сборки то, чего может не быть на десктопе, и `copyright` пакета; динамически, заменяемые. Библиотеки, которые есть на любом десктопе (GLib, D-Bus, systemd, сжатие, Kerberos, libpng, X11), в образ не кладутся: список в `packaging/linux/system-libraries.txt` |

Обязательства LGPLv3: динамическая линковка, библиотеки Qt заменяемы, тексты LGPLv3 и GPLv3 и ссылка на исходники Qt лежат рядом. Статической линковки Qt нет.

Модули только под GPLv3 не подключаются: Qt Canvas Painter, Qt CoAP, Qt Graphs, Qt GRPC, Qt HTTP Server, Qt Lottie Animation, Qt MQTT, Qt Network Authorization, Qt Qml Compiler, Qt Quick 3D, Qt Quick 3D Physics, Qt Quick Timeline, Qt Virtual Keyboard, Qt Wayland Compositor (prior-art §5.2). Отсюда же отказ от AOT-компиляции QML в v1: что именно входит в Qt Qml Compiler, не проверено.

Новая зависимость — только по согласованию, с записью в эту таблицу и в `THIRD_PARTY_LICENSES`.
