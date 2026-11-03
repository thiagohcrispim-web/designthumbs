# DesignThumbs

Ferramentas leves para o Explorer do Windows 10/11 trabalhar com arquivos de design e fotografia.

- **DesignThumbs.dll** — miniaturas no Explorer.
- **Painel de visualização** do Explorer (`Alt+P`) para os mesmos formatos e para **PDF**, com setas ‹ › no canto da imagem para folhear as páginas de PDF e InDesign.
- **DesignPeek.exe** — selecione um arquivo e aperte **Espaço** para um preview grande (estilo Quick Look do Mac).
- **Converter com Alt** — selecione arquivos e segure **Alt**: botões para PNG, JPG ou PDF aparecem sobre o mouse.

## Visual
Interface de vidro inspirada no visionOS: desfoque acrylic do Windows, cantos arredondados, cápsulas flutuantes e
controles no estilo iOS, seguindo automaticamente o tema claro/escuro do Windows. Sem o efeito de vidro (Windows antigo),
usa um fundo sólido com o mesmo visual.

## Formatos
| Tipo | Extensões | Origem da imagem |
|---|---|---|
| Photoshop | psd, psb | imagem composta (8/16/32 bits, RGB/Cinza/CMYK) |
| Illustrator | ai | miniatura XMP; no preview e na conversão, cada prancheta renderizada pelo leitor de PDF do Windows |
| PDF | pdf | painel Alt+P, preview e conversão, página a página (leitor de PDF do Windows) |
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
| PgUp / PgDn ou setas ‹ › na imagem | páginas (InDesign) |
| Enter | abre no programa padrão |
| Ctrl+C | copia a imagem (PNG com transparência) |
| ⚙ (canto superior esquerdo) | configurações e ajuda |

A barra inferior mostra dimensões, DPI e tamanho de impressão, modo de cor, páginas, câmera, tamanho do arquivo, programa de origem
e as **fontes** usadas em INDD/AI — as que não estão instaladas aparecem em vermelho com ⚠.

## Converter com Alt
Com um ou vários arquivos selecionados, segure **Alt** (~0,5 s) e clique em:
- **PNG** / **JPG** — um arquivo por original (InDesign: um por página), salvo ao lado do original, na maior resolução disponível.
- **PDF** — com um arquivo, um PDF; com vários, **um único PDF com as páginas na ordem em que você clicou nos arquivos**
  (selecionou 1.jpg, depois 3.jpg e 2.jpg → páginas 1, 3, 2),
  nomeado automaticamente como `primeiro arquivo (+N).pdf`. Um `.ai` sozinho é copiado como PDF (mantém o vetor).

Resolução: **72 dpi** (padrão) ou **300 dpi** — documentos com tamanho físico (InDesign, Illustrator, SVG) são gerados nessa
resolução; imagens mantêm seus pixels e recebem o DPI escolhido (no PDF, define o tamanho da página).
PDF **comprimido** (JPEG, arquivo pequeno) ou **normal, sem compressão** (sem perda, arquivo grande).

Um aviso no canto da tela mostra o progresso; clique nele para abrir a pasta com os arquivos gerados.

## Menu Novo › Pasta Padrão Maker
Botão direito numa pasta › **Novo › Pasta Padrão Maker** (logo depois de Pasta e Atalho) cria as pastas **Edit**, **Links** e **Preview** de uma vez
e deixa as três selecionadas.

## Configurações
Clique na engrenagem do preview, rode `DesignPeek.exe` de novo ou use o atalho "DesignPeek" do menu Iniciar:
preview com Espaço, botões do Alt (e tempo), qualidade do JPG/PDF, 72/300 dpi, PDF comprimido ou sem compressão, iniciar com o Windows.

## Instalar
Extraia numa pasta fixa e rode `install.bat` (não precisa de administrador). `uninstall.bat` remove tudo.

## Compilar
- Windows: `build.bat` no "x64 Native Tools Command Prompt for VS".
- Linux: `build.sh` (MinGW + headers Direct2D recentes do mingw-w64 em `$D2D_INC`).

## Diagnóstico
Arraste um arquivo para cima de `diagnostico.bat`: uma janela mostra, passo a passo, como ele é lido
(leitor de PDF, páginas, registro do painel Alt+P). Ctrl+C copia o texto da janela.

## Versões
- **v7.3** — ícone do programa.
- **v7.2** — ferramenta de diagnóstico (`diagnostico.bat`).
- **v7.1** — corrige o PDF travando até o tempo-limite (o arquivo é copiado para a memória antes de ir ao leitor de PDF);
  registro de diagnóstico em `%LOCALAPPDATA%\DesignThumbs\pdf.log`.
- **v7** — PDF e pranchetas do Illustrator página a página (leitor de PDF nativo do Windows); setas discretas no canto
  da imagem no painel Alt+P, só para PDF e InDesign; Pasta Padrão Maker logo abaixo de Pasta/Atalho no menu Novo.
- **v6** — setas de página no painel Alt+P e no preview; Novo › Pasta Padrão Maker (Edit, Links, Preview).
- **v5** — nova interface de vidro (preview, botões do Alt, aviso e configurações), tema claro/escuro do Windows,
  animação de abertura, `DesignPeek.exe /peek arquivo` para abrir um arquivo direto.
- **v4.2** — botões de conversão no Alt; 72/300 dpi; PDF comprimido ou sem compressão; ordem do PDF = ordem dos cliques.
- **v4.1** — botões de conversão passam do Shift para o Ctrl; Ctrl+clique reinicia a contagem.
- **v4** — painel de visualização (Alt+P), preview em segundo plano com cache e pré-carregamento dos vizinhos,
  conversão para PNG/JPG/PDF segurando Shift (v4.1: Ctrl, v4.2: Alt) (PDF multipáginas na ordem da seleção), configurações (engrenagem),
  fundo claro/escuro.
- **v3** — preview com zoom/arrastar, fundo xadrez, páginas do InDesign, barra de informações com fontes faltando,
  Enter/Ctrl+C, AI nítido via PDF, novos formatos (EPS completo, SVGZ, CDR, Affinity, XD, Sketch, Krita, Procreate, RAW de câmera),
  instalação sem administrador.
- **v2.1** — DesignPeek sem hook de teclado (WinEvents + atalho só na lista de arquivos), inicialização pela pasta Inicializar.
- **v2** — DesignPeek (preview com Espaço).
- **v1.1** — PSD lido sob demanda (arquivos grandes e na nuvem); miniaturas XMP truncadas ignoradas.
- **v1** — thumbnail provider inicial.
