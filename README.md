# DesignThumbs

Ferramentas leves para o Explorer do Windows 10/11 trabalhar com arquivos de design.

- **DesignThumbs.dll** — miniaturas para `.psd .psb .ai .indd .indt .eps .svg`.
- **DesignPeek.exe** — selecione um arquivo e aperte **Espaço** para ver um preview grande (estilo Quick Look do Mac).
  Espaço, Esc ou clique fecham; as setas trocam de arquivo com o preview aberto.

## Instalar
Extraia numa pasta fixa e rode `install.bat` como administrador. `uninstall.bat` remove tudo.

## Compilar
- Windows: `build.bat` no "x64 Native Tools Command Prompt for VS".
- Linux: `build.sh` (MinGW).

## Versões
- **v2** — DesignPeek (preview com Espaço).
- **v1.1** — PSD lido sob demanda (arquivos grandes e na nuvem); miniaturas XMP truncadas ignoradas.
- **v1** — thumbnail provider inicial.
