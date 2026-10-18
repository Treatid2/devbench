#include "PostLoadStartup.h"
#include "EventBus.h"
#include "mcp_server.h"
#include <Windows.h>
#include <atomic>
#include <memory>
#include <stdexcept>

namespace {
bool inLoader = false;
dvb::PostLoadStartup startup;
struct Operations {
    dvb::EventBus bus{false}; // Exact production thread-free ownership mode.
    std::unique_ptr<mcp::server> server;
    std::atomic<unsigned> workers{0};
    unsigned factoryCalls = 0;
    bool fail = false, registered = false, published = false, quarantined = false;
    bool Configure() { return true; }
    void Construct() {} // Transport must not be constructed in this half.
    void Prepare() { registered = true; }
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
    void Report(dvb::PostLoadStartup::Outcome, dvb::PostLoadStartup::Phase, std::exception_ptr) {}
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
            const auto result = startup.Prepare(*ops);
            inLoader = false;
            return result == dvb::PostLoadStartup::Outcome::Prepared;
        } catch (...) { inLoader = false; return FALSE; }
    }
    return TRUE; // Never destroy worker owners under process/thread detach.
}
