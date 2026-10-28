// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>

#include "g4_record_ownership.h"

using namespace iss::g4;

int main() {
    assert(CurrentRecordingOwnerMatches(CpLane::Graphics));
    assert(CurrentRecordingOwnerMatches(CpLane::Asc));
    {
        RecordingOwnerScope asc{{CpLane::Asc, 2, 7}};
        assert(CurrentRecordingOwnerMatches(CpLane::Asc));
        assert(!CurrentRecordingOwnerMatches(CpLane::Graphics));
        assert(active_recording_identity->guest_qid == 2);
        {
            RecordingOwnerScope graphics{{CpLane::Graphics, 2, 7}};
            assert(CurrentRecordingOwnerMatches(CpLane::Graphics));
            assert(!CurrentRecordingOwnerMatches(CpLane::Asc));
        }
        assert(CurrentRecordingOwnerMatches(CpLane::Asc));
    }
    assert(CurrentRecordingOwnerMatches(CpLane::Graphics));
}
