#include "sessions/model/merge_patch.hpp"

namespace roost::sessions {

namespace {

void CollectPaths(const QJsonObject& patch, KeyPath& prefix,
                  QSet<KeyPath>& out) {
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        prefix.append(it.key());
        if (it.value().isObject() && !it.value().toObject().isEmpty()) {
            CollectPaths(it.value().toObject(), prefix, out);
        } else {
            out.insert(prefix);
        }
        prefix.removeLast();
    }
}

}  // namespace

void ApplyPatch(QJsonObject& target, const QJsonObject& patch) {
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        const QString key = it.key();
        const QJsonValue value = it.value();
        if (value.isNull()) {
            target.remove(key);
        } else if (value.isObject()) {
            QJsonObject merged = target.value(key).toObject();
            ApplyPatch(merged, value.toObject());
            target.insert(key, merged);
        } else {
            target.insert(key, value);
        }
    }
}

QJsonObject DiffOf(const QJsonObject& before, const QJsonObject& after) {
    QJsonObject patch;
    for (auto it = before.begin(); it != before.end(); ++it) {
        if (!after.contains(it.key())) {
            patch.insert(it.key(), QJsonValue::Null);
        }
    }
    for (auto it = after.begin(); it != after.end(); ++it) {
        const QJsonValue old = before.value(it.key());
        if (it.value().isNull() || old == it.value()) {
            continue;
        }
        if (old.isObject() && it.value().isObject()) {
            const QJsonObject inner =
                DiffOf(old.toObject(), it.value().toObject());
            if (!inner.isEmpty()) {
                patch.insert(it.key(), inner);
            }
        } else {
            patch.insert(it.key(), it.value());
        }
    }
    return patch;
}

QSet<KeyPath> PatchPaths(const QJsonObject& patch) {
    QSet<KeyPath> out;
    KeyPath prefix;
    CollectPaths(patch, prefix, out);
    return out;
}

QJsonValue ValueAt(const QJsonObject& document, const KeyPath& path) {
    QJsonValue value = document;
    for (const QString& key : path) {
        if (!value.isObject()) {
            return QJsonValue::Undefined;
        }
        value = value.toObject().value(key);
    }
    return value;
}

void SetAt(QJsonObject& document, const KeyPath& path,
           const QJsonValue& value) {
    if (path.isEmpty()) {
        return;
    }
    const QString& key = path.first();
    if (path.size() == 1) {
        if (value.isUndefined()) {
            document.remove(key);
        } else if (!(value.isObject() && value.toObject().isEmpty() &&
                     document.value(key).isObject())) {
            document.insert(key, value);
        }
        return;
    }
    QJsonObject inner = document.value(key).toObject();
    SetAt(inner, path.mid(1), value);
    // Removing the last member of an object that was there leaves it empty.
    if (value.isUndefined() && !document.value(key).isObject()) {
        return;
    }
    document.insert(key, inner);
}

}  // namespace roost::sessions
