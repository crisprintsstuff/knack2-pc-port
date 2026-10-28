// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include "g4_asc_owner_pause.h"

using namespace iss::g4;

int main() {
    GraphicsControlMailbox mailbox;
    AscOwnerPause pause;
    bool ran{};
    auto upload = mailbox.Enqueue([&] { ran = true; });
    assert(pause.Begin(2, upload, 5));
    assert(pause.Next().gate == AscOwnerPause::Gate::Waiting);
    assert(!pause.Acknowledged());
    assert(!pause.AcknowledgePaused(5, 4, true)); // C prefix still open
    assert(!pause.AcknowledgePaused(5, 5, false)); // C has session work
    assert(!pause.AcknowledgePaused(6, 6, true)); // stale request observation
    assert(pause.AcknowledgePaused(5, 5, true));
    assert(pause.Acknowledged());
    assert(mailbox.ServeOne(true) && ran);
    assert(pause.Next().gate == AscOwnerPause::Gate::ResumeOrigin);
    assert(pause.AcknowledgeOriginResume(2));
    assert(pause.Next().gate == AscOwnerPause::Gate::Open);

    auto guest = mailbox.Enqueue([] {});
    assert(pause.BeginGuest(guest, 7));
    assert(!pause.Begin(1, guest, 7)); // no nested C/G readback control
    assert(pause.AcknowledgePaused(7, 7, true));
    assert(mailbox.ServeOne(true));
    assert(pause.Next().gate == AscOwnerPause::Gate::ReleaseGuest);
    assert(pause.AcknowledgeGuestRelease());
    assert(pause.Next().gate == AscOwnerPause::Gate::Open);
}
