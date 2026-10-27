# Kidisks — plano

Gerenciador de discos leve que junta três programas num só, em quatro abas:

1. **Unidades:** visão geral de todos os discos e partições: modelo,
   tamanho, tipo (SSD, HD, USB, NVMe), saúde, temperatura e quanto de cada
   partição está ocupado.
2. **Partições** (como o GParted): criar, excluir, formatar, redimensionar,
   rótulo, flags, nova tabela, montar e desmontar.
3. **Espaço** (como o Filelight): quanto cada pasta ocupa, em círculos
   concêntricos.
4. **Limpeza** (como o BleachBit): apagar caches, lixos e rastros de vários
   programas, com definições próprias que vão crescendo a cada versão.

Mesma filosofia do KiDesktop, do kikarinhas e do kisnitch: C11 + GTK2, abrir
na hora, pouca memória, nada de Python, Qt ou runtime pesado. Licença
GPL-3.0-or-later, como os outros.

Fora do escopo: editar o `/etc/fstab` (nunca; decisão do usuário). Por
enquanto, também: mover e copiar partições (o GParted faz isso
bloco a bloco por conta própria, e um erro destrói dados), editar RAID/LVM,
Windows, "apagamento seguro" de arquivos (shred não adianta nada em SSD),
backup ou clonagem de disco.

## Metas de desempenho

| Medida | Meta |
|---|---|
| Abrir a janela com os discos listados | < 200 ms (aqui; ver abaixo) |
| Memória própria parada (aba Unidades; `Private_Dirty`) | < 15 MB |
| Varrer `/home` com 1 milhão de arquivos (cache quente) | no máximo 1,5× o tempo do `du -sx` |
| Memória por arquivo varrido | ≤ 48 bytes + o nome |
| Redesenhar o gráfico de círculos | < 16 ms (60 fps no hover) |

O RSS não serve de meta: uma janela GTK2 vazia já marca ~25 MB, quase
tudo bibliotecas compartilhadas com os outros programas (GTK, librsvg dos
ícones SVG, fontes). Na fase 0 o Kidisks marcou 38 MB de RSS e 6,8 MB de
memória própria; na fase 1, com a aba Unidades, 42 MB e 9 MB.

A abertura era 150 ms na fase 0 (janela vazia: 105–130 ms). Com a aba
Unidades cheia ficou em 160–200 ms nesta máquina, e o tempo não é do
Kidisks: ler os discos leva 2 ms; o resto é o GTK2 medindo textos, e quase
tudo dentro disso é o fontconfig ordenando as 2455 fontes instaladas aqui
(`FcFontSetSort`), uma vez por variação de fonte usada (negrito, tamanho,
itálico). Por isso a aba usa poucas variações. Num sistema com poucas
fontes a abertura cai bastante.

## Arquitetura

```
kidisks                 (C11 + GTK2; roda como usuário comum)
 ├─ Unidades e Partições ─ leitura: sysfs + banco do udev (libudev),
 │                         statvfs, /proc/self/mountinfo, /proc/swaps
 │                       ├─ monitor do udev no laço do GTK (pendrive entrou/saiu)
 │                       └─ alterações e SMART: UDisks2 por GDBus
 │                            └─ polkit pede a senha → udisksd (root) executa
 ├─ Espaço ─── varredura numa thread (openat/fstatat), árvore compacta
 │             └─ gráfico de anéis em Cairo numa GtkDrawingArea
 └─ Limpeza ── definições .ini próprias (data/cleaners + ~/.local/share/kidisks/cleaners)
               └─ prévia (lista e total) → limpar
kidisks-helper          (fase 7; pequeno, via pkexec, só ações fixas de sistema:
                         apt clean, journald vacuum, /var/tmp, /var/crash)
```

### Privilégios: polkit + UDisks2

Editar partições precisa de root, sem discussão. O que muda é **quem** roda
como root:

- o `kidisks` (a interface GTK) roda sempre como usuário comum. GTK como
  root é má ideia (e no Wayland nem abre). Se for aberto com `sudo`, avisa e
  sai;
- para alterar um disco, o `kidisks` pede ao `udisksd`, que já roda como
  root no sistema. O polkit pede a senha do administrador (a janelinha de
  senha do KDE) e, liberado, o `udisksd` executa `mkfs`, `sfdisk`,
  `resize2fs` etc.;
- assim não temos código root nosso nas operações de disco. As ações do
  polkit são as do próprio UDisks2 (`org.freedesktop.udisks2.modify-device`,
  `...filesystem-mount-system` etc.), sem política nossa até a fase 7.

O GTK2 já traz a GLib, e com ela a GIO/GDBus: falar com o UDisks2 não
acrescenta dependência. O UDisks 2.9 (Ubuntu 22.04) já faz: tabela nova
(`Block.Format` com `dos`/`gpt`), `PartitionTable.CreatePartition`,
`Partition.Delete/Resize/SetType/SetName/SetFlags`, `Block.Format` (ext4,
vfat, ntfs, exfat, btrfs, xfs, swap... o que tiver `mkfs.*` instalado),
`Filesystem.Mount/Unmount/SetLabel/Resize/Check/Repair`,
`Drive.Eject/PowerOff`, `Drive.Ata` (SMART), LUKS e `Manager.LoopSetup`.

Sem o `udisksd` rodando, o Kidisks abre só para leitura e diz por quê.

### Princípios

- **Leitura sem senha.** Partições, início e tamanho vêm do sysfs
  (`/sys/block/sda/sda1/{start,size}`). Sistema de arquivos, rótulo, UUID e
  tipo da partição vêm do banco do udev (`ID_FS_TYPE`, `ID_FS_LABEL`,
  `ID_PART_ENTRY_TYPE`, `ID_PART_TABLE_TYPE`...), que qualquer usuário lê.
  Espaço livre são os buracos entre partições. Usado e livre das partições
  montadas vêm do `statvfs`; das desmontadas fica "?" até a fase 8.
- **Um modelo, várias vistas.** `kd_disks` monta a lista de discos e
  partições a partir do sistema; as abas Unidades e Partições só desenham.
  O mesmo modelo serve ao modo texto (`kidisks --list`), que é o que os
  testes usam.
- **Nada bloqueia a interface.** Chamadas ao UDisks2 são assíncronas (GDBus
  `_call` + callback). A varredura e a limpeza rodam numa thread que só
  entrega progresso e resultado pelo `g_idle_add`, com botão de cancelar.
- **Segurança nas operações destrutivas:**
  - nada destrutivo em partição montada, swap ativa ou membro de LVM/RAID/LUKS
    aberto. O diálogo oferece "Desmontar e continuar";
  - o disco do `/` e do `/home` ganha aviso extra;
  - toda alteração entra na fila de pendentes, com descrição em texto, e só é
    aplicada no "Aplicar", com confirmação que lista tudo o que será feito;
  - com `KIDISKS_ONLY_LOOP=1`, qualquer alvo que não seja loop device é
    recusado no próprio código, antes de chegar ao D-Bus. Os testes sempre
    rodam assim.
- **Limpeza que não escapa do lugar:**
  - nunca segue links simbólicos (`openat` com `O_NOFOLLOW`, `unlinkat`
    relativo ao diretório já aberto);
  - só aceita as variáveis conhecidas (`~`, `$XDG_CACHE_HOME`,
    `$XDG_CONFIG_HOME`, `$XDG_DATA_HOME`, `$XDG_RUNTIME_DIR`);
  - uma definição que aponte para `/`, para o `$HOME` inteiro ou para fora
    das raízes permitidas é recusada ao carregar;
  - a prévia é obrigatória antes de apagar, e o que foi apagado fica
    registrado em `~/.local/state/kidisks/limpeza.log`.

### Dependências

| Biblioteca | Uso |
|---|---|
| GTK2 (traz GLib/GIO/GDBus, Cairo, Pango) | interface, gráfico de anéis, UDisks2, lixeira (`g_file_trash`) |
| libudev | enumerar discos, ler o banco do udev, avisos de dispositivo entrando/saindo |
| pthreads | threads da varredura e da limpeza |
| (em tempo de execução) udisks2, polkit, `mkfs.*` | operações de disco |

Build com Makefile + `config.mk` + pkg-config, no mesmo esquema do
kikarinhas: `-std=c11 -Wall -Wextra -Werror -Wpedantic`, cabeçalhos das libs
como `-isystem`, `make test`, `make asan`, `make lint`.

Vêm do kikarinhas, com o prefixo trocado para `kd_`: `log.c`, `util.c`
(`kd_pathf` etc.), `ini.c` e `tests/harness.h` (e, na fase 8,
`tools/build-deb.sh`).

### Idiomas e tamanhos

- Interface multi-idiomas com gettext desde a fase 0: msgids em inglês,
  traduções em `po/` (começa com `pt_BR`). Vale para a janela, o modo texto
  e as mensagens de log; só o `-v` de depuração fica sem tradução.
- Tamanhos em unidades binárias, arredondados como no `lsblk` ("46.8 GiB",
  "290 MiB"). Discos inteiros mostram também o tamanho decimal do
  fabricante: "111.8 GiB (120 GB)". O separador decimal segue o idioma.

## As quatro abas

### 1. Unidades

Uma "ficha" por disco, rolável, atualizada sozinha quando algo entra, sai ou
é montado:

```
┌──────────────────────────────────────────────────────────────────────┐
│ [SSD] SanDisk SSD PLUS 120 GB      sda · SATA · 111,8 GiB · MBR       │
│       Saúde: boa · 34 °C · ligado há 1 ano e 3 meses                  │
│   sda1  ext4  root-ssd   /        ▓▓▓▓▓▓▓▓▓▓░░░░  31,2 de 46,8 GiB   │
│   sda2  ext4  home       /home    ▓▓▓▓▓▓▓░░░░░░░  27,0 de 59,5 GiB   │
│   sda3  vfat             —        (desmontada)        290 MiB        │
├──────────────────────────────────────────────────────────────────────┤
│ [USB] ...                                         [Ejetar] [Desligar] │
└──────────────────────────────────────────────────────────────────────┘
```

- Tipo pelo sysfs (`queue/rotational`, transporte do udev `ID_BUS`/
  `ID_USB_DRIVER`, `nvme*`, `mmcblk*`).
- Saúde e temperatura pelo `Drive.Ata` do UDisks2 (`SmartFailing`,
  `SmartTemperature`, `SmartPowerOnSeconds`, setores realocados), lidos sem
  senha. NVMe só tem SMART pelo UDisks 2.10+: no 2.9 aparece "sem dados".
- Barra colorida por ocupação (verde → amarelo → vermelho acima de 90%).
- Clique numa partição montada: abrir no gerenciador de arquivos, ou
  "Ver espaço" (abre a aba Espaço nela). Clique no disco: aba Partições
  nele.
- Botões: montar e desmontar (imediatos), ejetar e desligar (removíveis).
- Mostra discos, partições, LUKS, LVM e swap. Esconde loop de snap, zram e
  ram (opção para mostrar).

### 2. Partições (GParted)

```
┌ [sda  SanDisk SSD PLUS 120 GB ▾]                 [Aplicar] [Desfazer]  ┐
│ ┌──────────────── sda1 ext4 ───────────┬──── sda2 ext4 ──────┬ sda3 ┐  │
│ │ / ▓▓▓▓▓▓▓▓▓▓▓▓▓░░░░░░░░░ 46,8 GiB    │ /home ▓▓▓▓▓░░ 59,5  │ vfat │  │
│ └──────────────────────────────────────┴─────────────────────┴──────┘  │
│ Partição  Sistema  Rótulo    Tamanho   Usado   Livre   Flags  Montado │
│ sda1      ext4     root-ssd  46,8 GiB  ...                     /       │
│ ...                                                                    │
│ Pendentes: 1. Formatar sdc2 como ext4 (rótulo "dados")                 │
└────────────────────────────────────────────────────────────────────────┘
```

- Barra proporcional (largura mínima para partição pequena), cor por sistema
  de arquivos (paleta parecida com a do GParted), parte usada mais escura.
  A barra e a lista mostram o estado **depois** das pendentes, como no
  GParted.
- Menu de contexto e barra de ferramentas: Nova, Excluir, Formatar como,
  Redimensionar, Rótulo, Flags, Verificar, Montar/Desmontar, Nova tabela,
  Propriedades.
- Propriedades: modelo, série, tamanho lógico e físico do setor,
  alinhamento, tipo e UUID da tabela e da partição, opções de montagem.

### 3. Espaço (Filelight)

- Escolha de raiz: pontos de montagem (botões, como no Filelight), pasta
  pessoal ou "Escolher pasta...".
- Varredura: `openat` + `fdopendir` + `fstatat(AT_SYMLINK_NOFOLLOW)`. Conta
  `st_blocks * 512`, que é o tamanho real em disco, como o `du`. Não
  atravessa sistemas de arquivos (`st_dev`). Conta hardlinks uma vez só
  (tabela `(dev, ino)` só para `st_nlink > 1`). Pastas sem permissão
  aparecem marcadas. Mais tarde, se medido que compensa: várias threads
  (como o gdu).
- Árvore compacta: nós num vetor (índices em vez de ponteiros), nomes numa
  arena de strings, filhos ordenados por tamanho.
- Gráfico:
  - centro com a pasta atual e o total; até 5 anéis; ângulo proporcional ao
    tamanho;
  - cor por fatia do primeiro nível (matiz) e por profundidade (claridade),
    arquivos em cinza;
  - fatias menores que ~1° viram um "N arquivos pequenos";
  - teste de clique em coordenadas polares.
- Interação:
  - passar o mouse mostra nome e tamanho;
  - clique entra na pasta; clique no centro ou na barra de caminho volta;
  - botão direito: abrir no gerenciador de arquivos, abrir terminal aqui,
    copiar caminho, mandar para a lixeira, apagar;
  - "Atualizar" revarre só a pasta atual.
- Fundo em cache (superfície Cairo); o hover só redesenha o destaque.

### 4. Limpeza (BleachBit)

Definições próprias em .ini, lidas pelo mesmo leitor do kikarinhas, que
guarda chaves repetidas em ordem. **Não lemos os arquivos do BleachBit em
tempo de execução.** Usamos os CleanerML dele
(`/usr/share/bleachbit/cleaners/*.xml`) como referência de onde cada
programa guarda cache e rastros, e escrevemos os nossos arquivos à mão.
Como o BleachBit também é GPL-3.0+, um arquivo que copie listas de caminhos
dele leva no cabeçalho a linha `Baseado em <arquivo>.xml do BleachBit,
Copyright (C) 2008-2021 Andrew Ziem`.

```ini
# data/cleaners/thumbnails.ini
[cleaner]
id = thumbnails
name = Miniaturas
description = Imagens em miniatura geradas pelos gerenciadores de arquivos

[option.cache]
name = Cache
description = Apaga as miniaturas; elas são refeitas quando necessário
default = true
delete = walk.files ~/.cache/thumbnails/
delete = walk.files ~/.thumbnails/
```

- Buscas, com a mesma semântica do CleanerML para facilitar o porte:
  - `file`: um arquivo;
  - `glob`;
  - `walk.files`: os arquivos de uma árvore, mantendo as pastas;
  - `walk.all`: tudo, inclusive as pastas;
  - `walk.top`: o conteúdo e a própria pasta.

  Filtros: `older_than = 7d` e `regex`. Isso cobre a grande maioria do que o
  BleachBit faz no Linux: 672 das ~850 ações dele são de apagar. O resto
  (limpar bancos SQLite por dentro, editar JSON de navegador) fica de fora
  até alguém precisar.
- `running = firefox` avisa e pula a opção se o programa estiver aberto
  (procura em `/proc/*/comm`).
- `warning =` mostra um aviso ao marcar (ex.: "apaga senhas salvas").
- `root = true` marca as que precisam do helper (fase 7).
- Lidas de `data/cleaners/` (instaladas) e de
  `~/.local/share/kidisks/cleaners/` (do usuário; a mesma `id` sobrescreve).
- Interface:
  - árvore com caixas de marcar (limpador → opções), com o tamanho de cada
    uma depois da prévia;
  - botões "Prévia" e "Limpar"; a lista do que será apagado mostra o
    tamanho de cada item e o total no fim;
  - as marcações são lembradas em `~/.config/kidisks/kidisks.ini`.
    Cada gravação é atômica (tmp + fsync + rename) e guarda a versão
    anterior em `kidisks.ini.backup` (só se ela estiver íntegra: sem NUL e
    sem nenhum aviso de leitura). Se o `.ini` sumir ou ficar vazio (queda de
    energia), a leitura usa o `.backup`. Para "resetar" apagando o arquivo,
    apague o `.backup` também, senão ele volta.

Primeiro lote (fase 5), pensado num desktop como o daqui (Plasma 5,
Firefox, Chromium/Brave, Steam):

| Limpador | Opções |
|---|---|
| Sistema (usuário) | lixeira, miniaturas, documentos recentes (`recently-used.xbel`, `RecentDocuments` do KDE), arquivos temporários do usuário |
| Firefox | cache, cookies*, histórico*, sessão* |
| Chromium, Brave, Google Chrome | cache, cache de código/GPU, cookies*, histórico* |
| KDE | cache do Plasma/KIO, miniaturas do Gwenview, `ksycoca` |
| Desenvolvimento | caches do pip, npm, cargo (`registry/cache`), ccache, Go |
| Steam | shader cache, logs, `htmlcache` |
| Flatpak / Snap | cache de apps do usuário |
| Kikarinhas | cache de emotes |

\* desmarcadas por padrão, com aviso.

## Testes

Só virtuais nesta máquina. **Nenhum teste toca em disco real.**

- **Modelo de discos:** sysfs e banco do udev falsos em `tests/fixtures/`
  (copiados da estrutura real, com números inventados): sda MBR, NVMe GPT,
  pendrive sem tabela, disco vazio, LUKS, setor de 4 KiB.
- **Operações de disco:** imagens de arquivo no scratchpad →
  `udisksctl loop-setup` (sem root, pelo polkit) → `KIDISKS_ONLY_LOOP=1` →
  operações pelo `kidisks` → conferência com `sfdisk -d`, `blkid` e
  `fsck -n` → `udisksctl loop-delete`. Um script em `tools/` cria e desfaz
  os loops, e o `make test-disks` roda tudo.
- **Interface:** num X à parte (`Xvfb :77`), sem mexer no desktop do usuário,
  com captura de tela, como no kikarinhas.
- **Limpeza:** `$HOME` e `XDG_*` falsos no scratchpad.
- **Máquina real:** formatações e redimensionamentos de verdade são testados
  por você, num outro computador sem dados a perder. Cada fase de disco
  termina com um roteiro de teste manual (`docs/TESTE-MANUAL.md`).

## Fases

Cada fase termina com `make`, `make test` e `make asan` limpos, e com o
`PLANO.md` atualizado (estado e lições aprendidas).

### Fase 0 — esqueleto (feita)

- Makefile, `config.mk`, `tests/harness.h`, `make lint`, `.desktop`, ícone.
- `log.c`, `util.c` e `ini.c` trazidos do kikarinhas.
- Janela GTK2 com as quatro abas vazias, barra de status e "Sobre". A
  última aba usada é lembrada.
- Recusa rodar como root.
- `CLAUDE.md` no formato dos outros projetos.

**Aceite:** `build/kidisks` abre em < 150 ms e `make test` roda um teste do
ini.

**Estado:** feita. Janela com as quatro abas (marcadores do que cada uma
vai fazer), menus Arquivo/Ajuda, Sobre, Alt+1..4, Ctrl+Q, `-t ABA`,
`-c ARQ`, `-v`. Lembra aba, tamanho e maximizado, inclusive ao receber
SIGTERM/SIGINT/SIGHUP. Recusa root. Traduções pt_BR completas. 11 testes
(ini, prefs, util/tamanhos) passando com gcc, clang e ASan; `make lint`
limpo. Janela desenhada em 105–130 ms no Xvfb; 6,8 MB de memória própria.

Lições:
- O `~/.gtkrc-2.0` do usuário carrega o `appmenu-gtk-module`: a menu bar
  vai para o menu global e some da janela. É o esperado; em captura de
  tela, use um gtkrc sem `gtk-modules`.
- `g_signal_handlers_disconnect_by_func` não passa no `-Wpedantic`
  (ponteiro de função para `gpointer`): guarde o id do sinal.
- O clang pede `__attribute__((format(printf, N, 0)))` em funções que
  repassam `va_list` para `vfprintf`.

### Fase 1 — modelo de discos e aba Unidades

- `kd_disks`: enumeração (sysfs + udev), partições, buracos livres, tabela
  (dos/gpt), uso por `statvfs`, montagem por `/proc/self/mountinfo`, swap
  por `/proc/swaps`, holders (LUKS/LVM por `/sys/block/*/holders`).
- Monitor do udev ligado ao laço do GTK (`g_io_add_watch` no fd).
- SMART (só leitura) pelo `Drive.Ata` do UDisks2.
- Aba Unidades completa, com montar, desmontar, ejetar e desligar.
- `kidisks --list`: o modelo em texto.

**Aceite:** o `--list` bate com o
`lsblk -b -o NAME,START,SIZE,FSTYPE,LABEL,MOUNTPOINT` nesta máquina. Testes
com fixtures. Um loop device montado e desmontado pelo `kidisks` atualiza a
aba sozinho.

**Estado:** feita.

- `src/disks.c`: o modelo, lido do sysfs, do banco do udev (o arquivo
  `/run/udev/data/b<maj>:<min>` direto, sem libudev, para os testes
  apontarem tudo para fixtures), `mountinfo`, `swaps` e `statvfs`.
  Partições, lacunas livres (GPT reserva o fim; setor de 4 KiB), estendida e
  lógicas, holders (LUKS, LVM, RAID), HD/SSD, disco do sistema.
- `src/udisks.c`: GDBus assíncrono; SMART (`Drive.Ata`; `NVMe.Controller`
  quando o UDisks for 2.10+), montar, desmontar, ejetar, "remover com
  segurança" (desmonta tudo, tranca o LUKS, desliga), trava
  `KIDISKS_ONLY_LOOP`.
- `src/watch.c`: monitor do udev + `POLLPRI` no `mountinfo` e no `swaps`,
  com debounce de 250 ms.
- `ui/units.c`: um cartão por disco (modelo, detalhes, saúde, temperatura,
  tempo ligado), uma linha por partição e volume (sistema de arquivos,
  rótulo, ponto de montagem, barra de uso verde/âmbar/vermelha, montar,
  desmontar, abrir pasta), colunas alinhadas entre os cartões. Números
  atualizados a cada 5 s só com a aba visível; reconstrução só quando o
  layout muda (`kd_disks_layout_key`).
- Linha de comando: `--list [-b] [-a]`, `--info`, `--mount`, `--unmount`,
  `--eject`, `--power-off`.
- Testes: 10 do modelo (fixtures geradas por `tools/gen_fixtures.py`), 2
  da saúde; `tools/check-lsblk.py` compara com o `lsblk` (14 dispositivos
  aqui, 0 diferenças); `make test-disks` monta e desmonta partições de uma
  imagem em loop device. Tudo passa com gcc, clang e ASan.
- Ficou para a fase de cada um: "Ver espaço" (fase 4) e "clique no disco
  abre a aba Partições" (fase 2).

Lições:
- **Temperatura errada do libatasmart:** o UDisks2 tira a temperatura do
  libatasmart, que em SSDs Kingston/Phison lê o atributo 231 ("vida
  restante") como temperatura: 96 °C num SSD a 36 °C. O Kidisks pede os
  atributos (`SmartGetAttributes`, sem senha) e prefere o 194 (ou o 190);
  leituras fora de -20..110 °C viram "sem dado".
- **Pontes USB:** quase todas dizem ao kernel que são rotacionais, até
  pendrives. HD/SSD só por fonte confiável: RPM do ATA, NVMe, cartão SD ou o
  `RotationRate` do UDisks2.
- **Partição estendida:** o sysfs dá 1 KiB para ela; o tamanho real vem do
  `ID_PART_ENTRY_SIZE` do udev. Cada lógica tem o EBR 1 MiB antes:
  lacunas de até 1 MiB (inclusive) não são espaço livre.
- **btrfs:** no `mountinfo` aparece com `maj:min` anônimo (`0:xx`), então
  a montagem é achada também pela origem (`/dev/mapper/...`).
- **polkit:** sem `G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION` nas
  chamadas, o polkit nem pergunta a senha.
- **`udisksctl loop-setup`:** funciona sem senha para o usuário da sessão,
  o que deixa os testes de disco sem root.
- **GTK2:** uma `GtkDrawingArea` tem `GdkWindow` própria, então desenha a
  partir de 0,0 (não da alocação). O `qsort` com 0 elementos e ponteiro
  NULL é comportamento indefinido, e o UBSan pega.
- **Medir tempo:** o `perf` está bloqueado aqui (`perf_event_paranoid` = 4);
  o callgrind serve.

### Fase 2 — aba Partições, só leitura

- Barra gráfica, lista, propriedades, seletor de disco.
- Estrutura da fila de pendentes já aplicada ao modelo em memória (o
  "depois"), ainda sem executar nada: dá para montar a fila, ver o
  resultado e desfazer.

**Aceite:** fila de pendentes testada só em memória (criar, excluir e
formatar em sequência e conferir o layout resultante).

### Fase 3 — operações de disco (UDisks2)

- `kd_udisks`: proxies GDBus, chamadas assíncronas, erros do polkit e do
  `udisksd` traduzidos para o português.
- Aplicar a fila: nova tabela, criar, excluir, formatar, rótulo, flags e
  tipo, em ordem; para no primeiro erro e mostra o que foi feito e o que
  não foi.
- Diálogos:
  - nova partição: tamanho, espaço antes e depois em MiB, alinhamento de
    1 MiB, sistema de arquivos, rótulo;
  - formatar: só lista os `mkfs.*` instalados.

**Aceite:** `make test-disks` em loop devices.
`docs/TESTE-MANUAL.md` com o roteiro para o computador de teste.

### Fase 4 — Espaço (anéis) (feita, antes das fases 2 e 3)

- `kd_scan` (thread, progresso, cancelar) e a árvore compacta.
- `kd_rings`: layout dos anéis separado do desenho, testável sem GTK.
- Aba Espaço com navegação, hover, menu de contexto e lixeira.
- `kidisks --scan PASTA [--top N]`.

**Aceite:** totais iguais aos do `du -sxB1` em árvores de teste (com
hardlinks, links simbólicos, pastas sem permissão e nomes com bytes
inválidos em UTF-8). Metas de tempo e memória medidas em `docs/BENCH.md`
contra o `du` e o Filelight.

**Estado:** feita, antes das fases 2 e 3, a pedido.

- `src/scan.c`: `kd_tree_scan` (bloqueante, sem nada global, roda em
  thread; progresso e cancelamento por atômicos). `openat` + `fdopendir` +
  `fstatat(AT_SYMLINK_NOFOLLOW)`, `st_blocks * 512`, não sai do sistema de
  arquivos (`st_dev`; a pasta de montagem fica com tamanho 0, como no
  `du -x`), hardlink contado uma vez (conjunto de inodes só para
  `st_nlink > 1`). Lê todas as entradas de uma pasta antes de entrar nas
  subpastas, então os filhos de cada pasta ficam juntos num vetor de
  índices (`kids`), ordenados do maior para o menor. Nó de 32 bytes + 4 do
  índice + o nome numa arena. Ids estáveis: `kd_tree_graft` ("olhar de
  novo" uma pasta) e `kd_tree_remove` (lixeira, apagar) só acrescentam ou
  tiram do pai.
- `src/rings.c`: layout anel por anel (até 5), ângulo proporcional ao
  tamanho em relação à pasta do centro, fatias abaixo de 1° agrupadas em
  "N itens pequenos", matiz da fatia do primeiro anel; geometria e teste
  de clique em coordenadas polares.
- `ui/space.c`: botões dos lugares (pasta pessoal, cada sistema de
  arquivos montado, "Escolher pasta…"), trilha de pastas, progresso com
  cancelar, anéis em Cairo com fundo em cache e só o destaque redesenhado
  no hover, nomes do primeiro anel do lado de fora com linha-guia,
  menu de contexto (gerenciador de arquivos — num arquivo, com ele
  selecionado pelo `org.freedesktop.FileManager1` —, terminal, copiar
  caminho, lixeira, apagar com confirmação). Um trabalho por vez em
  thread (varredura, lixeira ou apagar), conferido por um timer de 100 ms;
  só a thread do GTK mexe na árvore.
- `kd_remove_tree` (`src/util.c`): o `rm -r` do "Apagar", que não segue
  links e não entra em outro sistema de arquivos montado dentro; a fase 5
  vai usar.
- Aba Unidades: botão "Ver o que ocupa espaço aqui" em cada partição
  montada. Linha de comando: `--scan PASTA [--top N] [-b]` e
  `--space PASTA` (abre a janela na aba Espaço).
- Testes: 8 da varredura (total igual ao `du -sxB1` com hardlink, link
  para fora, pasta sem permissão, nome com bytes inválidos; graft, remove,
  cancelar, erros, `kd_remove_tree` sem seguir link) e 4 dos anéis.
  Nesta máquina o `--scan` bateu com o `du -sxB1` em `/usr/share`, `/etc`,
  `/var`, `~/Downloads` e `/` (com pastas sem permissão e montagens
  dentro). Tudo passa com gcc, clang e ASan; a janela com ASan fecha
  limpa no meio de uma varredura.
- Medidas: hover redesenha em ~0,05 ms, o desenho completo em ~14 ms e o
  layout em ~0,3 ms (`-v`); 13 MB de memória própria com
  `/usr/share/icons` (56 mil arquivos) aberta. O tempo contra o `du` ainda
  vai para o `docs/BENCH.md`: a primeira medição saiu contaminada (disco
  disputado, cache frio por falta de memória livre).

Lições:
- **Pasta de montagem no `du -x`:** não conta nem o bloco da própria
  pasta; com tamanho 0 os totais batem, inclusive em `/`.
- **Hardlinks:** conta o primeiro encontrado. A ordem da nossa varredura
  (a pasta inteira antes das subpastas) não é a do `du`, mas o total é o
  mesmo.
- **`qsort` não tem ponteiro de usuário no C11:** a árvore a ordenar vai
  numa variável `_Thread_local` (cada thread ordena a sua).
- **Anéis estreitos demais para texto:** com 5 anéis numa janela comum,
  cada anel tem ~30 px; nomes dentro quase nunca cabem. Os do primeiro
  anel vão do lado de fora, com linha, como no Filelight.
- **GtkButton com imagem:** o filho deixa de ser o `GtkLabel`; para
  limitar a largura do texto, monte o botão com uma `GtkHBox`.
- **Lixeira em tmpfs:** o `g_file_trash` recusa ("system internal
  mounts"); o erro aparece num diálogo e nada muda na árvore.
- **LSan e GTK:** o GTK2 guarda para sempre o que carrega de ícones; a
  supressão por nome de função só funciona com
  `ASAN_OPTIONS=fast_unwind_on_malloc=0`.

### Fase 5 — Limpeza do usuário

- `kd_cleaner`: carregar e validar definições.
- `kd_clean`: prévia e execução, numa thread, com cancelar e registro.
- Aba Limpeza e primeiro lote de definições.
- `kidisks --clean-preview ID[.OPÇÃO]...` e `--clean`.

**Aceite** (testes num `$HOME` falso no scratchpad):
- links simbólicos apontando para fora não são seguidos nem apagados;
- definição com caminho proibido é recusada ao carregar;
- a prévia lista exatamente o que a limpeza apaga.

### Fase 6 — redimensionar e verificar

- Redimensionar na ordem certa, que o UDisks2 não faz sozinho: para
  aumentar, a partição primeiro e depois o sistema de arquivos; para
  diminuir, o contrário.
- Diálogo com a barra arrastável do GParted. Só a borda final se move,
  nunca o início.
- Verificar e reparar (`Filesystem.Check/Repair`).

**Aceite:** ext4, ntfs e vfat aumentados e diminuídos em loop devices, com
`fsck` limpo depois e os arquivos de teste intactos (checksum). Roteiro
manual atualizado.

### Fase 7 — limpeza do sistema

- `kidisks-helper`: binário pequeno, chamado via `pkexec` com uma política
  polkit própria (`data/org.kiyoshi.kidisks.policy`). Recebe só
  identificadores de ações fixas, nunca caminhos: `apt-clean`,
  `apt-autoclean`, `journal-vacuum`, `var-tmp`, `crash`, `coredumps`,
  `old-logs`.
- Os limpadores com `root = true` passam por ele.

**Aceite:** o helper recusa qualquer argumento fora da lista e é testado
num namespace, sem mexer no sistema de verdade.

### Fase 8 — extras (a escolher)

- Uso de partições desmontadas (`dumpe2fs -h`, `ntfsinfo`... pelo helper).
- LUKS: destrancar, trancar, formatar cifrado.
- Varrer como root na aba Espaço, pelo helper.
- Mais idiomas além do pt_BR.
- Pacote .deb.

## Estrutura prevista

| Arquivo | Papel |
|---|---|
| `src/main.c` | opções, gettext, modo texto (`--list`, `--scan`, `--clean-preview`) ou janela |
| `src/prefs.c` | o que a janela lembra (`~/.config/kidisks/kidisks.ini` e o `.backup`; `kd_backup_file` e `kd_read_file_or_backup` em `util.c`) |
| `src/disks.c` | modelo de discos e partições (sysfs + udev + mountinfo + swaps) |
| `src/udevmon.c` | monitor do udev no laço da GLib |
| `src/udisks.c` | UDisks2 por GDBus, assíncrono; SMART |
| `src/ops.c` | fila de operações pendentes e o layout "depois" |
| `src/scan.c` | varredura em thread, árvore compacta |
| `src/rings.c` | layout dos anéis (geometria pura) |
| `src/cleaner.c` | definições de limpeza: carregar, validar, expandir caminhos |
| `src/clean.c` | prévia e execução da limpeza |
| `src/ini.c`, `src/log.c`, `src/util.c` | do kikarinhas |
| `src/i18n.h`, `po/` | gettext: `_()`, `N_()`; `.pot` e um `.po` por idioma |
| `ui/window.c` | janela principal e abas |
| `ui/units.c` | aba Unidades |
| `ui/partitions.c`, `ui/diskbar.c` | aba Partições e a barra gráfica |
| `ui/space.c` | aba Espaço e o desenho dos anéis |
| `ui/cleanup.c` | aba Limpeza |
| `helper/main.c` | `kidisks-helper` (fase 7) |
| `data/cleaners/*.ini` | definições de limpeza |
| `tests/unit/test_*.c` | um binário por arquivo |
| `tools/loopdisks.sh` | cria e desfaz os loop devices dos testes |
| `docs/TESTE-MANUAL.md` | roteiro para o computador de teste |

## Decidido

- Licença GPL-3.0-or-later.
- Operações de disco pelo UDisks2, com senha pelo polkit; interface nunca
  como root.
- Limpeza com arquivos próprios. Os do BleachBit servem só de referência,
  e o que for copiado deles leva o crédito no cabeçalho.
- Abas: Unidades, Partições, Espaço, Limpeza.
- Testes de disco só em loop devices aqui; testes reais em outro computador.
- Interface multi-idiomas com gettext desde o começo.
- Tamanhos em GiB, com o GB do fabricante entre parênteses nos discos.
- Nada de mexer no `/etc/fstab`.
