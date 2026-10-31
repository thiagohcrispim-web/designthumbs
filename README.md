# DesignThumbs

Ferramentas leves para o Explorer do Windows 10/11 trabalhar com arquivos de design e fotografia.

- **DesignThumbs.dll** — miniaturas no Explorer.
- **Painel de visualização** do Explorer (`Alt+P`) para os mesmos formatos.
- **DesignPeek.exe** — selecione um arquivo e aperte **Espaço** para um preview grande (estilo Quick Look do Mac).
- **Converter com Shift** — selecione arquivos e segure **Shift**: botões para PNG, JPG ou PDF aparecem sobre o mouse.

## Formatos
| Tipo | Extensões | Origem da imagem |
|---|---|---|
| Photoshop | psd, psb | imagem composta (8/16/32 bits, RGB/Cinza/CMYK) |
| Illustrator | ai | miniatura XMP; no preview, renderização em PDF pelo Windows (Edge/Acrobat) |
| InDesign | indd, indt | miniaturas XMP, uma por página |
| EPS | eps, epsf, epsi | preview TIFF/WMF, XMP ou EPSI |
| SVG | svg, svgz | Direct2D |
| Pacotes | cdr, afdesign, afphoto, afpub, xd, sketch, kra, ora, procreate | preview guardado no pacote |
| RAW de câmera | cr2, cr3, crw, nef, nrw, arw, srf, sr2, dng, raf, orf, rw2, rwl, pef, srw, 3fr, erf, kdc, mrw, mef, iiq | JPEG de preview embutido, com rotação |

## DesignPeek — atalhos
| Tecla | Ação |
|---|---|
| Espaço / Esc / clique direito | fecha |
| Setas ou clique em outro arquivo | troca o preview |
| Roda do mouse, + / - | zoom no cursor |
| Arrastar | mover |
| Z / 0 / duplo clique | 100% / ajustar / alternar |
| PgUp / PgDn | páginas (InDesign) |
| Enter | abre no programa padrão |
| Ctrl+C | copia a imagem (PNG com transparência) |
| B | fundo claro/escuro |
| ⚙ (canto superior esquerdo) | configurações e ajuda |

A barra inferior mostra dimensões, DPI e tamanho de impressão, modo de cor, páginas, câmera, tamanho do arquivo, programa de origem
e as **fontes** usadas em INDD/AI — as que não estão instaladas aparecem em vermelho com ⚠.

## Converter com Shift
Com um ou vários arquivos selecionados, segure **Shift** (sozinho, ~0,5 s) e clique em:
- **PNG** / **JPG** — um arquivo por original (InDesign: um por página), salvo ao lado do original, na maior resolução disponível.
- **PDF** — com um arquivo, um PDF; com vários, **um único PDF com todas as páginas na ordem da seleção**,
  nomeado automaticamente como `primeiro arquivo (+N).pdf`. Um `.ai` sozinho é copiado como PDF (mantém o vetor).

Um aviso no canto da tela mostra o progresso; clique nele para abrir a pasta com os arquivos gerados.

## Configurações
Clique na engrenagem do preview, rode `DesignPeek.exe` de novo ou use o atalho "DesignPeek" do menu Iniciar:
preview com Espaço, botões do Shift (e tempo), qualidade do JPG/PDF, iniciar com o Windows.

## Instalar
Extraia numa pasta fixa e rode `install.bat` (não precisa de administrador). `uninstall.bat` remove tudo.

## Compilar
- Windows: `build.bat` no "x64 Native Tools Command Prompt for VS".
- Linux: `build.sh` (MinGW + headers Direct2D recentes do mingw-w64 em `$D2D_INC`).

## Versões
- **v4** — painel de visualização (Alt+P), preview em segundo plano com cache e pré-carregamento dos vizinhos,
  conversão para PNG/JPG/PDF segurando Shift (PDF multipáginas na ordem da seleção), configurações (engrenagem),
  fundo claro/escuro.
- **v3** — preview com zoom/arrastar, fundo xadrez, páginas do InDesign, barra de informações com fontes faltando,
  Enter/Ctrl+C, AI nítido via PDF, novos formatos (EPS completo, SVGZ, CDR, Affinity, XD, Sketch, Krita, Procreate, RAW de câmera),
  instalação sem administrador.
- **v2.1** — DesignPeek sem hook de teclado (WinEvents + atalho só na lista de arquivos), inicialização pela pasta Inicializar.
- **v2** — DesignPeek (preview com Espaço).
- **v1.1** — PSD lido sob demanda (arquivos grandes e na nuvem); miniaturas XMP truncadas ignoradas.
- **v1** — thumbnail provider inicial.
