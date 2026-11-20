// The layout model: the default tree, the round trip and the repair of broken
// documents. Pure data: no plugins, no window; the panel types are a table.

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

#include "roost/layout_model.hpp"

using namespace roost;

namespace {

LayoutKnownPanels Known() {
    return {
        {"sessions.list",
         {.Inputs = {}, .Outputs = {{"selected", "SessionRef"}}}},
        {"terminal.view",
         {.Inputs = {{"session", "SessionRef"}}, .Outputs = {}}},
        {"counter.view",
         {.Inputs = {{"count", "Number"}}, .Outputs = {{"total", "Number"}}}},
    };
}

QJsonValue Json(const char* text) {
    QJsonParseError error{};
    const QJsonDocument document =
        QJsonDocument::fromJson(QByteArray(text), &error);
    if (error.error != QJsonParseError::NoError) {
        qFatal("bad JSON in the test: %s", qPrintable(error.errorString()));
    }
    return document.isObject() ? QJsonValue(document.object())
                               : QJsonValue(document.array());
}

bool AnyProblemHas(const LayoutParseResult& result, const QString& part) {
    for (const QString& problem : result.Problems) {
        if (problem.contains(part)) {
            return true;
        }
    }
    return false;
}

// The leaves of a tree in document order.
void Leaves(const LayoutNode& node, std::vector<const LayoutNode*>& out) {
    if (node.Kind == ELayoutNodeKind::Leaf) {
        out.push_back(&node);
        return;
    }
    for (const LayoutNode& child : node.Children) {
        Leaves(child, out);
    }
}

std::vector<const LayoutNode*> Leaves(const Layout& layout) {
    std::vector<const LayoutNode*> out;
    Leaves(layout.Root, out);
    return out;
}

constexpr auto DEFAULT_DOCUMENT = R"json({
  "version": 1,
  "root": {
    "split": "h",
    "children": [
      { "instance": "list", "panel": "sessions.list", "links": {} },
      { "instance": "terminal", "panel": "terminal.view",
        "links": { "session": { "from": "list", "output": "selected" } } }
    ],
    "sizes": [320, "fill"]
  }
})json";

}  // namespace

class LayoutModelTest : public QObject {
    Q_OBJECT

private slots:
    void DefaultIsTheDocumentedTree() {
        QCOMPARE(LayoutToJson(DefaultLayout()),
                 Json(DEFAULT_DOCUMENT).toObject());
        const LayoutParseResult result =
            ParseLayout(Json(DEFAULT_DOCUMENT), Known());
        QVERIFY2(result.Problems.isEmpty(),
                 qPrintable(result.Problems.join('\n')));
        QVERIFY(result.Writable);
        QCOMPARE(result.Tree, DefaultLayout());
    }

    void DefaultHasProblemsOnlyWhenPanelsAreMissing() {
        const LayoutParseResult result =
            ParseLayout(LayoutToJson(DefaultLayout()), {});
        QVERIFY(AnyProblemHas(result, "no panel sessions.list"));
        QVERIFY(AnyProblemHas(result, "no panel terminal.view"));
        // Stubs and switched-off links do not change what is written.
        QCOMPARE(LayoutToJson(result.Tree), LayoutToJson(DefaultLayout()));
    }

    void RoundTrip_data() {
        QTest::addColumn<QByteArray>("document");
        QTest::newRow("default") << QByteArray(DEFAULT_DOCUMENT);
        QTest::newRow("nested") << QByteArray(R"json({
          "version": 1,
          "root": { "split": "v", "sizes": ["fill", 200], "children": [
            { "tabs": [
                { "instance": "a", "panel": "sessions.list", "links": {} },
                { "split": "h", "sizes": [100, 250, "fill"], "children": [
                    { "instance": "b", "panel": "sessions.list", "links": {} },
                    { "instance": "c", "panel": "terminal.view",
                      "links": { "session": { "from": "b", "output": "selected" } } },
                    { "instance": "d", "panel": "terminal.view",
                      "links": { "session": { "from": "a", "output": "selected" } } }
                ] }
              ], "active": 1 },
            { "instance": "e", "panel": "counter.view", "links": {} }
          ] } })json");
        // A stub and a link that is off are kept as written.
        QTest::newRow("kept as written") << QByteArray(R"json({
          "version": 1,
          "root": { "split": "h", "sizes": ["fill", "fill"], "children": [
            { "instance": "x", "panel": "no.such", "links": {} },
            { "instance": "t", "panel": "terminal.view",
              "links": { "session": { "from": "gone", "output": "selected" } } }
          ] } })json");
    }

    void RoundTrip() {
        QFETCH(QByteArray, document);
        const QJsonValue input = Json(document.constData());
        const LayoutParseResult first = ParseLayout(input, Known());
        QCOMPARE(LayoutToJson(first.Tree), input.toObject());
        const LayoutParseResult second =
            ParseLayout(LayoutToJson(first.Tree), Known());
        QCOMPARE(second.Tree, first.Tree);
        QCOMPARE(second.Problems, first.Problems);
    }

    // Every broken document gives a problem naming the place and a tree that
    // can be shown.
    void BrokenDocument_data() {
        QTest::addColumn<QByteArray>("document");
        QTest::addColumn<QString>("problem");
        QTest::addColumn<int>("leaves");
        QTest::addColumn<QString>("check");

        const char* const two = R"json({ "version": 1, "root": { "split": "h",
            "sizes": [100, "fill"], "children": [
              { "instance": "list", "panel": "sessions.list" },
              { "instance": "terminal", "panel": "terminal.view",
                "links": { "session": { "from": "list", "output": "selected" } } } ] } })json";
        QTest::newRow("sane")
            << QByteArray(two) << QString() << 2 << "link:terminal:ok";

        QTest::newRow("unknown panel")
            << QByteArray(R"json({ "root": {
            "instance": "x", "panel": "no.such" } })json")
            << "root: no panel no.such" << 1 << "stub:x:no panel no.such";
        QTest::newRow("no panel type") << QByteArray(R"json({ "root": {
            "instance": "x" } })json") << "root: no panel type"
                                       << 1 << "stub:x:no panel type";
        QTest::newRow("link to a missing instance")
            << QByteArray(R"json({ "root": {
            "instance": "t", "panel": "terminal.view",
            "links": { "session": { "from": "gone", "output": "selected" } } } })json")
            << "there is no instance gone" << 1 << "link:t:off";
        QTest::newRow("link to a missing output")
            << QByteArray(R"json({ "root": { "split": "h",
            "sizes": ["fill", "fill"], "children": [
            { "instance": "l", "panel": "sessions.list" },
            { "instance": "t", "panel": "terminal.view",
              "links": { "session": { "from": "l", "output": "nope" } } } ] } })json")
            << "sessions.list has no output nope" << 2 << "link:t:off";
        QTest::newRow("link to a missing input")
            << QByteArray(R"json({ "root": { "split": "h",
            "sizes": ["fill", "fill"], "children": [
            { "instance": "l", "panel": "sessions.list" },
            { "instance": "t", "panel": "terminal.view",
              "links": { "wrong": { "from": "l", "output": "selected" } } } ] } })json")
            << "terminal.view has no input wrong" << 2 << "link:t:off";
        QTest::newRow("mismatched port types")
            << QByteArray(R"json({ "root": { "split": "h",
            "sizes": ["fill", "fill"], "children": [
            { "instance": "l", "panel": "sessions.list" },
            { "instance": "c", "panel": "counter.view",
              "links": { "count": { "from": "l", "output": "selected" } } } ] } })json")
            << "the output selected is SessionRef, the input count is Number"
            << 2 << "link:c:off";
        QTest::newRow("link from a stub")
            << QByteArray(R"json({ "root": { "split": "h",
            "sizes": ["fill", "fill"], "children": [
            { "instance": "l", "panel": "no.such" },
            { "instance": "t", "panel": "terminal.view",
              "links": { "session": { "from": "l", "output": "selected" } } } ] } })json")
            << "the panel no.such of l is unknown" << 2 << "link:t:off";
        QTest::newRow("link to itself")
            << QByteArray(R"json({ "root": {
            "instance": "c", "panel": "counter.view",
            "links": { "count": { "from": "c", "output": "total" } } } })json")
            << "cannot feed itself" << 1 << "link:c:off";
        QTest::newRow("link forward")
            << QByteArray(R"json({ "root": { "split": "h",
            "sizes": ["fill", "fill"], "children": [
            { "instance": "t", "panel": "terminal.view",
              "links": { "session": { "from": "l", "output": "selected" } } },
            { "instance": "l", "panel": "sessions.list" } ] } })json")
            << QString() << 2 << "link:t:ok";
        QTest::newRow("link without from") << QByteArray(R"json({ "root": {
            "instance": "t", "panel": "terminal.view",
            "links": { "session": { "output": "selected" } } } })json")
                                           << "a link needs" << 1 << "nolink:t";
        QTest::newRow("duplicate instance")
            << QByteArray(R"json({ "root": { "split": "h",
            "sizes": ["fill", "fill"], "children": [
            { "instance": "p", "panel": "sessions.list" },
            { "instance": "p", "panel": "terminal.view",
              "links": { "session": { "from": "p", "output": "selected" } } } ] } })json")
            << "instance \"p\" is used twice; this one is \"p-2\"" << 2
            << "link:p-2:ok";
        QTest::newRow("duplicate keeps an explicit suffixed id")
            << QByteArray(R"json({ "root": { "split": "h",
            "sizes": ["fill", "fill", "fill", "fill"], "children": [
            { "instance": "a", "panel": "sessions.list" },
            { "instance": "a", "panel": "terminal.view" },
            { "instance": "a-2", "panel": "sessions.list" },
            { "instance": "t", "panel": "terminal.view",
              "links": { "session": { "from": "a-2", "output": "selected" } } } ] } })json")
            << "instance \"a\" is used twice; this one is \"a-3\"" << 4
            << "link:t:ok";
        QTest::newRow("no instance keeps a later explicit id")
            << QByteArray(R"json({ "root": { "split": "h",
            "sizes": ["fill", "fill", "fill"], "children": [
            { "panel": "terminal.view" },
            { "instance": "terminal.view", "panel": "sessions.list" },
            { "instance": "t", "panel": "terminal.view",
              "links": { "session": { "from": "terminal.view", "output": "selected" } } } ] } })json")
            << "no instance id; \"terminal.view-2\" is used" << 3
            << "link:t:ok";
        QTest::newRow("no instance")
            << QByteArray(R"json({ "root": {
            "panel": "sessions.list" } })json")
            << "no instance id; \"sessions.list\" is used" << 1
            << "instance:sessions.list";
        QTest::newRow("bad split direction")
            << QByteArray(R"json({ "root": {
            "split": "x", "children": [ { "instance": "a", "panel": "sessions.list" } ] } })json")
            << "\"split\" is \"h\" or \"v\"" << 1 << QString();
        QTest::newRow("split without children")
            << QByteArray(R"json({ "root": { "split": "h",
            "sizes": [], "children": [] } })json")
            << "a split without children is dropped" << 2 << "default";
        QTest::newRow("node that is nothing")
            << QByteArray(R"json({ "root": { "split": "h",
            "sizes": [100, 200, "fill"], "children": [
            { "instance": "a", "panel": "sessions.list" }, 5,
            { "instance": "b", "panel": "terminal.view" } ] } })json")
            << "root.children[1]: a node is an object" << 2 << "sizes:100,fill";
        QTest::newRow("bad size")
            << QByteArray(R"json({ "root": { "split": "h",
            "sizes": [-5, "wide"], "children": [
            { "instance": "a", "panel": "sessions.list" },
            { "instance": "b", "panel": "terminal.view" } ] } })json")
            << "root.sizes[0]: a size is" << 2 << "sizes:fill,fill";
        QTest::newRow("sizes missing")
            << QByteArray(R"json({ "root": { "split": "h",
            "children": [ { "instance": "a", "panel": "sessions.list" },
            { "instance": "b", "panel": "terminal.view" } ] } })json")
            << QString() << 2 << "sizes:fill,fill";
        QTest::newRow("sizes too short")
            << QByteArray(R"json({ "root": { "split": "h",
            "sizes": [100], "children": [ { "instance": "a", "panel": "sessions.list" },
            { "instance": "b", "panel": "terminal.view" } ] } })json")
            << "root.sizes: has 1 entries for 2 children" << 2
            << "sizes:100,fill";
        QTest::newRow("sizes not an array")
            << QByteArray(R"json({ "root": { "split": "h",
            "sizes": 3, "children": [ { "instance": "a", "panel": "sessions.list" } ] } })json")
            << "root.sizes: must be an array" << 1 << "sizes:fill";
        QTest::newRow("active out of range")
            << QByteArray(R"json({ "root": {
            "tabs": [ { "instance": "a", "panel": "sessions.list" } ], "active": 4 } })json")
            << "root.active: is not the index of a tab" << 1 << "active:0";
        QTest::newRow("active follows a dropped tab")
            << QByteArray(R"json({ "root": {
            "tabs": [ 7, { "instance": "a", "panel": "sessions.list" },
                      { "instance": "b", "panel": "terminal.view" } ], "active": 2 } })json")
            << "root.tabs[0]" << 2 << "active:1";
        QTest::newRow("tabs not an array")
            << QByteArray(R"json({ "root": { "split": "h",
            "sizes": ["fill", "fill"], "children": [
            { "tabs": 1 }, { "instance": "a", "panel": "sessions.list" } ] } })json")
            << "root.children[0].tabs: must be an array" << 1 << "sizes:fill";
        QTest::newRow("links not an object")
            << QByteArray(R"json({ "root": {
            "instance": "t", "panel": "terminal.view", "links": [] } })json")
            << "root.links: must be an object" << 1 << "nolink:t";
        QTest::newRow("root missing")
            << QByteArray(R"json({ "version": 1 })json") << "no usable root"
            << 2 << "default";
        QTest::newRow("root unusable")
            << QByteArray(R"json({ "root": { "hello": 1 } })json")
            << "neither a split" << 2 << "default";
    }

    void BrokenDocument() {
        QFETCH(QByteArray, document);
        QFETCH(QString, problem);
        QFETCH(int, leaves);
        QFETCH(QString, check);

        const LayoutParseResult result =
            ParseLayout(Json(document.constData()), Known());
        if (problem.isEmpty()) {
            QVERIFY2(result.Problems.isEmpty(),
                     qPrintable(result.Problems.join('\n')));
        } else {
            QVERIFY2(AnyProblemHas(result, problem),
                     qPrintable(result.Problems.join('\n')));
        }
        QVERIFY(result.Writable);
        const std::vector<const LayoutNode*> found = Leaves(result.Tree);
        QCOMPARE(static_cast<int>(found.size()), leaves);

        // The tree can be written and read again without a problem the first
        // read did not have: what was repaired stays repaired.
        const LayoutParseResult again =
            ParseLayout(LayoutToJson(result.Tree), Known());
        QCOMPARE(again.Tree, result.Tree);

        const QStringList parts = check.split(':');
        if (check.isEmpty()) {
            return;
        }
        if (check == "default") {
            QCOMPARE(result.Tree, DefaultLayout());
            return;
        }
        if (parts[0] == "sizes" || parts[0] == "active") {
            const LayoutNode* node = &result.Tree.Root;
            while (parts[0] == "sizes" &&
                   node->Kind != ELayoutNodeKind::Split &&
                   !node->Children.empty()) {
                node = &node->Children.front();
            }
            if (parts[0] == "active") {
                QCOMPARE(node->Active, parts[1].toInt());
            } else {
                QStringList sizes;
                for (const LayoutSize& size : node->Sizes) {
                    sizes.append(size.Fill ? "fill"
                                           : QString::number(size.Pixels));
                }
                QCOMPARE(sizes.join(','), parts[1]);
            }
            return;
        }
        if (parts[0] == "instance") {
            QCOMPARE(found.front()->Instance, parts[1]);
            return;
        }
        const LayoutNode* leaf = nullptr;
        for (const LayoutNode* candidate : found) {
            if (candidate->Instance == parts[1]) {
                leaf = candidate;
            }
        }
        if (leaf == nullptr) {
            QFAIL(qPrintable(check));
        }
        if (parts[0] == "stub") {
            QCOMPARE(leaf->Error, parts[2]);
        } else if (parts[0] == "nolink") {
            QVERIFY(leaf->Links.isEmpty());
        } else {
            QCOMPARE(leaf->Links.size(), 1);
            QCOMPARE(leaf->Links.begin()->Problem.isEmpty(), parts[2] == "ok");
        }
    }

    void UnknownVersion_data() {
        QTest::addColumn<QByteArray>("version");
        QTest::newRow("2") << QByteArray("2");
        QTest::newRow("2.5") << QByteArray("2.5");
        QTest::newRow("1.5") << QByteArray("1.5");
        QTest::newRow("1e30") << QByteArray("1e30");
        QTest::newRow("-5") << QByteArray("-5");
        QTest::newRow("string") << QByteArray("\"1\"");
        QTest::newRow("null") << QByteArray("null");
    }

    void UnknownVersion() {
        QFETCH(QByteArray, version);
        const QByteArray text = "{\"version\": " + version +
                                ", \"root\": {\"instance\": \"x\", \"panel\": "
                                "\"sessions.list\"}}";
        const LayoutParseResult result =
            ParseLayout(Json(text.constData()), Known());
        QVERIFY(!result.Writable);
        QCOMPARE(result.Tree, DefaultLayout());
        QVERIFY(AnyProblemHas(result, "version this roost does not know"));
    }

    void NoVersionIsRead() {
        const LayoutParseResult result = ParseLayout(
            Json(
                R"json({ "root": { "instance": "x", "panel": "sessions.list" } })json"),
            Known());
        QVERIFY(result.Writable);
        QVERIFY(result.Problems.isEmpty());
        QCOMPARE(Leaves(result.Tree).front()->Instance, QString("x"));
    }

    void NotAnObjectGivesTheDefaultTree() {
        const LayoutParseResult result = ParseLayout(Json("[1]"), Known());
        QCOMPARE(result.Tree, DefaultLayout());
        QVERIFY(AnyProblemHas(result, "not an object"));
        QVERIFY(result.Writable);
    }

    void DefaultForThePanelsThereAreIsTheDefaultTree() {
        const DefaultLayoutResult result = DefaultLayoutFor(
            {"counter.view", "sessions.list", "terminal.view"}, Known());
        QCOMPARE(result.Tree, DefaultLayout());
        QVERIFY(result.Problems.isEmpty());
    }

    void DefaultTakesTheListAndTheTerminalByTheirPorts() {
        LayoutKnownPanels known = Known();
        known.remove("sessions.list");
        known.remove("terminal.view");
        known.insert("alt.list", {.Inputs = {{"filter", "String"}},
                                  .Outputs = {{"selected", "SessionRef"}}});
        known.insert("alt.screen",
                     {.Inputs = {{"open", "SessionRef"}}, .Outputs = {}});
        const DefaultLayoutResult result =
            DefaultLayoutFor({"counter.view", "alt.list", "alt.screen"}, known);
        QVERIFY(result.Problems.isEmpty());
        const LayoutNode& root = result.Tree.Root;
        QCOMPARE(root.Children[0].Instance, QString("list"));
        QCOMPARE(root.Children[0].Panel, QString("alt.list"));
        QCOMPARE(root.Children[1].Instance, QString("terminal"));
        QCOMPARE(root.Children[1].Panel, QString("alt.screen"));
        QCOMPARE(root.Children[1].Links.keys(), QStringList{"open"});
        QCOMPARE(root.Children[1].Links["open"].From, QString("list"));
        QCOMPARE(root.Children[1].Links["open"].Output, QString("selected"));
        QCOMPARE(root.Sizes, (std::vector<LayoutSize>{LayoutSize::Px(320),
                                                      LayoutSize::Rest()}));
        // The tree is a good one for these panels.
        const LayoutParseResult parsed =
            ParseLayout(LayoutToJson(result.Tree), known);
        QVERIFY2(parsed.Problems.isEmpty(),
                 qPrintable(parsed.Problems.join('\n')));
    }

    void DefaultWithoutAListOrATerminalKeepsTheBuiltInTypes() {
        const DefaultLayoutResult result =
            DefaultLayoutFor({"counter.view"}, {{"counter.view", {}}});
        QCOMPARE(result.Tree, DefaultLayout());
        QVERIFY(result.Problems.isEmpty());
    }

    void DefaultWithSeveralListsTakesTheFirstInLoadOrderAndNamesTheOthers() {
        LayoutKnownPanels known = Known();
        known.insert("a.list",
                     {.Inputs = {}, .Outputs = {{"selected", "SessionRef"}}});
        known.insert("z.list",
                     {.Inputs = {}, .Outputs = {{"selected", "SessionRef"}}});
        // Load order, not the order of the table.
        const DefaultLayoutResult result = DefaultLayoutFor(
            {"z.list", "sessions.list", "a.list", "terminal.view"}, known);
        QCOMPARE(result.Tree.Root.Children[0].Panel, QString("z.list"));
        QCOMPARE(result.Problems.size(), 1);
        QVERIFY(result.Problems.first().contains("z.list is used"));
        QVERIFY(result.Problems.first().contains("sessions.list, a.list"));
        QVERIFY(result.Problems.first().contains("plugins.disabled"));
    }

    void DefaultListNeedsTheOutputSelectedOfTypeSessionRef() {
        LayoutKnownPanels known = Known();
        known.remove("sessions.list");
        known.insert("a.list",
                     {.Inputs = {}, .Outputs = {{"picked", "SessionRef"}}});
        known.insert("b.list",
                     {.Inputs = {}, .Outputs = {{"selected", "String"}}});
        const DefaultLayoutResult result =
            DefaultLayoutFor({"a.list", "b.list", "terminal.view"}, known);
        QCOMPARE(result.Tree, DefaultLayout());
        QVERIFY(result.Problems.isEmpty());
    }

    void DefaultTerminalHasExactlyOneSessionRefInput() {
        // A panel with two inputs of the type is not told from the others by
        // its type; it is passed over and the next panel is the terminal.
        LayoutKnownPanels known = Known();
        known.insert("two.view",
                     {.Inputs = {{"a", "SessionRef"}, {"b", "SessionRef"}},
                      .Outputs = {}});
        const DefaultLayoutResult result = DefaultLayoutFor(
            {"sessions.list", "two.view", "terminal.view"}, known);
        QCOMPARE(result.Tree, DefaultLayout());
        QVERIFY(result.Problems.isEmpty());
    }

    void DefaultListIsNotItsOwnTerminal() {
        LayoutKnownPanels known = Known();
        known.remove("sessions.list");
        known.insert("alt.list", {.Inputs = {{"open", "SessionRef"}},
                                  .Outputs = {{"selected", "SessionRef"}}});
        const DefaultLayoutResult result =
            DefaultLayoutFor({"alt.list", "terminal.view"}, known);
        QVERIFY(result.Problems.isEmpty());
        QCOMPARE(result.Tree.Root.Children[0].Panel, QString("alt.list"));
        QCOMPARE(result.Tree.Root.Children[1].Panel, QString("terminal.view"));
        QCOMPARE(result.Tree.Root.Children[1].Links["session"].From,
                 QString("list"));
    }

    void DefaultWithSeveralTerminalsTakesTheFirstAndNamesTheOthers() {
        LayoutKnownPanels known = Known();
        known.insert("a.screen",
                     {.Inputs = {{"open", "SessionRef"}}, .Outputs = {}});
        const DefaultLayoutResult result = DefaultLayoutFor(
            {"sessions.list", "a.screen", "terminal.view"}, known);
        QCOMPARE(result.Tree.Root.Children[1].Panel, QString("a.screen"));
        QCOMPARE(result.Tree.Root.Children[1].Links.keys(),
                 QStringList{"open"});
        QCOMPARE(result.Problems.size(), 1);
        QVERIFY(
            result.Problems.first().contains("several panels take a "
                                             "SessionRef input"));
        QVERIFY(result.Problems.first().contains("a.screen is used"));
        QVERIFY(result.Problems.first().contains("terminal.view not placed"));
    }

    void UnusableDocumentGivesTheFallbackTree() {
        LayoutKnownPanels known = Known();
        known.insert("alt.list",
                     {.Inputs = {}, .Outputs = {{"selected", "SessionRef"}}});
        known.remove("sessions.list");
        const Layout other =
            DefaultLayoutFor({"alt.list", "terminal.view"}, known).Tree;
        QCOMPARE(other.Root.Children[0].Panel, QString("alt.list"));
        QCOMPARE(ParseLayout(Json("[1]"), known, other).Tree, other);
        QCOMPARE(ParseLayout(Json(R"({"version": 2})"), known, other).Tree,
                 other);
        QCOMPARE(ParseLayout(Json(R"({"root": 5})"), known, other).Tree, other);
    }
};

QTEST_GUILESS_MAIN(LayoutModelTest)
#include "layout_model_test.moc"
