# 10. Лицензии и зависимости

| Зависимость | Лицензия | Как подключена |
|---|---|---|
| Qt 6: Core, Gui, Qml, Quick, Quick Controls, Network | LGPLv3 | динамически, заменяемые библиотеки в `.app` и AppImage |
| Qt Test | LGPLv3 | только тесты |
| libghostty-vt | MIT | из исходников, статически |
| Zig | MIT | только сборка |

Обязательства LGPLv3: динамическая линковка, библиотеки Qt заменяемы, тексты LGPLv3 и GPLv3 и ссылка на исходники Qt лежат рядом. Статической линковки Qt нет.

Модули только под GPLv3 не подключаются: Qt Canvas Painter, Qt CoAP, Qt Graphs, Qt GRPC, Qt HTTP Server, Qt Lottie Animation, Qt MQTT, Qt Network Authorization, Qt Qml Compiler, Qt Quick 3D, Qt Quick 3D Physics, Qt Quick Timeline, Qt Virtual Keyboard, Qt Wayland Compositor (prior-art §5.2). Отсюда же отказ от AOT-компиляции QML в v1: что именно входит в Qt Qml Compiler, не проверено.

Новая зависимость — только по согласованию, с записью в эту таблицу и в `THIRD_PARTY_LICENSES`.
