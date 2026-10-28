// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <vector>

#include "g4_graphics_control.h"

using iss::g4::GraphicsControlMailbox;

namespace {

void SafePointFifoAndNoReentrancy() {
    GraphicsControlMailbox mailbox;
    std::vector<int> order;
    auto first = mailbox.Enqueue([&] {
        order.push_back(1);
        assert(!mailbox.ServeOne(true)); // a control cannot recursively run another
    });
    auto second = mailbox.Enqueue([&] { order.push_back(2); });
    assert(first && second && mailbox.HasPending());
    assert(!mailbox.ServeOne(false));
    assert(first->Status() == GraphicsControlMailbox::Control::State::Queued);
    assert(mailbox.ServeOne(true));
    assert((order == std::vector<int>{1}));
    assert(first->Status() == GraphicsControlMailbox::Control::State::Done);
    assert(mailbox.ServeOne(true));
    assert((order == std::vector<int>{1, 2}));
    assert(second->Status() == GraphicsControlMailbox::Control::State::Done);
    assert(!mailbox.HasPending());
}

void FailureAndStopNeverPublishDone() {
    GraphicsControlMailbox failed_mailbox;
    auto failed = failed_mailbox.Enqueue([] { throw std::runtime_error{"G control failed"}; });
    assert(failed_mailbox.ServeOne(true));
    assert(failed->Status() == GraphicsControlMailbox::Control::State::Failed);
    bool caught{};
    try {
        std::rethrow_exception(failed->Failure());
    } catch (const std::runtime_error&) {
        caught = true;
    }
    assert(caught);

    GraphicsControlMailbox stopped_mailbox;
    auto queued = stopped_mailbox.Enqueue([] { assert(false); });
    stopped_mailbox.Stop();
    assert(queued->Status() == GraphicsControlMailbox::Control::State::Cancelled);
    assert(!stopped_mailbox.ServeOne(true) && !stopped_mailbox.Enqueue([] {}));

    GraphicsControlMailbox active_mailbox;
    std::binary_semaphore entered{0}, release{0};
    auto active = active_mailbox.Enqueue([&] { entered.release(); release.acquire(); });
    std::thread graphics([&] { assert(active_mailbox.ServeOne(true)); });
    entered.acquire();
    active_mailbox.Stop();
    assert(active->Status() == GraphicsControlMailbox::Control::State::Cancelled);
    release.release();
    graphics.join();
    assert(active->Status() == GraphicsControlMailbox::Control::State::Cancelled);
}

void FailedSealCancelsOnlyItsQueuedControl() {
    GraphicsControlMailbox mailbox;
    bool ran = false;
    auto cancelled = mailbox.Enqueue([&] { ran = true; });
    auto next = mailbox.Enqueue([&] { ran = true; });
    assert(mailbox.CancelPending(cancelled));
    assert(cancelled->Status() == GraphicsControlMailbox::Control::State::Cancelled);
    assert(!mailbox.CancelPending(cancelled));
    assert(mailbox.ServeOne(true) && next->Status() ==
           GraphicsControlMailbox::Control::State::Done && ran);
}

} // namespace

int main() {
    SafePointFifoAndNoReentrancy();
    FailureAndStopNeverPublishDone();
    FailedSealCancelsOnlyItsQueuedControl();
}
