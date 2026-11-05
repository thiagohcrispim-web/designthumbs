# DesignThumbs

**[⬇ Baixar o DesignThumbs (.zip)](https://github.com/thiagohcrispim-web/designthumbs/raw/main/dist/DesignThumbs.zip)** — extraia e execute `DesignThumbs-Setup.exe`.
Ou o instalador direto: [DesignThumbs-Setup.exe](https://github.com/thiagohcrispim-web/designthumbs/raw/main/dist/DesignThumbs-Setup.exe)

> **Aviso de vírus ao baixar?** É um alarme falso: o programa ainda não tem assinatura digital e é novo, então o Chrome,
> o SmartScreen e alguns antivírus desconfiam dele. No Chrome, abra `chrome://downloads` e clique em **Manter** / **Baixar arquivo suspeito**;
> no Windows, clique em **Mais informações › Executar assim mesmo**. O código é aberto e é compilado pelo GitHub Actions a partir deste repositório.

Ferramentas leves para o Explorer do Windows 10/11 trabalhar com arquivos de design e fotografia.

- **DesignThumbs.dll** — miniaturas no Explorer.
- **Painel de visualização** do Explorer (`Alt+P`) para os mesmos formatos e para **PDF**, com setas ‹ › no canto da imagem para folhear as páginas de PDF e InDesign.
- **DesignPeek.exe** — selecione um arquivo e aperte **Espaço** para um preview grande (estilo Quick Look do Mac).
- **Converter com Alt** — selecione arquivos e segure **Alt**: botões para PNG, JPG ou PDF aparecem sobre o mouse.

## Visual
Temas: **Windows** (segue o claro/escuro do sistema), **Claro**, **Escuro** e **Crispy** — preto chumbo minimalista
com detalhes laranja #FF6633, um toque de neon e a fonte Bahnschrift. Escolha em Configurações › Sistema › Tema.

Ícone na área de notificação (ao lado do relógio): clique para **Configurações**, **Reiniciar** (se algo parar de
responder) ou **Encerrar**.

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
| Paletas | ase, aco | miniatura com as amostras; no preview, nome e HEX de cada cor |
| Fontes | ttf, otf, ttc | painel Alt+P e preview com folha de amostra (miniaturas ficam com o Windows) |
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
| Passar o mouse / clicar | conta-gotas: mostra o HEX da cor / copia |
| ⚙ (canto superior esquerdo) | configurações e ajuda |

A barra inferior mostra dimensões, DPI e tamanho de impressão, modo de cor, páginas, câmera, tamanho do arquivo, programa de origem
e as **fontes** usadas em INDD/AI — as que não estão instaladas aparecem em vermelho com ⚠.

## Menu circular (segure Alt)
Selecione arquivos no Explorer e segure **Alt**: um menu circular aparece em volta do mouse, só com o que serve para
aqueles arquivos. **Gire a roda do mouse** (ou clique no centro) para trocar de lado:

| Lado | O que tem |
|---|---|
| **Converter** | PNG, JPG, PDF, WEBP, TIFF (várias páginas), ICO (16–256 px), GIF, BMP |
| **Ferramentas** | Redimensionar (25–75% ou 1080–3840 px), Recortar (com proporções 1:1, 4:5, 3:2, 16:9, 9:16), Girar 90°, Espelhar, Tons de cinza, Sem metadados |
| **PDF** | Juntar (na ordem dos cliques, mantendo texto e vetores), Dividir, Extrair páginas (`1-3, 5, 8-`), Comprimir |
| **Extras** | Camadas do PSD em PNG, Folha de cores (PNG + PDF), HEX/CSS, ASE ⇄ ACO, Folha de amostra da fonte |
| **Predefinições** | WhatsApp, Instagram, Web, E-mail, Impressão, Ícone… editáveis em Configurações › Predefinições |

Os arquivos novos são salvos ao lado dos originais, sem sobrescrever nada. Resolução (72/300 dpi), qualidade e PDF
comprimido ou sem compressão ficam nas configurações. Linha de comando: `DesignPeek.exe /ring arquivo…`
(também `/resize`, `/pages`, `/crop`).

## Menu Novo › Pasta Padrão Maker
Botão direito numa pasta › **Novo › Pasta Padrão Maker** (logo depois de Pasta e Atalho) cria as pastas da lista
(padrão: **Edit**, **Links** e **Preview**) e deixa todas selecionadas. A lista fica em
`%LOCALAPPDATA%\DesignThumbs\pastas.txt` — uma pasta por linha, `Links\Fontes` cria subpasta — e abre pelo botão
**Editar** nas configurações.

## Idiomas
Português, inglês e espanhol — segue o idioma do Windows, ou escolha nas configurações.

## Configurações
Clique na engrenagem do preview, rode `DesignPeek.exe` de novo ou use o atalho "DesignPeek" do menu Iniciar:
preview com Espaço, botões do Alt (e tempo), qualidade do JPG/PDF, 72/300 dpi, PDF comprimido ou sem compressão, iniciar com o Windows.

## Instalar
Baixe e rode **DesignThumbs-Setup.exe** (não precisa de administrador). Rodando de novo, ele oferece **atualizar/reinstalar** ou **desinstalar**. É o próprio DesignPeek com a DLL embutida:
ele se copia para `%LOCALAPPDATA%\Programs\DesignThumbs`, registra as miniaturas e aparece em
**Configurações › Aplicativos**, onde também se desinstala. Rodar uma versão nova por cima atualiza, sem reiniciar o Explorer.

Linha de comando: `DesignThumbs-Setup.exe /install /S` (silencioso) · `DesignPeek.exe /uninstall [/S]`.

## Atualização automática
O DesignPeek procura versões novas nas Releases de `thiagohcrispim-web/designthumbs` uma vez por dia. Quando encontra,
mostra um aviso; um clique baixa e instala em segundo plano. Dá para desligar ou procurar na hora nas configurações.

## Licença
[MIT](LICENSE) — grátis para usar, modificar e redistribuir, mantendo o aviso de autoria.

## Code signing policy (política de assinatura de código)
Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org).
*(Em processo de inscrição — enquanto não for aprovado, os binários ainda não são assinados.)*

- Committers and reviewers: [Thiago Crispim](https://github.com/thiagohcrispim-web)
- Approvers: [Thiago Crispim](https://github.com/thiagohcrispim-web)

Os binários são compilados automaticamente pelo GitHub Actions a partir deste repositório (`.github/workflows/build.yml`)
e cada versão é aprovada manualmente antes da assinatura.

## Privacidade
Este programa não envia informação a outros sistemas em rede, exceto quando solicitado pelo usuário ou por quem o instala ou opera:
uma vez por dia ele consulta este repositório no GitHub (`latest.json`) para saber se há versão nova, sem enviar dados pessoais.
A verificação pode ser desligada em **Configurações › Atualizar automaticamente**. A política de privacidade do GitHub
se aplica a essa consulta: https://docs.github.com/site-policy/privacy-policies/github-general-privacy-statement

## Compilar
- Linux: `./build.sh` (MinGW `g++-mingw-w64-x86-64-posix`; os headers mais novos do mingw-w64 já estão em `third_party/`).
- Automático: o GitHub Actions compila a cada envio e publica o `DesignThumbs-Setup.exe` como artefato.

## Diagnóstico
`DesignPeek.exe /diag "arquivo"` (ou arraste um arquivo para cima de `diagnostico.bat`) mostra, passo a passo, como ele é lido
(leitor de PDF, páginas, registro do painel Alt+P). Ctrl+C copia o texto da janela.

## Versões
- **v9.1** — WEBP com compressão máxima (método 6, YUV nítido; gráficos tentam sem perdas e fica o menor arquivo);
  menu circular em vidro líquido com brilho laranja que segue o mouse e granulado; ícones e botões minimalistas;
  temas Windows / Claro / Escuro / **Crispy** (preto chumbo, #FF6633, toque neon, fonte Bahnschrift);
  ícone na área de notificação com Configurações, Reiniciar e Encerrar.
- **v9** — menu circular com lados (converter, ferramentas, PDF, extras, predefinições); WEBP, TIFF, ICO, GIF, BMP;
  redimensionar, recortar, girar, espelhar, cinza, sem metadados; juntar/dividir/extrair PDF mantendo o original; comprimir PDF;
  camadas do PSD, paletas (folha, HEX/CSS, ASE⇄ACO), amostra de fonte; instalador oferece desinstalar ao ser reaberto.
- **v8.1** — instalador próprio (o DesignPeek se instala; sem NSIS), que não reinicia o Explorer; evita o alarme falso de "PUP" de antivírus.
- **v8** — instalador (DesignThumbs-Setup.exe) com desinstalação pelo Windows; atualização automática; paletas .ase/.aco;
  fontes .ttf/.otf; conta-gotas no preview; Pasta Padrão Maker configurável; português, inglês e espanhol.
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
