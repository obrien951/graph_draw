#include <catch2/catch_all.hpp>
#include <QApplication>
#include <memory>

// Catch2 v3 session-level setup/teardown via EventListenerBase.
// testRunStarting fires once before any test; testRunEnded fires after all of
// them, giving every test case a live QApplication for the full run.
// On headless / CI systems set QT_QPA_PLATFORM=offscreen.
class QtAppListener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override
    {
        static int argc = 0;
        m_app = std::make_unique<QApplication>(argc, nullptr);
    }

    void testRunEnded(Catch::TestRunStats const&) override
    {
        m_app.reset();
    }

private:
    std::unique_ptr<QApplication> m_app;
};

CATCH_REGISTER_LISTENER(QtAppListener);
