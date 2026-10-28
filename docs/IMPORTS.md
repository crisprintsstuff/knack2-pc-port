# Measured import inventory

Generated 2026-10-28 from the decrypted `CUSA08014` dump on the user's own machine. Every number
here was read out of the binaries' dynamic symbol tables; none of it is inferred. The parser is
the same shape as `pc-port/scripts/prepare.py` in got-pc-port.

Two images, because this game boots in two stages: `eboot.bin` (37,006 B, sha256
`e65b5fd0…3a4041db`) is a loader; `game.elf` (20,205,618 B, sha256 `80ccc228…d573fb20`) is the
game, built as `MGame_Master_ORBIS.elf`.

A NID is the hashed name of a PS4 export, so the count is what matters here, not the names.

| Library | `eboot.bin` | `game.elf` | Total |
| --- | ---: | ---: | ---: |
| `libc` | 59 | 192 | 251 |
| `libSceGnmDriver` | — | 113 | 113 |
| `libkernel` | 2 | 82 | 84 |
| `libSceFios2` | — | 43 | 43 |
| `libSceJson2` | — | 40 | 40 |
| `libSceFont` | — | 34 | 34 |
| `libSceNet` | — | 33 | 33 |
| `libSceNpScore` | — | 19 | 19 |
| `libSceAvPlayer` | — | 17 | 17 |
| `libSceVideoOut` | — | 14 | 14 |
| `libSceHttp` | — | 12 | 12 |
| `libSceAudio3d` | — | 11 | 11 |
| `libSceNgs2` | — | 11 | 11 |
| `libSceNpManager` | — | 11 | 11 |
| `libScePlayGo` | — | 11 | 11 |
| `libSceSaveData` | — | 11 | 11 |
| `libScePad` | — | 10 | 10 |
| `libSceNpTus` | — | 9 | 9 |
| `libSceAudioOut` | — | 8 | 8 |
| `libSceNpWebApi` | — | 8 | 8 |
| `libSceJobManager` | — | 7 | 7 |
| `libSceNpTrophy` | — | 7 | 7 |
| `libSceFiber` | — | 6 | 6 |
| `libSceSystemService` | 1 | 5 | 6 |
| `libSceUserService` | — | 6 | 6 |
| `libSceMsgDialog` | — | 5 | 5 |
| `libScePngDec` | — | 5 | 5 |
| `libSceSaveDataDialog` | — | 5 | 5 |
| `libSceErrorDialog` | — | 4 | 4 |
| `libSceIme` | — | 4 | 4 |
| `libSceMouse` | — | 4 | 4 |
| `libSceNetCtl` | — | 4 | 4 |
| `libSceRtc` | — | 4 | 4 |
| `libSceAppContent` | — | 3 | 3 |
| `libSceGameLiveStreaming` | — | 3 | 3 |
| `libSceSharePlay` | — | 3 | 3 |
| `libSceSysmodule` | — | 3 | 3 |
| `libSceCoredump` | — | 2 | 2 |
| `libSceFontFt` | — | 2 | 2 |
| `libSceScreenShot` | — | 2 | 2 |
| `libSceSsl` | — | 2 | 2 |
| `libSceCommonDialog` | — | 1 | 1 |
| `libScePosix` | — | 1 | 1 |
| `libSceVideoRecording` | — | 1 | 1 |
| **total** | **62** | **778** | **840** |

`game.elf` declares 48 modules in `DT_NEEDED` but resolves imports against 43 libraries; the
difference is the modules it loads only for their presence (`libSceFont-module.prx`,
`libSceFontFt-module.prx`, `libSceNpScoreRanking.prx`, `libSceContentExport.prx`,
`libSceCompanionUtil.prx`).
