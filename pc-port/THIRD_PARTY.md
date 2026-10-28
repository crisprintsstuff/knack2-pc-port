# Proveniência

`scripts/build_gpu.py` compila fontes do renderer Vulkan, shader recompiler,
GnmDriver, VideoOut, AJM e Videodec de `../research/shadPS4-perf`, com shims em
`gpu/` e sem o main/loader do emulador. Essa integração permanece sob GPL-2.0-or-later.
Alguns shims foram adaptados da referência Bloodborne citada abaixo. As bibliotecas
de terceiros (incluindo SDL, Vulkan, ImGui, FFmpeg e fmt) permanecem nas árvores
locais existentes, com seus respectivos avisos e licenças; não foram relicenciadas.

Este protótipo adota GPL-2.0-or-later; o texto completo está em `LICENSE`.

`src/pad.c` usa a API SDL3 e a convenção de integração quaternion de
`shadPS4/src/input/controller.cpp`. `src/save_dialog.c` adapta layouts e estados
de `shadPS4/src/core/libraries/save_data/dialog/`, com UI Windows própria.
As fontes do PhonePad do usuário foram consultadas para identificar o DS4 virtual;
nenhum código nem credenciais do Companion foi copiado ou modificado.

`scripts/ps4_format.py` adapta as funções de leitura SELF/SFO/NID de
[`deadinside28/bloodborne_pc/scripts/prepare.py`](https://github.com/deadinside28/bloodborne_pc/blob/4d4d20812c0af23a11c8b8d40f5e06a9054925ef/scripts/prepare.py),
snapshot `4d4d20812c0af23a11c8b8d40f5e06a9054925ef`, GPL-2.0-or-later.
Mantivemos os limites e verificações de segmentos; removemos o import de `game_check`
e o restante do preparador específico de Bloodborne. A cópia de referência local
está em `../research/bloodborne_pc`; não é o checkout de desenvolvimento do nosso port.

O formato de identidade NID/biblioteca/módulo foi conferido com
[`scripts/link_libc.py`](https://github.com/deadinside28/bloodborne_pc/blob/4d4d20812c0af23a11c8b8d40f5e06a9054925ef/scripts/link_libc.py)
e com `../research/shadPS4-perf/src/core/module.cpp` e `loader/elf.h`.
O formato binário `ISSPRB01` e o loader Windows deste projeto são próprios.
`ISSPRB02` adiciona a tabela de módulos/TLS e bindings. `scripts/link_modules.py`
usa o projeto citado como referência de organização, mas resolve identidades e
versões exatas e não copia o patch FS→GS usado no Linux.

`src/runtime.c` utiliza Zydis/Zycore (MIT) já presentes/compilados no checkout
local de shadPS4 para decodificar instruções TLS realmente executadas. O layout
TCB/DTV foi conferido em `src/core/tls.h`/`linker.cpp` de shadPS4 e em
`bloodborne_pc/src/runtime_thread.c`. O contrato/layout de `sceLibcHeapGetTraceInfo`
foi adaptado de `src/core/libraries/libc_internal/libc_internal_memory.cpp`,
Copyright 2024 shadPS4 Emulator Project, GPL-2.0-or-later.
O checkout local tem base `584374867d6a655d911a26fe06145f3a7cb2e89d` e alterações anteriores.

O inventário lê a tabela AeroLib local de shadPS4 apenas para fornecer nomes
indicativos aos NIDs. O relatório registra seu caminho/hash. A tabela não está
copiada neste projeto e nomes isolados não implementam nem validam uma API.

Não há executáveis nem assets do jogo no código do projeto. `out/boot.bin` é gerado
localmente a partir do dump do usuário, contém código/dados do jogo e está ignorado
pelo Git. Não publicar `out/` nem os dumps existentes no workspace.
