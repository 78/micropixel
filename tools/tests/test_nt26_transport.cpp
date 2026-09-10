#include <array>
#include <cassert>
#include <string>

#include "nt26_at_response.h"
#include "nt26_frame.h"
#include "nt26_tx_pool.h"

using micropixel::nt26::AtResponse;
using micropixel::nt26::TxPool;

int main() {
    micropixel::nt26::FrameHeader header{};
    header.SetPayloadLength(12);
    header.SetSequence(0);
    header.SetType(micropixel::nt26::FrameType::kEthernet);
    header.UpdateChecksum();
    assert(header.raw[0] == 0x0c && header.raw[1] == 0 && header.raw[2] == 0 && header.raw[3] == 0x0f);
    header.SetPayloadLength(1596);
    header.SetSequence(31);  // Four-bit sequence wraps independently of length.
    header.SetType(micropixel::nt26::FrameType::kAtCommand);
    header.SetContinue(true);
    header.SetFlowControl(true);
    header.UpdateChecksum();
    assert(header.GetPayloadLength() == 1596 && header.GetSequence() == 15);
    assert(header.GetContinue() && header.GetFlowControl() && header.ValidateChecksum());
    header.raw[0] ^= 1;
    assert(!header.ValidateChecksum());

    TxPool pool;
    std::array<TxPool::Slot*, TxPool::kCapacity> pending{};
    for (auto& slot : pending) {
        slot = pool.Acquire(1600, true);
        assert(slot != nullptr);
        slot->data[0] = 0x53;
    }
    assert(pool.Acquire(1, false) == nullptr);
    assert(pool.Acquire(0, false) == nullptr);
    assert(pool.Acquire(1601, false) == nullptr);

    // The caller times out while the frame is still queued. It cannot recycle
    // either payload or completion storage until the worker acknowledges it.
    pool.ReleaseWaiter(*pending[0]);
    assert(pool.Acquire(1, false) == nullptr);
    assert(pending[0]->data[0] == 0x53);
    pool.Complete(*pending[0], -1);
    auto* reused = pool.Acquire(32, false);
    assert(reused == pending[0]);
    assert(!reused->completed && !reused->waiter && reused->worker);

    // Completion may arrive just before the caller processes the wakeup.
    pool.Complete(*pending[1], 42);
    assert(pool.Acquire(1, false) == nullptr);
    assert(pending[1]->result == 42 && pending[1]->completed);
    pool.ReleaseWaiter(*pending[1]);
    assert(pool.Acquire(1, false) == pending[1]);

    // Shutdown cancels every remaining queued frame; callers retain ownership
    // until observing their completion. Queue-send failure has the same release.
    for (size_t i = 2; i < pending.size(); ++i) {
        pool.Complete(*pending[i], -2);
        assert(pending[i]->result == -2);
        assert(pool.Acquire(1, false) == nullptr);
        pool.ReleaseWaiter(*pending[i]);
        assert(pool.Acquire(1, false) == pending[i]);
    }

    AtResponse response;
    using Status = AtResponse::Status;
    assert(response.Begin());
    assert(response.Append("\r\n+COPS: 0,0,\"OK MOBILE\",7\r\n") == Status::kPending);
    assert(response.Append("\r\nO") == Status::kPending);
    assert(response.Append("K\r\n") == Status::kOk);
    assert(response.text().find("+COPS:") != std::string_view::npos);

    assert(response.Begin("+ECPING: DONE"));
    assert(response.Append("\r\nOK\r\n") == Status::kPending);
    assert(response.Append("\r\n+ECPING: DO") == Status::kPending);
    assert(response.Append("NE\r\n") == Status::kOk);
    assert(response.Begin());
    assert(response.Append("\r\n+CME ER") == Status::kPending);
    assert(response.Append("ROR: 10\r\n") == Status::kError);
    assert(response.Begin());
    assert(response.Append("\r\nERROR\r\n") == Status::kError);
    assert(response.Begin());
    assert(response.Append(std::string(AtResponse::kCapacity - 1, 'x')) == Status::kPending);
    assert(response.Append("x") == Status::kOverflow);
    assert(response.text().size() == AtResponse::kCapacity - 1);
    assert(!response.Begin(std::string(AtResponse::kMarkerCapacity, 'x')));
    assert(response.Begin());
    assert(response.text().empty());
    assert(response.Append("OK\r\n") == Status::kOk);
}
