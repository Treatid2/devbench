#include "PostLoadStartup.h"
#include "EventBus.h"
#include "mcp_server.h"
#include <Windows.h>
#include <atomic>
#include <memory>
#include <stdexcept>

namespace {
bool inLoader = false;
struct InjectedPreparationExit {};
dvb::PostLoadStartup startup;
struct Operations {
    dvb::EventBus bus{false}; // Exact production thread-free ownership mode.
    std::unique_ptr<mcp::server> server;
    std::atomic<unsigned> workers{0};
    unsigned factoryCalls = 0;
    bool fail = false, failPrepare = false, registered = false, published = false, quarantined = false;
    bool exactPreparationFailure = false;
    bool Configure() { return true; }
    void Construct() {} // Transport must not be constructed in this half.
    void Prepare() {
        if (failPrepare) throw InjectedPreparationExit{};
        registered = true;
    }
    bool Listen() {
        if (inLoader) throw std::runtime_error("worker phase entered in DllMain");
        bus.StartDelivery();
        mcp::server::configuration cfg;
        cfg.host = "127.0.0.1";
        cfg.port = 37395; // Owned isolated fixture; collision is a failed test.
        cfg.threadpool_size = 2;
        cfg.startup_thread_factory = [this](std::function<void()> body) {
            if (++factoryCalls == 2 && fail)
                throw std::runtime_error("injected listener worker construction failure");
            return std::thread([this, body = std::move(body)] {
                ++workers;
                struct Exit { std::atomic<unsigned>& count; ~Exit() { --count; } } exit{workers};
                body();
            });
        };
        server = std::make_unique<mcp::server>(cfg);
        return server->start(false);
    }
    void Publish() { published = true; }
    void Quarantine() { quarantined = true; published = false; }
    void Report(dvb::PostLoadStartup::Outcome, dvb::PostLoadStartup::Phase phase, std::exception_ptr error) {
        if (phase != dvb::PostLoadStartup::Phase::Preparation || !error) return;
        try { std::rethrow_exception(error); }
        catch (const InjectedPreparationExit&) { exactPreparationFailure = true; }
        catch (...) {}
    }
};
Operations* ops = nullptr;
}

extern "C" __declspec(dllexport) int CompleteStartup(int fail) {
    if (!ops || inLoader || startup.Current() != dvb::PostLoadStartup::State::Prepared ||
        ops->server || ops->factoryCalls != 0 || !ops->registered) return 10;
    ops->fail = fail != 0;
    const auto outcome = startup.Complete(*ops);
    const bool stateOK = fail ? outcome == dvb::PostLoadStartup::Outcome::Failed &&
        !startup.Ready() && ops->quarantined && !ops->published :
        outcome == dvb::PostLoadStartup::Outcome::Ready && startup.Ready() && ops->published;
    if (ops->server) ops->server->stop(); // Actual production drain, outside loader context.
    const bool drained = ops->workers == 0;
    // Explicit owner cleanup before FreeLibrary; DllMain never joins/detaches.
    delete ops;
    ops = nullptr;
    return stateOK && drained ? 0 : 11;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        inLoader = true;
        try {
            ops = new Operations;
            wchar_t flag[2]{};
            ops->failPrepare = GetEnvironmentVariableW(L"DEVBENCH_LOADER_FIXTURE_FAIL_PREPARE", flag, 2) != 0;
            const auto result = startup.Prepare(*ops);
            if (result != dvb::PostLoadStartup::Outcome::Prepared) {
                if (ops->exactPreparationFailure && ops->quarantined && !ops->server &&
                    !ops->published && ops->factoryCalls == 0 && ops->workers == 0)
                    SetEnvironmentVariableW(L"DEVBENCH_LOADER_FIXTURE_PREPARE_FAILED", L"1");
                delete ops; // Bus was explicitly thread-free; a regression hangs this real loader test.
                ops = nullptr;
            }
            inLoader = false;
            return result == dvb::PostLoadStartup::Outcome::Prepared;
        } catch (...) { inLoader = false; return FALSE; }
    }
    return TRUE; // Never destroy worker owners under process/thread detach.
}
