"""One bounded, passive CPU/GPU join with the approved shaders and isolated saves."""
import os
from pathlib import Path
import run_probe


def main():
    out = Path(__file__).resolve().parents[1] / 'out'
    run_probe.apply_approved_environment(os.environ, out)
    os.environ.update(
        ISS_PROBE_EXE=str(out / 'releases/20261013-feedback-join/iss-probe.exe'),
        ISS_SAVE_SEED=str(out / 'user/city-savedata'),
        ISS_NATIVE_GEOMETRY='0', ISS_NATIVE_DRAW_STATE='0',
        ISS_NATIVE_DRAW_STATE_VERIFY='0', ISS_NATIVE_INDEXED_BATCH='0',
        ISS_NATIVE_INSTANCED_BATCH='0', ISS_NATIVE_CITY_FETCH='0',
        ISS_PROFILE_CITY_DRAW_GPU='0', ISS_DRAW_PIPE='0',
        ISS_MINIMAL_LOGS='0', ISS_FEEDBACK_IDENTITY='1',
        ISS_FEEDBACK_JOIN='1', ISS_FEEDBACK_JOIN_START_FRAME='1400',
        ISS_FEEDBACK_JOIN_FRAMES='60', ISS_SECONDARY_BATCH_PROBE='1',
        ISS_READBACK_DIAG='1', ISS_READBACK_DIAG_START_FRAME='1400',
        ISS_READBACK_DIAG_FRAMES='60',
        ISS_SECONDARY_BATCH_PROBE_START_SECONDS='20',
        ISS_NATIVE_MEMORY_REPORT='1')
    return run_probe.run(entry=True, linked=True, gpu=True, timeout=140)


if __name__ == '__main__':
    raise SystemExit(main())
