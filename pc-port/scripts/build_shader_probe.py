"""Build an isolated probe with opt-in, reproducible controller test inputs.

Only the experimental copy of pad.c is changed. No OS input injection or user
save is involved. Run with ISS_SHADER_POWER_REPLAY=1 only in isolated benchmarks.
"""
from pathlib import Path
import sys

ROOT=Path(__file__).resolve().parents[1]
helper=(ROOT/'scripts/build_hybrid_probe.py').read_text(encoding='utf-8')
needle="    sources = ('probe.c'"
insertion='''    pad_source = exe.parent / 'pad-shader-replay.c'
    pad = (ROOT / 'src/pad.c').read_text(encoding='utf-8')
    marker = '    if(out->l2>30)out->buttons|=0x100;'
    assert pad.count(marker) == 1
    replay = r\"\"\"
    /* Test only: repeat the same powers on an isolated save, independent of UI focus. */
    static int shader_replay = -1;
    static uint64_t shader_replay_start = 0;
    static unsigned shader_replay_last = 0;
    if (shader_replay < 0) {
        const char *flag = getenv("ISS_SHADER_POWER_REPLAY");
        shader_replay = flag && flag[0] == '1';
    }
    if (shader_replay) {
        if (!shader_replay_start) shader_replay_start = out->timestamp;
        uint64_t elapsed = out->timestamp - shader_replay_start;
        if (elapsed >= 40000000 && elapsed < 64000000) {
            unsigned cycle = (unsigned)((elapsed - 40000000) / 3000000);
            unsigned phase = (unsigned)((elapsed - 40000000) % 3000000);
            if (shader_replay_last != cycle + 1) {
                shader_replay_last = cycle + 1;
                fprintf(stderr,"SHADER_POWER_REPLAY cycle=%u elapsed_us=%llu\\n",
                    cycle, (unsigned long long)elapsed);
            }
            if (phase < 250000) out->buttons |= 0x800; /* R1 */
            if (phase >= 750000 && phase < 1750000) out->r2 = 255; /* R2 */
        }
    }
\"\"\"
    pad_source.write_text(pad.replace(marker, replay + marker), encoding='utf-8')
'''
helper=helper.replace(needle,insertion+needle)
helper=helper.replace("*[str(ROOT / 'src' / name) for name in sources]",
    "*[str(pad_source if name == 'pad.c' else ROOT / 'src' / name) for name in sources]")
helper=helper.replace("'/I' + str(ROOT / 'out')", "'/I' + str(ROOT / 'out'), '/I' + str(ROOT / 'src')")
exec(compile(helper, str(ROOT/'scripts/build_hybrid_probe.py'), 'exec'), globals())
