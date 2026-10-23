# Kidisks

*[English](README.md)*

Gerenciador de discos leve para Linux, em C e GTK2. Junta num programa só
o que o GParted, o Filelight e o BleachBit fazem:

- **Unidades:** todos os discos e partições num relance: modelo, tipo,
  saúde, temperatura e quanto cada partição está ocupada.
- **Partições:** criar, excluir, formatar e redimensionar, com uma fila de
  operações que só é aplicada quando você manda.
- **Espaço:** o que ocupa cada disco, em círculos concêntricos.
- **Limpeza:** apaga caches, sobras e rastros dos programas, sempre com
  prévia do que vai sair.

O Kidisks roda como usuário comum. Quando uma alteração precisa de root, o
UDisks2 pede a senha de administrador pelo polkit.

**Estado:** em desenvolvimento. Prontas a aba Unidades (discos, partições,
LUKS e LVM, uso, saúde e temperatura pelo SMART, montar, desmontar e
remover com segurança) e a aba Espaço (anéis como no Filelight, com os
mesmos totais do `du`, lixeira e apagar). Veja o [PLANO.md](PLANO.md).

Também pela linha de comando:

```sh
kidisks --list            # discos e partições
kidisks --scan ~ --top 10 # o que mais ocupa espaço na pasta pessoal
kidisks --space ~/Downloads  # abre a aba Espaço nessa pasta
kidisks --info sda        # saúde, temperatura, tempo ligado
kidisks --mount sdb1      # monta (e diz onde)
kidisks --power-off sdb   # desmonta tudo e desliga o disco
```

## Compilar

Dependências (Debian/Ubuntu): `build-essential pkg-config libgtk2.0-dev
libudev-dev gettext`; para usar: `udisks2`.

```sh
make
build/kidisks
sudo make install      # em /usr/local; PREFIX=/usr para mudar
```

## Licença

GPL-3.0-or-later. Veja [LICENSE](LICENSE).
