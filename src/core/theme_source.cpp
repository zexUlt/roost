#include "core/theme_source.hpp"

#include <QFile>
#include <QJsonDocument>

#include <optional>

namespace roost {

namespace {

constexpr auto DEFAULT_THEME_FILE = ":/roost/themes/default.json";
const QString TEST_THEME_ID = QStringLiteral("test");

std::optional<QJsonObject> ReadTokens(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}

}  // namespace

ResolvedTheme ResolveTheme(const QJsonObject& settings) {
    ResolvedTheme resolved{.Id = DEFAULT_THEME_ID, .Tokens = {}, .Problem = {}};
    const auto defaults = ReadTokens(QString::fromLatin1(DEFAULT_THEME_FILE));
    if (!defaults) {
        resolved.Problem =
            QStringLiteral("the default theme is missing or not valid");
        return resolved;
    }
    resolved.Tokens = *defaults;

    const QJsonValue wanted = settings.value(QStringLiteral("theme"));
    if (wanted.isUndefined() || wanted.toString() == DEFAULT_THEME_ID) {
        return resolved;
    }
    std::optional<QJsonObject> tokens;
    if (wanted.isString() && wanted.toString() == TEST_THEME_ID &&
        qEnvironmentVariableIsSet("ROOST_TEST_THEME_FILE")) {
        tokens = ReadTokens(qEnvironmentVariable("ROOST_TEST_THEME_FILE"));
    }
    if (!tokens) {
        resolved.Problem =
            wanted.isString()
                ? QStringLiteral("unknown theme \"%1\"; the default is used")
                      .arg(wanted.toString())
                : QStringLiteral(
                      "settings.theme is not a string; the default is used");
        return resolved;
    }
    for (auto it = tokens->begin(); it != tokens->end(); ++it) {
        resolved.Tokens.insert(it.key(), it.value());
    }
    resolved.Id = wanted.toString();
    return resolved;
}

ResolvedTerminalColors ResolveTerminalColors(const QJsonObject& tokens) {
    auto colors = ParseTerminalColors(tokens);
    if (colors) {
        return {.Colors = *colors, .Problem = {}, .Fallback = false};
    }
    const QString problem = colors.error();
    auto fallback = ParseTerminalColors(ResolveTheme({}).Tokens);
    if (!fallback) {
        return {.Colors = std::nullopt,
                .Problem = fallback.error(),
                .Fallback = false};
    }
    return {.Colors = *fallback, .Problem = problem, .Fallback = true};
}

}  // namespace roost
