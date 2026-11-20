#include "qml_api/theme.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtLogging>

#include <algorithm>
#include <array>

namespace roost {

namespace {

constexpr auto DEFAULT_THEME = ":/qt/qml/Roost/Api/themes/default.json";
constexpr int CATEGORY_COUNT = 8;

struct Token {
    const char* Key;
    QColor Theme::* Member;
};

}  // namespace

Theme::Theme(QObject* parent) : QObject(parent) {
    QFile file(QString::fromLatin1(DEFAULT_THEME));
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning("roost: the default theme is missing: %s", DEFAULT_THEME);
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject() || !load(document.object())) {
        qWarning("roost: the default theme is not valid: %s", DEFAULT_THEME);
    }
}

bool Theme::load(const QJsonObject& tokens) {
    static const std::array TOKENS = {
        Token{"windowBackground", &Theme::WindowBackground_},
        Token{"panelBackground", &Theme::PanelBackground_},
        Token{"overlayBackground", &Theme::OverlayBackground_},
        Token{"text", &Theme::Text_},
        Token{"textStrong", &Theme::TextStrong_},
        Token{"textSecondary", &Theme::TextSecondary_},
        Token{"textMuted", &Theme::TextMuted_},
        Token{"accent", &Theme::Accent_},
        Token{"accentTint", &Theme::AccentTint_},
        Token{"selection", &Theme::Selection_},
        Token{"cursorFocused", &Theme::CursorFocused_},
        Token{"cursorUnfocused", &Theme::CursorUnfocused_},
        Token{"stateWorking", &Theme::StateWorking_},
        Token{"stateBlocked", &Theme::StateBlocked_},
        Token{"stateFailed", &Theme::StateFailed_},
        Token{"bannerBackground", &Theme::BannerBackground_},
        Token{"bannerRefusedBackground", &Theme::BannerRefusedBackground_},
        Token{"bannerText", &Theme::BannerText_},
        Token{"problemsBackground", &Theme::ProblemsBackground_},
        Token{"problemsText", &Theme::ProblemsText_},
        Token{"warning", &Theme::Warning_},
        Token{"error", &Theme::Error_},
        Token{"windowError", &Theme::WindowError_},
    };

    bool taken = true;
    bool differs = false;
    const auto colorOf = [](const QJsonValue& value) {
        return value.isString() ? QColor::fromString(value.toString())
                                : QColor();
    };

    for (auto it = tokens.begin(); it != tokens.end(); ++it) {
        const QString key = it.key();
        if (key == QLatin1String("categories")) {
            QVariantList list;
            const QJsonArray array = it.value().toArray();
            for (const QJsonValue& value : array) {
                list.append(colorOf(value));
            }
            const bool valid = list.size() == CATEGORY_COUNT &&
                               std::ranges::all_of(list, [](const QVariant& v) {
                                   return v.value<QColor>().isValid();
                               });
            if (!valid) {
                qWarning("roost: theme: categories need %d colours; ignored",
                         CATEGORY_COUNT);
                taken = false;
            } else if (list != Categories_) {
                Categories_ = list;
                differs = true;
            }
            continue;
        }
        const auto* const token = std::ranges::find_if(
            TOKENS,
            [&key](const Token& t) { return key == QLatin1String(t.Key); });
        if (token == TOKENS.end()) {
            qWarning("roost: theme: unknown token %s; ignored",
                     qUtf8Printable(key));
            taken = false;
            continue;
        }
        const QColor color = colorOf(it.value());
        if (!color.isValid()) {
            qWarning("roost: theme: %s is not a colour; ignored",
                     qUtf8Printable(key));
            taken = false;
        } else if (this->*(token->Member) != color) {
            this->*(token->Member) = color;
            differs = true;
        }
    }
    if (differs) {
        emit changed();
    }
    return taken;
}

}  // namespace roost
