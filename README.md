# DesignThumbs

Ferramentas leves para o Explorer do Windows 10/11 trabalhar com arquivos de design.

- **DesignThumbs.dll** — miniaturas para `.psd .psb .ai .indd .indt .eps .svg`.
- **DesignPeek.exe** — selecione um arquivo e aperte **Espaço** para ver um preview grande (estilo Quick Look do Mac).
  Espaço, Esc ou clique fecham; as setas ou um clique em outro arquivo trocam o preview.

## Instalar
Extraia numa pasta fixa e rode `install.bat` como administrador. `uninstall.bat` remove tudo.

## Compilar
- Windows: `build.bat` no "x64 Native Tools Command Prompt for VS".
- Linux: `build.sh` (MinGW).

## Versões
- **v2.1** — DesignPeek sem hook de teclado (WinEvents + atalho só na lista de arquivos), inicialização pela pasta Inicializar, informações de versão no .exe. Evita o falso positivo IDP.Generic do Norton.
- **v2** — DesignPeek (preview com Espaço).
- **v1.1** — PSD lido sob demanda (arquivos grandes e na nuvem); miniaturas XMP truncadas ignoradas.
- **v1** — thumbnail provider inicial.
