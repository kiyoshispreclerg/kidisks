# Kidisks — notas para o Claude

Gerenciador de discos leve em C11 + GTK2: GParted, Filelight e BleachBit num
programa só, em quatro abas (Unidades, Partições, Espaço, Limpeza). Leia o
`PLANO.md` antes de começar uma fase: ele tem a arquitetura, o estado de cada
fase e as lições aprendidas.

O usuário escreve em português (pt-BR) e quer as respostas em português.
Comentários e identificadores no código, em inglês. A interface é
multi-idiomas com gettext: todo texto que uma pessoa lê (janela, modo
texto, mensagens de log) vai em `_("...")` com msgid em **inglês**
(`N_()` para tabelas, `ngettext()` para plurais), e as traduções ficam em
`po/` (por enquanto só `pt_BR`). Mexeu em texto? `make pot` e traduza o que
aparecer vazio no `po/pt_BR.po` (`tools/fill_po.py` ajuda em lote). Só o
`kd_log_debug` (do `-v`) fica sem tradução.

## Segurança: discos de verdade

**Nunca rode operações que alteram disco nesta máquina fora de loop
devices.** Formatar, particionar e redimensionar só em imagens de arquivo
ligadas com `udisksctl loop-setup`, com `KIDISKS_ONLY_LOOP=1`. O usuário
testa em discos reais num outro computador, pelo roteiro de
`docs/TESTE-MANUAL.md`. **Nunca mexa no `/etc/fstab`** (decisão do usuário).

## Compilar e testar

```sh
make                 # build/kidisks e build/locale, sem avisos (-Werror -Wpedantic)
make test            # testes de unidade (tests/unit/test_*.c)
make asan            # os testes com ASan/UBSan
make lint            # cppcheck + funções proibidas (strcpy, strcat, sprintf, vsprintf, gets)
make BUILD=build/clang CC=clang test   # também tem que passar com clang
make pot             # recria po/kidisks.pot e mescla nos .po
make test-disks      # integração em loop device (udisksctl loop-setup, sem root)
tools/check-lsblk.py # compara o --list com o lsblk desta máquina
python3 tools/gen_fixtures.py          # recria tests/fixtures/sys1
build/kidisks -v --exit-after-map      # tempos da abertura, etapa por etapa
build/kidisks --list [-b] [-a]         # o modelo de discos em texto
build/kidisks --info sda               # saúde (SMART) pelo UDisks2
build/kidisks --scan DIR -b --top 0    # total da pasta; tem que bater com du -sxB1
build/kidisks --space DIR              # janela já na aba Espaço, olhando DIR
```

- Janela em teste: num X à parte, sem mexer no desktop do usuário:
  `Xvfb :77 -screen 0 1200x800x24 &`, rode com `DISPLAY=:77
  XDG_CONFIG_HOME=<scratchpad>/cfg NO_AT_BRIDGE=1` e capture com
  `python3 -c "from PIL import ImageGrab; ImageGrab.grab(xdisplay=':77').save('x.png')"`.
  Mouse e cliques com `DISPLAY=:77 xdotool mousemove X Y click 1`. Para
  achar o processo, `pgrep -x kidisks`: o `pgrep -f kidisks` acha também o
  shell que roda o comando (e o `kill` derruba o próprio shell).
- `make asan-run` e o ASan na janela: as supressões do `tools/lsan.supp`
  citam funções do GTK, que só aparecem na pilha com
  `ASAN_OPTIONS=fast_unwind_on_malloc=0` (o `asan-run` já põe).
- O `~/.gtkrc-2.0` do usuário carrega o `appmenu-gtk-module`: a menu bar vai
  para o menu global e **não aparece na janela**. É o esperado. Para vê-la
  na captura, use um gtkrc sem a linha `gtk-modules` (`GTK2_RC_FILES=`).
- Discos desta máquina: `sda` (SSD, sistema: `/` e `/home`), `sdb` (HD),
  `sdc` (SSD Kingston, temperatura errada no libatasmart), `sdd` (leitor de
  cartão vazio), `sde` (HD USB com LUKS aberto em `/media/kiyoshi/BACKUP`).
  Só leitura neles: `--info`, `--list`. Nada de `--mount`/`--unmount` em
  disco real; para isso, loop device.
- Memória: meça a própria (`Private_Dirty` em `/proc/PID/smaps_rollup`),
  não o RSS: só as bibliotecas do GTK2 e os ícones SVG já dão ~30 MB de RSS
  compartilhado.
- Mudar `VERSION` (ou outro `-D` de `config.mk`) não força recompilação:
  `make clean` antes de conferir.

## Estrutura

| Arquivo | Papel |
|---|---|
| `src/main.c` | opções, gettext, recusa root, abre a janela |
| `src/log.c`, `src/util.c`, `src/ini.c` | vindos do kikarinhas (`kd_pathf`, tamanhos em GiB/GB, .ini editável no lugar) |
| `src/prefs.c` | o que a janela lembra (`~/.config/kidisks/kidisks.ini`) |
| `src/disks.c` | modelo de discos: sysfs + banco do udev + mountinfo + swaps + statvfs; `kd_sources` aponta para fixtures |
| `src/udisks.c` | UDisks2 por GDBus: SMART, montar, desmontar, ejetar, remover com segurança; `KIDISKS_ONLY_LOOP` |
| `src/watch.c` | avisa mudanças: udev + POLLPRI no mountinfo/swaps, com debounce |
| `src/health.c` | SMART em palavras, "4 anos e 11 meses" |
| `src/cli.c` | `--info`, `--mount`, `--unmount`, `--eject`, `--power-off`, `--scan` |
| `src/scan.c` | varredura (como o `du -x`) e árvore compacta: nós num vetor, nomes numa arena, filhos do maior para o menor |
| `src/rings.c` | layout dos anéis (ângulos, fatias pequenas agrupadas, teste de clique), sem GTK |
| `ui/app.h` | o que as abas compartilham (modelo, UDisks2, status, erros) |
| `ui/units.c` | aba Unidades: cartões, barras de uso, botões |
| `ui/space.c` | aba Espaço: lugares, trilha, anéis em Cairo (cache + destaque), menu, trabalhos em thread |
| `src/i18n.h` | `_()`, `N_()`, domínio `kidisks` |
| `ui/window.c` | janela, abas, menus, atalhos (Alt+1..4, Ctrl+Q, F5), sinais; mantém o modelo atualizado |
| `data/` | `.desktop` e ícone (achado em `../data` quando roda do `build/`) |
| `po/` | `kidisks.pot`, `LINGUAS`, `POTFILES.in`, um `.po` por idioma |
| `tools/fill_po.py` | preenche traduções vazias ou fuzzy de um `.po` a partir de um JSON (plurais como lista) |
| `tools/gen_fixtures.py` | gera `tests/fixtures/sys1` (sysfs, udev, mountinfo, swaps falsos) |
| `tools/check-lsblk.py` | compara `--list --all --bytes` com o `lsblk -b` |
| `tests/integration/test-disks.sh` | `make test-disks`: imagem → loop → montar/desmontar |
| `docs/TESTE-MANUAL.md` | roteiro para o computador de teste |

`src/` (menos `main.c`) vira `build/libkd.a`, sem GTK (GLib, GIO e libudev
sim), que é o que os testes linkam. Código que precisa do GTK vai em `ui/`.
Fonte da interface: poucas variações (negrito, `<big>`, `<small>`); cada
nova custa um `FcFontSort` sobre todas as fontes instaladas (2455 aqui).

## Convenções

- Prefixo `kd_` em tudo que é público; um módulo por arquivo, cabeçalho com
  comentário explicando o porquê e de quem é a memória.
- Nada bloqueia a interface: D-Bus assíncrono; varredura e limpeza em
  thread, resultado pelo `g_idle_add`.
- Caminhos com `kd_pathf` (falha em vez de truncar).
- Tamanhos: `kd_format_size` (GiB, como o `lsblk`); discos inteiros com
  `kd_format_disk_size` ("111.8 GiB (120 GB)").
- `README.md` (pt-BR, padrão) e `README.en.md` andam juntos: mudou um, mude
  o outro.
- Licença GPL-3.0-or-later; `SPDX-License-Identifier` no topo de cada
  arquivo.
- Commits em português, terminando com a linha Co-Authored-By.
