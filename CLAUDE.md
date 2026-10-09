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
build/kidisks -v --exit-after-map      # mede o tempo até a janela aparecer
```

- Janela em teste: num X à parte, sem mexer no desktop do usuário:
  `Xvfb :77 -screen 0 1200x800x24 &`, rode com `DISPLAY=:77
  XDG_CONFIG_HOME=<scratchpad>/cfg NO_AT_BRIDGE=1` e capture com
  `python3 -c "from PIL import ImageGrab; ImageGrab.grab(xdisplay=':77').save('x.png')"`.
- O `~/.gtkrc-2.0` do usuário carrega o `appmenu-gtk-module`: a menu bar vai
  para o menu global e **não aparece na janela**. É o esperado. Para vê-la
  na captura, use um gtkrc sem a linha `gtk-modules` (`GTK2_RC_FILES=`).
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
| `src/i18n.h` | `_()`, `N_()`, domínio `kidisks` |
| `ui/window.c` | janela, abas, menus, atalhos (Alt+1..4, Ctrl+Q), sinais |
| `data/` | `.desktop` e ícone (achado em `../data` quando roda do `build/`) |
| `po/` | `kidisks.pot`, `LINGUAS`, `POTFILES.in`, um `.po` por idioma |
| `tools/fill_po.py` | preenche traduções vazias de um `.po` a partir de um JSON |

`src/` (menos `main.c`) vira `build/libkd.a`, sem GTK, que é o que os
testes linkam. Código que precisa do GTK vai em `ui/`.

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
