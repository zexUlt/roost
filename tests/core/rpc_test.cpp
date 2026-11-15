// JSON-RPC 2.0 dispatch (design.md §4). The examples of section 7 of the
// specification come first, verbatim, then what the specification leaves to
// the server: asynchronous answers and handlers that fail.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QPromise>
#include <QTest>

#include <memory>
#include <stdexcept>

#include "core/rpc.hpp"

using roost::rpc::Call;
using roost::rpc::Dispatcher;
using roost::rpc::EErrorCode;
using roost::rpc::Ready;
using roost::rpc::Result;

namespace {

QJsonValue Json(QByteArrayView text) {
    const QJsonDocument document = QJsonDocument::fromJson(text.toByteArray());
    if (document.isNull()) {
        qFatal("bad JSON in the test: %s", text.toByteArray().constData());
    }
    return document.isArray() ? QJsonValue(document.array())
                              : QJsonValue(document.object());
}

// The response to a message whose handlers answer at once; undefined when
// there is none.
QJsonValue Answer(const Dispatcher& dispatcher, QByteArrayView message) {
    const QFuture<QByteArray> future = dispatcher.Handle(message, nullptr);
    if (!future.isFinished()) {
        qFatal("the dispatcher did not answer at once");
    }
    const QByteArray body = future.result();
    return body.isEmpty() ? QJsonValue(QJsonValue::Undefined) : Json(body);
}

int ErrorCode(const QJsonValue& response) {
    return response[QStringLiteral("error")][QStringLiteral("code")].toInt();
}

constexpr int Code(EErrorCode code) { return static_cast<int>(code); }

QJsonValue Subtract(const QJsonValue& params) {
    if (params.isArray()) {
        return params[0].toInt() - params[1].toInt();
    }
    return params[QStringLiteral("minuend")].toInt() -
           params[QStringLiteral("subtrahend")].toInt();
}

// The methods the examples of the specification call.
class SpecMethods {
public:
    SpecMethods() {
        Add("subtract",
            [](const Call& call) { return Ready(Subtract(call.Params)); });
        Add("sum", [](const Call& call) {
            int sum = 0;
            for (const QJsonValue& term : call.Params.toArray()) {
                sum += term.toInt();
            }
            return Ready(sum);
        });
        Add("update", [this](const Call&) {
            ++Updates;
            return Ready(QJsonValue::Null);
        });
        Add("notify_hello", [this](const Call&) {
            ++Updates;
            return Ready(QJsonValue::Null);
        });
        Add("notify_sum", [this](const Call&) {
            ++Updates;
            return Ready(QJsonValue::Null);
        });
        Add("get_data", [](const Call&) {
            return Ready(QJsonArray{QStringLiteral("hello"), 5});
        });
    }

    Dispatcher Methods;
    int Updates = 0;

private:
    void Add(const char* name, roost::rpc::Handler handler) {
        if (!Methods.Register(QString::fromLatin1(name), std::move(handler))) {
            qFatal("could not register %s", name);
        }
    }
};

}  // namespace

class RpcTest : public QObject {
    Q_OBJECT

private slots:
    void PositionalParameters();
    void NamedParameters();
    void Notifications();
    void MethodNotFound();
    void ParseError();
    void InvalidRequest_data();
    void InvalidRequest();
    void BatchWithInvalidJson();
    void EmptyBatch();
    void BatchOfJunk();
    void MixedBatch();
    void BatchOfNotifications();
    void NullIdIsARequest();
    void UndefinedResultIsNull();
    void ErrorDataIsSent();
    void HandlerSeesMethodParamsAndPeer();
    void AsynchronousAnswer();
    void BatchWaitsForEveryAnswer();
    void HandlerThrows();
    void HandlerFails();
    void HandlerDropsThePromise();
    void HandlerFinishesWithoutAResult();
    void BatchSurvivesEveryFailure();
    void AnswerOutlivesTheDispatcher();
    void ReservedAndTakenNames();
};

void RpcTest::PositionalParameters() {
    SpecMethods spec;
    QCOMPARE(
        Answer(
            spec.Methods,
            R"({"jsonrpc": "2.0", "method": "subtract", "params": [42, 23], "id": 1})"),
        Json(R"({"jsonrpc": "2.0", "result": 19, "id": 1})"));
    QCOMPARE(
        Answer(
            spec.Methods,
            R"({"jsonrpc": "2.0", "method": "subtract", "params": [23, 42], "id": 2})"),
        Json(R"({"jsonrpc": "2.0", "result": -19, "id": 2})"));
}

void RpcTest::NamedParameters() {
    SpecMethods spec;
    QCOMPARE(
        Answer(
            spec.Methods,
            R"({"jsonrpc": "2.0", "method": "subtract", "params": {"subtrahend": 23, "minuend": 42}, "id": 3})"),
        Json(R"({"jsonrpc": "2.0", "result": 19, "id": 3})"));
    QCOMPARE(
        Answer(
            spec.Methods,
            R"({"jsonrpc": "2.0", "method": "subtract", "params": {"minuend": 42, "subtrahend": 23}, "id": 4})"),
        Json(R"({"jsonrpc": "2.0", "result": 19, "id": 4})"));
}

void RpcTest::Notifications() {
    SpecMethods spec;
    QVERIFY(
        Answer(
            spec.Methods,
            R"({"jsonrpc": "2.0", "method": "update", "params": [1,2,3,4,5]})")
            .isUndefined());
    QCOMPARE(spec.Updates, 1);
    // Not even an unknown method is answered.
    QVERIFY(Answer(spec.Methods, R"({"jsonrpc": "2.0", "method": "foobar"})")
                .isUndefined());
}

void RpcTest::MethodNotFound() {
    SpecMethods spec;
    const QJsonValue response = Answer(
        spec.Methods, R"({"jsonrpc": "2.0", "method": "foobar", "id": "1"})");
    QCOMPARE(ErrorCode(response), Code(EErrorCode::MethodNotFound));
    QCOMPARE(response[QStringLiteral("id")], QJsonValue(QStringLiteral("1")));
}

void RpcTest::ParseError() {
    SpecMethods spec;
    const QJsonValue response = Answer(
        spec.Methods,
        R"({"jsonrpc": "2.0", "method": "foobar, "params": "bar", "baz])");
    QCOMPARE(ErrorCode(response), Code(EErrorCode::ParseError));
    QCOMPARE(response[QStringLiteral("id")], QJsonValue(QJsonValue::Null));
}

void RpcTest::InvalidRequest_data() {
    QTest::addColumn<QByteArray>("message");
    QTest::addColumn<QJsonValue>("id");

    const QJsonValue null(QJsonValue::Null);
    QTest::newRow("spec: method is a number")
        << QByteArray(R"({"jsonrpc": "2.0", "method": 1, "params": "bar"})")
        << null;
    QTest::newRow("no jsonrpc")
        << QByteArray(R"({"method": "sum", "params": [1], "id": 7})")
        << QJsonValue(7);
    QTest::newRow("jsonrpc 1.0")
        << QByteArray(R"({"jsonrpc": "1.0", "method": "sum", "id": 7})")
        << QJsonValue(7);
    QTest::newRow("jsonrpc as a number")
        << QByteArray(R"({"jsonrpc": 2.0, "method": "sum", "id": 7})")
        << QJsonValue(7);
    QTest::newRow("no method")
        << QByteArray(R"({"jsonrpc": "2.0", "id": 7})") << QJsonValue(7);
    QTest::newRow("scalar params")
        << QByteArray(
               R"({"jsonrpc": "2.0", "method": "sum", "params": 3, "id": 7})")
        << QJsonValue(7);
    QTest::newRow("null params")
        << QByteArray(
               R"({"jsonrpc": "2.0", "method": "sum", "params": null, "id": 7})")
        << QJsonValue(7);
    QTest::newRow("object id")
        << QByteArray(R"({"jsonrpc": "2.0", "method": "sum", "id": {}})")
        << null;
    QTest::newRow("boolean id")
        << QByteArray(R"({"jsonrpc": "2.0", "method": "sum", "id": true})")
        << null;
    QTest::newRow("a response")
        << QByteArray(R"({"jsonrpc": "2.0", "result": 1, "id": 7})")
        << QJsonValue(7);
    QTest::newRow("bare number") << QByteArray("1") << null;
    QTest::newRow("bare string") << QByteArray(R"("sum")") << null;
    QTest::newRow("bare null") << QByteArray(" null ") << null;
}

void RpcTest::InvalidRequest() {
    QFETCH(QByteArray, message);
    QFETCH(QJsonValue, id);
    SpecMethods spec;
    const QJsonValue response = Answer(spec.Methods, message);
    QCOMPARE(ErrorCode(response), Code(EErrorCode::InvalidRequest));
    QCOMPARE(response[QStringLiteral("id")], id);
    QCOMPARE(response[QStringLiteral("jsonrpc")],
             QJsonValue(QStringLiteral("2.0")));
}

void RpcTest::BatchWithInvalidJson() {
    SpecMethods spec;
    const QJsonValue response = Answer(spec.Methods, R"([
        {"jsonrpc": "2.0", "method": "sum", "params": [1,2,4], "id": "1"},
        {"jsonrpc": "2.0", "method"
    ])");
    QVERIFY(response.isObject());
    QCOMPARE(ErrorCode(response), Code(EErrorCode::ParseError));
}

void RpcTest::EmptyBatch() {
    SpecMethods spec;
    const QJsonValue response = Answer(spec.Methods, "[]");
    QVERIFY(response.isObject());
    QCOMPARE(ErrorCode(response), Code(EErrorCode::InvalidRequest));
    QCOMPARE(response[QStringLiteral("id")], QJsonValue(QJsonValue::Null));
}

void RpcTest::BatchOfJunk() {
    SpecMethods spec;
    const QJsonArray one = Answer(spec.Methods, "[1]").toArray();
    QCOMPARE(one.size(), 1);
    QCOMPARE(ErrorCode(one[0]), Code(EErrorCode::InvalidRequest));

    const QJsonArray three = Answer(spec.Methods, "[1,2,3]").toArray();
    QCOMPARE(three.size(), 3);
    for (const QJsonValue& response : three) {
        QCOMPARE(ErrorCode(response), Code(EErrorCode::InvalidRequest));
        QCOMPARE(response[QStringLiteral("id")], QJsonValue(QJsonValue::Null));
    }
}

void RpcTest::MixedBatch() {
    SpecMethods spec;
    const QJsonArray responses = Answer(spec.Methods, R"([
        {"jsonrpc": "2.0", "method": "sum", "params": [1,2,4], "id": "1"},
        {"jsonrpc": "2.0", "method": "notify_hello", "params": [7]},
        {"jsonrpc": "2.0", "method": "subtract", "params": [42,23], "id": "2"},
        {"foo": "boo"},
        {"jsonrpc": "2.0", "method": "foo.get", "params": {"name": "myself"}, "id": "5"},
        {"jsonrpc": "2.0", "method": "get_data", "id": "9"}
    ])")
                                     .toArray();

    QCOMPARE(responses.size(), 5);
    QCOMPARE(responses[0],
             Json(R"({"jsonrpc": "2.0", "result": 7, "id": "1"})"));
    QCOMPARE(responses[1],
             Json(R"({"jsonrpc": "2.0", "result": 19, "id": "2"})"));
    QCOMPARE(ErrorCode(responses[2]), Code(EErrorCode::InvalidRequest));
    QCOMPARE(responses[2][QStringLiteral("id")], QJsonValue(QJsonValue::Null));
    QCOMPARE(ErrorCode(responses[3]), Code(EErrorCode::MethodNotFound));
    QCOMPARE(responses[3][QStringLiteral("id")],
             QJsonValue(QStringLiteral("5")));
    QCOMPARE(responses[4],
             Json(R"({"jsonrpc": "2.0", "result": ["hello", 5], "id": "9"})"));
    QCOMPARE(spec.Updates, 1);
}

void RpcTest::BatchOfNotifications() {
    SpecMethods spec;
    QVERIFY(Answer(spec.Methods, R"([
        {"jsonrpc": "2.0", "method": "notify_sum", "params": [1,2,4]},
        {"jsonrpc": "2.0", "method": "notify_hello", "params": [7]}
    ])")
                .isUndefined());
    QCOMPARE(spec.Updates, 2);
}

void RpcTest::NullIdIsARequest() {
    SpecMethods spec;
    QCOMPARE(
        Answer(
            spec.Methods,
            R"({"jsonrpc": "2.0", "method": "sum", "params": [2], "id": null})"),
        Json(R"({"jsonrpc": "2.0", "result": 2, "id": null})"));
}

void RpcTest::UndefinedResultIsNull() {
    Dispatcher dispatcher;
    QVERIFY(dispatcher.Register(QStringLiteral("nothing"), [](const Call&) {
        return Ready(QJsonValue(QJsonValue::Undefined));
    }));
    QCOMPARE(Answer(dispatcher,
                    R"({"jsonrpc": "2.0", "method": "nothing", "id": 1})"),
             Json(R"({"jsonrpc": "2.0", "result": null, "id": 1})"));
}

void RpcTest::ErrorDataIsSent() {
    Dispatcher dispatcher;
    QVERIFY(dispatcher.Register(QStringLiteral("picky"), [](const Call&) {
        return Ready(std::unexpected(roost::rpc::Error::FromCode(
            EErrorCode::InvalidParams, QStringLiteral("need a name"),
            QJsonObject{{QStringLiteral("field"), QStringLiteral("name")}})));
    }));
    QVERIFY(dispatcher.Register(QStringLiteral("own_code"), [](const Call&) {
        return Ready(std::unexpected(
            roost::rpc::Error{.Code = 1, .Message = QStringLiteral("busy")}));
    }));

    QCOMPARE(
        Answer(dispatcher, R"({"jsonrpc": "2.0", "method": "picky", "id": 1})"),
        Json(R"({"jsonrpc": "2.0", "id": 1, "error":
                     {"code": -32602, "message": "need a name", "data": {"field": "name"}}})"));
    QCOMPARE(
        Answer(dispatcher,
               R"({"jsonrpc": "2.0", "method": "own_code", "id": 2})"),
        Json(
            R"({"jsonrpc": "2.0", "id": 2, "error": {"code": 1, "message": "busy"}})"));
}

void RpcTest::HandlerSeesMethodParamsAndPeer() {
    Dispatcher dispatcher;
    Call seen;
    QVERIFY(
        dispatcher.Register(QStringLiteral("look"), [&seen](const Call& call) {
            seen = call;
            return Ready(QJsonValue::Null);
        }));
    QObject peer;

    dispatcher.Handle(
        R"({"jsonrpc": "2.0", "method": "look", "params": {"a": 1}})", &peer);
    QCOMPARE(seen.Method, QStringLiteral("look"));
    QCOMPARE(seen.Params, Json(R"({"a": 1})"));
    QCOMPARE(seen.Peer, &peer);

    dispatcher.Handle(R"({"jsonrpc": "2.0", "method": "look"})", nullptr);
    QVERIFY(seen.Params.isUndefined());
    QCOMPARE(seen.Peer, nullptr);
}

void RpcTest::AsynchronousAnswer() {
    Dispatcher dispatcher;
    auto promise = std::make_shared<QPromise<Result>>();
    QVERIFY(dispatcher.Register(QStringLiteral("slow"), [promise](const Call&) {
        promise->start();
        return promise->future();
    }));

    const QFuture<QByteArray> response = dispatcher.Handle(
        R"({"jsonrpc": "2.0", "method": "slow", "id": 1})", nullptr);
    QVERIFY(!response.isFinished());

    promise->addResult(Result(42));
    promise->finish();
    QVERIFY(response.isFinished());
    QCOMPARE(Json(response.result()),
             Json(R"({"jsonrpc": "2.0", "result": 42, "id": 1})"));
}

void RpcTest::BatchWaitsForEveryAnswer() {
    SpecMethods spec;
    auto promise = std::make_shared<QPromise<Result>>();
    QVERIFY(
        spec.Methods.Register(QStringLiteral("slow"), [promise](const Call&) {
            promise->start();
            return promise->future();
        }));

    const QFuture<QByteArray> response = spec.Methods.Handle(R"([
        {"jsonrpc": "2.0", "method": "slow", "id": 1},
        {"jsonrpc": "2.0", "method": "sum", "params": [3], "id": 2}
    ])",
                                                             nullptr);
    QVERIFY(!response.isFinished());

    promise->addResult(Result(QStringLiteral("late")));
    promise->finish();
    QVERIFY(response.isFinished());
    QCOMPARE(Json(response.result()), Json(R"([
        {"jsonrpc": "2.0", "result": "late", "id": 1},
        {"jsonrpc": "2.0", "result": 3, "id": 2}
    ])"));
}

void RpcTest::HandlerThrows() {
    Dispatcher dispatcher;
    QVERIFY(dispatcher.Register(QStringLiteral("boom"),
                                [](const Call&) -> QFuture<Result> {
                                    throw std::runtime_error("boom");
                                }));
    const QJsonValue response =
        Answer(dispatcher, R"({"jsonrpc": "2.0", "method": "boom", "id": 1})");
    QCOMPARE(ErrorCode(response), Code(EErrorCode::InternalError));
    QCOMPARE(response[QStringLiteral("id")], QJsonValue(1));
}

void RpcTest::HandlerFails() {
    Dispatcher dispatcher;
    QVERIFY(dispatcher.Register(QStringLiteral("fail"), [](const Call&) {
        QPromise<Result> promise;
        promise.start();
        promise.setException(std::make_exception_ptr(std::runtime_error("no")));
        promise.finish();
        return promise.future();
    }));
    const QJsonValue response =
        Answer(dispatcher, R"({"jsonrpc": "2.0", "method": "fail", "id": 1})");
    QCOMPARE(ErrorCode(response), Code(EErrorCode::InternalError));
}

void RpcTest::HandlerDropsThePromise() {
    Dispatcher dispatcher;
    QVERIFY(dispatcher.Register(QStringLiteral("drop"), [](const Call&) {
        QPromise<Result> promise;
        promise.start();
        return promise.future();
    }));
    const QJsonValue response =
        Answer(dispatcher, R"({"jsonrpc": "2.0", "method": "drop", "id": 1})");
    QCOMPARE(ErrorCode(response), Code(EErrorCode::InternalError));
}

void RpcTest::HandlerFinishesWithoutAResult() {
    Dispatcher dispatcher;
    QVERIFY(dispatcher.Register(QStringLiteral("empty"), [](const Call&) {
        QPromise<Result> promise;
        promise.start();
        promise.finish();
        return promise.future();
    }));
    const QJsonValue response =
        Answer(dispatcher, R"({"jsonrpc": "2.0", "method": "empty", "id": 1})");
    QCOMPARE(ErrorCode(response), Code(EErrorCode::InternalError));
    QCOMPARE(response[QStringLiteral("id")], QJsonValue(1));
}

// Every way a handler can fail, side by side in one batch: each element gets
// its own error and the healthy one its result.
void RpcTest::BatchSurvivesEveryFailure() {
    SpecMethods spec;
    QVERIFY(spec.Methods.Register(
        QStringLiteral("throw"),
        [](const Call&) -> QFuture<Result> { throw std::runtime_error("x"); }));
    QVERIFY(spec.Methods.Register(QStringLiteral("fail"), [](const Call&) {
        QPromise<Result> promise;
        promise.start();
        promise.setException(std::make_exception_ptr(std::runtime_error("x")));
        promise.finish();
        return promise.future();
    }));
    QVERIFY(spec.Methods.Register(QStringLiteral("drop"), [](const Call&) {
        QPromise<Result> promise;
        promise.start();
        return promise.future();
    }));
    QVERIFY(spec.Methods.Register(QStringLiteral("empty"), [](const Call&) {
        QPromise<Result> promise;
        promise.start();
        promise.finish();
        return promise.future();
    }));

    const QJsonArray responses = Answer(spec.Methods, R"([
        {"jsonrpc": "2.0", "method": "throw", "id": 1},
        {"jsonrpc": "2.0", "method": "fail", "id": 2},
        {"jsonrpc": "2.0", "method": "drop", "id": 3},
        {"jsonrpc": "2.0", "method": "empty", "id": 4},
        {"jsonrpc": "2.0", "method": "drop"},
        {"jsonrpc": "2.0", "method": "sum", "params": [5], "id": 5}
    ])")
                                     .toArray();

    QCOMPARE(responses.size(), 5);
    for (int i = 0; i < 4; ++i) {
        QCOMPARE(ErrorCode(responses[i]), Code(EErrorCode::InternalError));
        QCOMPARE(responses[i][QStringLiteral("id")], QJsonValue(i + 1));
    }
    QCOMPARE(responses[4], Json(R"({"jsonrpc": "2.0", "result": 5, "id": 5})"));
}

void RpcTest::AnswerOutlivesTheDispatcher() {
    auto dispatcher = std::make_unique<Dispatcher>();
    auto promise = std::make_shared<QPromise<Result>>();
    QVERIFY(
        dispatcher->Register(QStringLiteral("slow"), [promise](const Call&) {
            promise->start();
            return promise->future();
        }));
    const QFuture<QByteArray> response = dispatcher->Handle(
        R"({"jsonrpc": "2.0", "method": "slow", "id": 1})", nullptr);
    dispatcher.reset();

    promise->addResult(Result(1));
    promise->finish();
    QVERIFY(response.isFinished());
    QCOMPARE(Json(response.result()),
             Json(R"({"jsonrpc": "2.0", "result": 1, "id": 1})"));
}

void RpcTest::ReservedAndTakenNames() {
    Dispatcher dispatcher;
    auto null = [](const Call&) { return Ready(QJsonValue::Null); };
    QVERIFY(!dispatcher.Register(QStringLiteral("rpc.discover"), null));
    QVERIFY(dispatcher.Register(QStringLiteral("hello"), null));
    QVERIFY(!dispatcher.Register(QStringLiteral("hello"), null));
}

QTEST_GUILESS_MAIN(RpcTest)
#include "rpc_test.moc"
