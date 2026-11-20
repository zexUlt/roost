// The merge patch of the sessions list (plugins/sessions/model/merge_patch.hpp)
// against the one of the store (src/core/store.hpp): the plugin keeps its own
// copy, because it does not link roost core, so the two must agree.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include "core/store.hpp"
#include "sessions/model/merge_patch.hpp"

using namespace roost::sessions;

namespace {

QJsonObject ObjectOf(const char* json) {
    return QJsonDocument::fromJson(json).object();
}

}  // namespace

class SessionsMergePatchTest : public QObject {
    Q_OBJECT

private slots:
    void ApplyAgreesWithTheStore_data();
    void ApplyAgreesWithTheStore();
    void DiffAgreesWithTheStoreAndAppliesBack_data();
    void DiffAgreesWithTheStoreAndAppliesBack();
    void PathsAreTheLeavesOfAPatch();
    void ValueAtAndSetAtFollowAPath();
};

void SessionsMergePatchTest::ApplyAgreesWithTheStore_data() {
    QTest::addColumn<QByteArray>("target");
    QTest::addColumn<QByteArray>("patch");
    const auto row = [](const char* name, const char* target,
                        const char* patch) {
        QTest::newRow(name) << QByteArray(target) << QByteArray(patch);
    };
    row("replace", R"({"a":1})", R"({"a":2})");
    row("add", R"({"a":1})", R"({"b":2})");
    row("null removes", R"({"a":1,"b":2})", R"({"a":null})");
    row("null of a missing key", R"({"a":1})", R"({"b":null})");
    row("objects merge", R"({"a":{"b":1,"c":2}})", R"({"a":{"c":3,"d":4}})");
    row("an array is replaced", R"({"a":[1,2,3]})", R"({"a":[9]})");
    row("an object into a scalar", R"({"a":1})", R"({"a":{"b":2}})");
    row("an object with nulls into nothing", R"({})",
        R"({"a":{"b":null,"c":1}})");
    row("an empty object creates one", R"({})", R"({"a":{}})");
    row("an empty object keeps one", R"({"a":{"b":1}})", R"({"a":{}})");
}

void SessionsMergePatchTest::ApplyAgreesWithTheStore() {
    QFETCH(QByteArray, target);
    QFETCH(QByteArray, patch);
    QJsonObject ours = ObjectOf(target);
    QJsonObject theirs = ours;
    ApplyPatch(ours, ObjectOf(patch));
    roost::ApplyMergePatch(theirs, ObjectOf(patch));
    QCOMPARE(ours, theirs);
}

void SessionsMergePatchTest::DiffAgreesWithTheStoreAndAppliesBack_data() {
    QTest::addColumn<QByteArray>("before");
    QTest::addColumn<QByteArray>("after");
    const auto row = [](const char* name, const char* before,
                        const char* after) {
        QTest::newRow(name) << QByteArray(before) << QByteArray(after);
    };
    row("equal", R"({"a":1})", R"({"a":1})");
    row("from empty", R"({})", R"({"a":1,"b":{"c":[1,2]}})");
    row("to empty", R"({"a":1,"b":{"c":2}})", R"({})");
    row("a key removed", R"({"a":1,"b":2})", R"({"a":1})");
    row("a nested key changed", R"({"a":{"b":1,"c":2},"d":3})",
        R"({"a":{"b":1,"c":5},"d":3})");
    row("an array changed", R"({"a":[1,2,3]})", R"({"a":[1,2]})");
    row("an object becomes a scalar", R"({"a":{"b":1}})", R"({"a":1})");
    row("a scalar becomes an empty object", R"({"a":1})", R"({"a":{}})");
    row("false and 0", R"({"a":true,"b":1})", R"({"a":false,"b":0})");
}

void SessionsMergePatchTest::DiffAgreesWithTheStoreAndAppliesBack() {
    QFETCH(QByteArray, before);
    QFETCH(QByteArray, after);
    const QJsonObject a = ObjectOf(before);
    const QJsonObject b = ObjectOf(after);
    const QJsonObject diff = DiffOf(a, b);
    QCOMPARE(diff, roost::MergeDiff(a, b));
    QJsonObject applied = a;
    ApplyPatch(applied, diff);
    QCOMPARE(applied, b);
}

void SessionsMergePatchTest::PathsAreTheLeavesOfAPatch() {
    const QSet<KeyPath> paths = PatchPaths(
        ObjectOf(R"({"a":1,"b":{"c":[1],"d":{"e":null}},"f":{},"g":false})"));
    QCOMPARE(paths,
             (QSet<KeyPath>{{"a"}, {"b", "c"}, {"b", "d", "e"}, {"f"}, {"g"}}));
    QVERIFY(PatchPaths({}).isEmpty());
}

void SessionsMergePatchTest::ValueAtAndSetAtFollowAPath() {
    QJsonObject document = ObjectOf(R"({"a":{"b":1},"c":2})");
    QCOMPARE(ValueAt(document, {"a", "b"}), QJsonValue(1));
    QVERIFY(ValueAt(document, {"a", "x"}).isUndefined());
    QVERIFY(ValueAt(document, {"c", "x"}).isUndefined());

    SetAt(document, {"a", "b"}, 5);
    SetAt(document, {"d", "e", "f"}, QStringLiteral("deep"));
    SetAt(document, {"c"}, QJsonValue::Undefined);
    SetAt(document, {"x", "y"}, QJsonValue::Undefined);
    QCOMPARE(document, ObjectOf(R"({"a":{"b":5},"d":{"e":{"f":"deep"}}})"));

    // An empty object does not wipe the one that is there.
    SetAt(document, {"a"}, QJsonObject());
    QCOMPARE(ValueAt(document, {"a", "b"}), QJsonValue(5));
    SetAt(document, {"n"}, QJsonObject());
    QVERIFY(ValueAt(document, {"n"}).isObject());
}

QTEST_GUILESS_MAIN(SessionsMergePatchTest)
#include "sessions_merge_patch_test.moc"
