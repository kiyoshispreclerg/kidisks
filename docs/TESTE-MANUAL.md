# Kidisks — roteiro de teste manual

Para o computador de teste, o que **não tem dados a perder**. Cada fase de
disco acrescenta uma seção. Anote o resultado de cada item (ok / falhou +
o que apareceu) e, se algo falhar, rode de novo com `kidisks -v` e guarde a
saída.

Antes de começar:

```sh
make && build/kidisks --version
build/kidisks --list --all          # o que o Kidisks vê
lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT   # para comparar
tools/check-lsblk.py                # a comparação automática
```

## Fase 1 — aba Unidades (só leitura, montar e desmontar)

Nada nesta fase formata ou particiona. O pior que pode acontecer é
desmontar algo em uso: feche os programas que usam o disco antes.

### Listagem

- [ ] `tools/check-lsblk.py` termina com "0 differ".
- [ ] A aba Unidades mostra um cartão por disco, com modelo, barramento
      (SATA, USB, NVMe), HD/SSD, tamanho em GiB com o GB do fabricante entre
      parênteses, tabela (MBR/GPT) e número de série.
- [ ] O disco do sistema traz "disco do sistema" e **não** oferece
      "Desmontar" para `/`, `/home`, `/boot` e `/boot/efi`.
- [ ] As barras de uso batem com o `df -h` (mesmos números, arredondados).
- [ ] Espaço não alocado de 64 MiB ou mais aparece como "não alocado".
- [ ] Partição estendida (se houver MBR com lógicas) aparece com as lógicas
      recuadas embaixo.
- [ ] LUKS aberto aparece como "destrancado", com o volume de dentro
      recuado embaixo e a barra de uso dele; LUKS fechado, "trancado".
- [ ] Exibir → "Mostrar dispositivos ocultos" mostra os loops (snaps) e a
      zram; desmarcar esconde de novo; a escolha é lembrada ao reabrir.

### Saúde (SMART)

- [ ] Discos SATA mostram "Boa", a temperatura e o tempo ligado.
      Compare com `sudo smartctl -A /dev/sdX` (atributo 194) ou com o
      GNOME Disks: a temperatura deve bater com o atributo 194.
- [ ] Discos USB sem SMART mostram "Sem dados de saúde (SMART)".
- [ ] NVMe: com UDisks 2.9 aparece "Sem dados de saúde"; com 2.10 ou mais
      novo, temperatura e tempo ligado (anote a versão: `udisksctl
      status | head -1` ou `dpkg -l udisks2`).

### Montar, desmontar, remover

Use um pendrive.

- [ ] Plugar o pendrive: o cartão aparece sozinho em até 1 s.
- [ ] "Montar" numa partição desmontada: monta (em `/media/...`), a barra
      aparece, a barra de status diz onde montou.
- [ ] O botão de pasta abre o gerenciador de arquivos nela.
- [ ] Com um terminal aberto dentro da pasta montada, "Desmontar" mostra o
      erro de dispositivo em uso (em português); fechando o terminal,
      desmonta.
- [ ] Montar uma partição interna (não removível) pede a senha de
      administrador; cancelar o pedido não mostra erro.
- [ ] "Remover com segurança": desmonta tudo e desliga o pendrive (a luz
      apaga); o cartão some.
- [ ] Montar ou desmontar pelo gerenciador de arquivos (Dolphin) com o
      Kidisks aberto: a aba atualiza sozinha.
- [ ] `KIDISKS_ONLY_LOOP=1 build/kidisks`: os botões de discos reais ficam
      desativados, com a explicação ao passar o mouse.

## Fase 4 — aba Espaço

Só lê, a não ser "Mandar para a lixeira" e "Apagar…". Para esses dois, use
uma pasta de teste criada para isso (por exemplo `~/kidisks-teste`, com
algumas subpastas e arquivos copiados), nunca algo que você queira manter.

### Totais

- [ ] `build/kidisks --scan ~ -b --top 0` e `du -sxB1 ~` dão o mesmo
      número. Repita com `/` (os avisos de "sem permissão" são esperados,
      e os totais têm que bater mesmo assim) e com um disco externo
      montado.
- [ ] `build/kidisks --scan ~ --top 10`: as 10 maiores entradas da pasta
      pessoal, da maior para a menor; pastas com `/` no fim.
- [ ] Anote o tempo de `time build/kidisks --scan ~ -b --top 0` e de
      `time du -sxB1 ~`, cada um rodado duas vezes seguidas (a segunda é a
      que vale: cache quente).

### Anéis

- [ ] Aba Espaço sem nada escolhido: a explicação no meio; botões "Pasta
      pessoal", um por sistema de arquivos montado (com o rótulo, ou o nome
      da pasta de montagem) e "Escolher pasta…".
- [ ] "Pasta pessoal": aparece o progresso (arquivos e tamanho subindo) e
      depois os anéis. O centro mostra o nome, o total e os arquivos.
- [ ] Os nomes das fatias grandes do primeiro anel aparecem do lado de
      fora, com uma linha até a fatia, sem se sobrepor.
- [ ] Passar o mouse destaca a fatia e mostra embaixo o caminho, o tamanho
      e a porcentagem; nas fatias cinza agrupadas, "N itens pequenos".
- [ ] Clicar numa pasta entra nela (mesmo num anel de fora); clicar no
      centro, no botão de subir, em Backspace ou no botão "voltar" do mouse
      volta. A trilha de pastas acima acompanha e cada pedaço dela é
      clicável.
- [ ] "Cancelar" durante a varredura de `/` para na hora e volta ao que
      havia antes.
- [ ] Fechar o Kidisks no meio de uma varredura fecha na hora.
- [ ] Na aba Unidades, o botão de espaço de uma partição montada abre a
      aba Espaço nela.
- [ ] `build/kidisks --space ~/Downloads` abre direto na aba Espaço
      olhando a pasta.

### Menu de contexto (botão direito)

- [ ] "Abrir no gerenciador de arquivos" numa pasta abre o Dolphin nela;
      num arquivo, abre a pasta dele com o arquivo selecionado.
- [ ] "Abrir um terminal aqui" abre o Konsole na pasta.
- [ ] "Copiar o caminho" e colar num editor: o caminho completo.
- [ ] Na pasta de teste: "Mandar para a lixeira" some com a fatia e o
      item aparece na lixeira do Dolphin; os totais acima diminuem.
- [ ] Na pasta de teste: "Apagar…" pede confirmação com o tamanho; ao
      confirmar, apaga e a fatia some. Confira no terminal que um link
      simbólico dentro da pasta apagada **não** levou junto o que ele
      apontava.
- [ ] "Mandar para a lixeira" e "Apagar…" ficam desativados no centro da
      pasta de onde a varredura começou.
- [ ] Criar um arquivo grande na pasta mostrada e clicar em "Olhar esta
      pasta de novo": a fatia nova aparece sem varrer tudo outra vez.
