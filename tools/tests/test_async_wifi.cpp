#include <atomic>
#include <cassert>
#include <cstring>
#include <functional>
#include <latch>
#include <thread>

#include "host/network/async_wifi.hpp"

using namespace micropixel;
class Wifi final : public device::Wifi {
   public:
    device::WifiSnapshot state{};
    mutable unsigned reads{};
    unsigned writes{};
    bool fail{};
    std::function<void()> read_hook;
    std::function<void()> write_hook;
    device::WifiStateChangeSink sink{};
    void* context{};
    std::expected<void, device::WifiError> Initialize() override { return {}; }
    device::WifiSnapshot Snapshot() const override {
        ++reads;
        if (read_hook) read_hook();
        return state;
    }
    void SetStateChangeSink(device::WifiStateChangeSink s, void* c) override {
        sink = s;
        context = c;
    }
    std::expected<void, device::WifiError> SetEnabled(bool enabled) override {
        ++writes;
        if (write_hook) write_hook();
        if (fail) return std::unexpected(device::WifiError::kOperationFailed);
        state.enabled = enabled;
        if (sink) sink(context);
        return {};
    }
    std::expected<void, device::WifiError> RequestScan() override { return SetEnabled(true); }
    std::expected<void, device::WifiError> ConnectSaved(std::string_view) override { return SetEnabled(true); }
    std::expected<void, device::WifiError> Connect(std::string_view, std::string_view password) override {
        assert(password == "copied-secret");
        return SetEnabled(true);
    }
    std::expected<void, device::WifiError> Disconnect() override { return SetEnabled(false); }
    std::expected<void, device::WifiError> Forget(std::string_view) override { return SetEnabled(false); }
};
int main() {
    Wifi driver;
    driver.state.available = true;
    work::BackgroundExecutor worker;
    {
        host::network::AsyncWifi wifi(driver, worker);
        assert(driver.reads == 0);  // Construction does not issue an RPC either.
        worker.Run();
        const unsigned reads = driver.reads;
        assert(wifi.Snapshot().available && driver.reads == reads);
        assert(wifi.SetEnabled(true));
        assert(wifi.Snapshot().enabled && wifi.Snapshot().control_pending && driver.writes == 0);
        assert(!wifi.SetEnabled(false));  // An opposite operation cannot queue behind a pending one.
        worker.Run();
        assert(driver.writes == 1 && !wifi.Snapshot().control_pending && wifi.Snapshot().enabled);
        driver.fail = true;
        assert(wifi.SetEnabled(false));
        assert(!wifi.Snapshot().enabled && wifi.Snapshot().control_pending);
        worker.Run();
        assert(wifi.Snapshot().enabled && wifi.Snapshot().control_failed && !wifi.Snapshot().control_pending);
        driver.fail = false;
        worker.accepting = false;
        assert(!wifi.SetEnabled(false));
        assert(wifi.Snapshot().enabled && !wifi.Snapshot().control_pending);
        worker.accepting = true;
        // A blocked driver command must not hold the snapshot/control mutex.
        std::latch entered(1), release(1);
        driver.write_hook = [&] {
            entered.count_down();
            release.wait();
        };
        assert(wifi.SetEnabled(false));
        std::thread running([&] { worker.Run(); });
        entered.wait();
        assert(!wifi.Snapshot().enabled && wifi.Snapshot().control_pending);
        assert(!wifi.RequestScan());
        release.count_down();
        running.join();
        driver.write_hook = {};
        assert(!wifi.Snapshot().enabled && !wifi.Snapshot().control_pending);
        // A cached read remains immediate during a blocked signal RPC; an accepted
        // command retains its desired state until the worker actually executes it.
        std::latch reading(1), finish_read(1);
        std::atomic_bool first{true};
        driver.read_hook = [&] {
            if (first.exchange(false)) {
                reading.count_down();
                finish_read.wait();
            }
        };
        wifi.Poll();
        std::thread polling([&] { worker.Run(); });
        reading.wait();
        assert(!wifi.Snapshot().enabled);
        assert(wifi.SetEnabled(true));
        assert(wifi.Snapshot().enabled && wifi.Snapshot().control_pending);
        finish_read.count_down();
        polling.join();
        driver.read_hook = {};
        assert(wifi.Snapshot().enabled && !wifi.Snapshot().control_pending);
        char password[] = "copied-secret";
        assert(wifi.Connect("test", password));
        std::memset(password, 0, sizeof(password));
        worker.Run();
        assert(!wifi.Snapshot().control_pending);
    }
    assert(driver.sink == nullptr);
    std::puts(
        "Async Wi-Fi: cached reads, blocked RPC/control, rollback, bounded requests and owned credentials passed");
}
