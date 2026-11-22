// Linked into every test binary: a direct run (build/dev/bin/<test>) must not
// open windows or write a QML disk cache, as under ctest. Child processes such
// as roost and roostd inherit the variables. A value set explicitly is kept.

#include <QByteArray>
#include <QtGlobal>

namespace {

struct TestEnvironment {
    TestEnvironment() {
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
        if (!qEnvironmentVariableIsSet("QML_DISABLE_DISK_CACHE")) {
            qputenv("QML_DISABLE_DISK_CACHE", "1");
        }
        if (!qEnvironmentVariableIsSet("ROOST_CODEX_EXECUTABLE")) {
            qputenv("ROOST_CODEX_EXECUTABLE", "roost-no-codex-in-tests");
        }
        // Empty: the window makes no request for a newer release. A test of
        // the check sets a URL of its own.
        if (!qEnvironmentVariableIsSet("ROOST_TEST_UPDATES_URL")) {
            qputenv("ROOST_TEST_UPDATES_URL", "");
        }
    }
};

const TestEnvironment TEST_ENVIRONMENT;

}  // namespace
