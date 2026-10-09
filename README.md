# Kidisks

*[English](README.en.md)*

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

**Estado:** em desenvolvimento (fase 0: a janela com as abas vazias). Veja o
[PLANO.md](PLANO.md).

## Compilar

Dependências (Debian/Ubuntu): `build-essential pkg-config libgtk2.0-dev
gettext`.

```sh
make
build/kidisks
sudo make install      # em /usr/local; PREFIX=/usr para mudar
```

## Licença

GPL-3.0-or-later. Veja [LICENSE](LICENSE).
