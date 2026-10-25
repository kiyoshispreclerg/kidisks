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

## Instalar

O jeito mais fácil é o pacote `.deb` da
[página de releases](https://github.com/kiyoshispreclerg/kidisks/releases).
Cada release traz um para Ubuntu 22.04, Ubuntu 24.04, Debian 12 e Debian 13;
escolha o da sua distro. Os nomes são `kidisks_VERSÃO-1_SISTEMA_ARQ.deb`, e
cada um foi instalado e executado num sistema limpo do próprio tipo antes de
ser publicado. As releases `v0.x.x` (e tags com hífen, como `v1.0.0-rc1`)
saem marcadas como pré-release.

```sh
sudo apt install ./kidisks_0.1.0-1_ubuntu24.04_amd64.deb   # use o arquivo que baixou
sudo apt remove kidisks
```

O `apt` traz as bibliotecas necessárias, e o `udisks2` (usado para montar,
ejetar e ler o SMART) vem junto como pacote recomendado. Para instalar a
partir do código, veja [Compilar](#compilar).

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
