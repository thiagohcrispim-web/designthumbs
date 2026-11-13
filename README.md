<p align="center"><img src="docs/hero.png" alt="Vizu: miniaturas e preview de arquivos de design no Explorer do Windows" width="100%"></p>

<h3 align="center">Veja todos os seus arquivos de design no Windows,<br>mesmo sem os programas instalados.</h3>

<p align="center">
  <a href="https://github.com/thiagohcrispim-web/vizu/releases/latest/download/Vizu-Setup.exe"><b>⬇ Baixar o Vizu para Windows</b></a> &nbsp;·&nbsp; grátis e de código aberto (MIT)
</p>

---

O Vizu ensina o Explorer do Windows 10 e 11 a mostrar **miniaturas e preview** de arquivos de Photoshop, Illustrator,
InDesign, CorelDRAW, Affinity, Sketch, fotos RAW e muito mais. Sem abrir nenhum programa, sem ter os programas
instalados. E, de quebra, junta no mesmo lugar as ferramentas que você usa o dia todo: converter, recortar, ler texto
de imagens, traduzir, cronometrar o foco e organizar o seu dia.

**Vizu 9.8:** escolha a edição no instalador (cerca de 2 MB):

| Edição | O que tem |
|---|---|
| **Vizu Lite** | Prévia com Espaço e miniaturas no Explorer |
| **Vizu** | Tudo do Lite + menu circular (converter, ferramentas, PDF, extras), OCR, relógio e agenda |
| **Vizu Ultra** | Tudo do Vizu + CDR/SVG para **SVG e EPS vetorial** (baixa o motor Inkscape, ~100 MB, uma única vez) |

## Veja tudo, sem abrir nada

<img src="docs/formatos.png" alt="Formatos suportados" width="100%">

Miniaturas direto nas pastas e no painel de visualização do Explorer (`Alt+P`). Nada de ícone genérico: você vê a
arte, a capa da revista, a prancheta do Illustrator, a foto da câmera.

## Espaço pra ver

<img src="docs/preview.png" alt="Preview com a barra de espaço" width="100%">

Selecione um arquivo e aperte **Espaço**: um preview grande abre na hora, como o Quick Look do Mac. Setas trocam de
arquivo, a roda do mouse dá zoom, `Z` mostra em 100%. Embaixo aparecem tamanho, DPI, modo de cor, camadas e até as
fontes usadas no arquivo (as que faltam no seu PC ficam em vermelho). Passe o mouse para ver o **HEX** de qualquer cor,
e clique para copiar.

## Alt pra resolver

<img src="docs/ring.png" alt="Menu circular de conversão" width="100%">

Selecione arquivos e segure **Alt**: um menu circular aparece em volta do mouse, só com o que faz sentido para eles.

- **Converter** para PNG, JPG, PDF, WEBP, TIFF, SVG ou EPS, ou **compactar em ZIP**.
- **Ferramentas**: redimensionar, recortar com proporções prontas, girar, espelhar, tons de cinza, tirar metadados.
- **PDF**: juntar na ordem em que você clicou, dividir, extrair páginas, comprimir.
- **Extras**: exportar camadas do PSD, folha de cores, HEX/CSS, converter paletas ASE ⇄ ACO, amostra de fonte.

Os arquivos novos ficam ao lado dos originais. Nada é sobrescrito.

**Conversão vetorial SVG/EPS:** disponível para arquivos CDR e SVG, quando o Conversor de Arquivos Vetoriais é selecionado no instalador. O motor portátil está incluído no pacote e é configurado automaticamente. Os objetos são lidos do documento original; bitmaps não são incorporados como substitutos dos vetores. Efeitos incompatíveis bloqueiam EPS com diagnóstico. Veja [configuração do motor e limitações](docs/vector-conversion.md).

## Ler texto de qualquer lugar

<img src="docs/ocr.png" alt="OCR e tradução" width="100%">

Aperte **Ctrl+Alt+T**, arraste sobre qualquer coisa na tela (imagem, PDF, vídeo, site) e o texto já vai para a área de
transferência. Um clique traduz para **português, espanhol ou inglês**. A leitura é feita no seu próprio PC; o texto
só sai dele quando você pede a tradução.

## Relógio, foco e o seu dia

<img src="docs/clock.png" alt="Relógio com Pomodoro e roubo de tempo" width="100%">

Um relógio discreto na área de trabalho, ou dentro da barra de tarefas, com:

- **Foco (Pomodoro)** por cores: 25 minutos de foco, 5 de pausa, e o relógio muda de cor.
- **Roubo de tempo**: alguém parou na sua mesa? Clique na caveira para contar, e de novo para registrar. No fim do dia
  você sabe exatamente quanto tempo perdeu.
- **Dia**: sua agenda, com avisos 10 e 5 minutos antes de cada compromisso.

<img src="docs/agenda.png" alt="Agenda do dia e avisos" width="100%">

## E mais

<img src="docs/extras.png" alt="Limpeza de temporários, Pastas Padrão Vizu e mais" width="100%">

- **Pastas Padrão Vizu**: botão direito › Novo cria `Edit`, `Links` e `Preview` (cada uma com `_Morto`) de uma vez.
- **Limpar arquivos temporários** pelo botão direito, com a opção de esvaziar a Lixeira em seguida.
- Ícone na área de notificação para abrir as Configurações, reiniciar ou encerrar.

## Do seu jeito

<img src="docs/temas.png" alt="Temas: Claro, Escuro, Windows, Vizu Classic e Vizu Crispy" width="100%">

Cinco temas: **Claro**, **Escuro**, **Windows** (segue o seu Windows e a cor de destaque), **Vizu Classic** e
**Vizu Crispy** (neon na cor que você escolher). Em português, inglês e espanhol. Cada recurso pode ser ligado ou
desligado nas Configurações.

<p align="center"><img src="docs/settings.png" alt="Configurações do Vizu" width="60%"></p>

## Instalar

1. Baixe o **[instalador](https://github.com/thiagohcrispim-web/vizu/releases/latest/download/Vizu-Setup.exe)** e abra. Não precisa de administrador.
2. Pronto: as miniaturas aparecem conforme você navega pelas pastas.

Para atualizar, o Vizu avisa sozinho quando sai versão nova. Para desinstalar, rode o instalador de novo ou use
**Configurações do Windows › Aplicativos**.

> **O Windows ou o navegador avisou que o arquivo é suspeito?** É um alarme falso: o Vizu é novo e ainda não tem
> assinatura digital, então o SmartScreen desconfia. Clique em **Mais informações › Executar assim mesmo** (no Chrome,
> **Manter**). O código está todo aqui, e o instalador é compilado pelo GitHub Actions a partir deste repositório.

## Privacidade

O Vizu não envia nada sobre você. Ele só acessa a internet para: procurar versão nova uma vez por dia (consulta o
`latest.json` deste repositório; dá para desligar) e traduzir um texto quando você clica em PT, ES ou EN. Mais nada.
A política de privacidade do GitHub se aplica à consulta de atualização:
https://docs.github.com/site-policy/privacy-policies/github-general-privacy-statement

<details>
<summary><b>Todos os formatos</b></summary>

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
</details>

<details>
<summary><b>Atalhos do preview</b></summary>

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
| ⚙ (canto superior esquerdo) | Configurações |
</details>

<details>
<summary><b>Para desenvolvedores</b></summary>

- Compilar no Linux: `./build.sh` (MinGW `g++-mingw-w64-x86-64-posix`; os headers mais novos do mingw-w64 estão em `third_party/`).
- O GitHub Actions compila a cada envio e publica o instalador como artefato (`.github/workflows/build.yml`).
- Instalação silenciosa: `Vizu-Setup.exe /install /S` · desinstalar: `Vizu.exe /uninstall [/S]`.
- Diagnóstico de um arquivo: `Vizu.exe /diag "arquivo"` ou arraste o arquivo sobre `diagnostico.bat`.
- A biblioteca `DesignThumbs.dll` (miniaturas do Explorer) e a chave de registro `HKCU\Software\DesignThumbs` mantêm o
  nome antigo, para não quebrar as miniaturas nem as configurações de quem já usa.
</details>

<details>
<summary><b>Histórico de versões</b></summary>

- **v9.8.9** — Corrige o fechamento do Vizu ao abrir a prévia com Espaço em pastas com SVG (também afetava as miniaturas de SVG). Se o Vizu fechar sozinho, o motivo fica em `%LOCALAPPDATA%\DesignThumbs\crash.log`.
- **v9.8.8** — Telas no visual do Vizu Classic: menu do Alt, Espiar (com botões copiar, abrir e fechar), avisos, relógio, agenda, OCR e conta-gotas. Histórico do roubo de tempo com Exportar CSV e Ver semana; "Atualizações" no menu da bandeja. Mais segurança: arquivos corrompidos não fecham o Vizu, a limpeza de temporários poupa pastas em uso e a atualização só instala com SHA-256 conferido.
- **v9.8.7** — Instalador com novo visual: escolha da versão em lista, progresso com etapas (e MB, velocidade e tempo no download do Ultra) e tela final com as teclas Espaço e Alt.
- **v9.8.6** — Configurações reorganizadas: Tempo (Relógio e Agenda), Outros (OCR, Pastas e Limpeza) e Sistema recolhível, com "Procurar nova versão" na linha de atualização. Cores do Pomodoro atualizadas e OCR com "Juntar" por padrão.
- **v9.8.5** — Nova versão de distribuição, sem mudanças de funcionamento em relação à 9.8.4.
- **v9.8.4** — Download do motor vetorial (Ultra) com progresso real: MB, velocidade e tempo restante. Retoma de onde parou se a conexão cair e permite cancelar.
- **v9.8.3** — Renomeado para Vizu: executável `Vizu.exe`, instalador `Vizu-Setup.exe` e repositório `vizu`.
- **v9.8.2** — Correções do conta-gotas: textos "Sugestão:" traduzidos; busca PANTONE mantém o resultado exato.
- **v9.8.1** — Conta-gotas mostra, embaixo do HEX, o **CMYK** (perfil de impressão do Windows) e o **PANTONE** Coated mais
  próximo (marcado como "Sugestão:", consultivo; confirme no guia físico). O clique continua copiando só o HEX.
- **v9.8** — Instalador com três edições: **Lite**, **Vizu** e **Ultra**. O instalador volta a ter ~2 MB: o Ultra baixa o
  motor vetorial com verificação SHA-256 e não o reinstala nas atualizações. SVG aceita bitmaps embutidos no CDR e o EPS
  aceita gradientes; erros de conversão viram um aviso curto com o detalhe num arquivo. Atualizações conferem o SHA-256.
- **v9.6.2** — SVG convertido agora abre no Illustrator (antes aparecia em branco).
- **v9.6.1** — Converter agora também gera **SVG** e **EPS** (a imagem vai embutida no arquivo; não é vetorização).
- **v9.6** — Identidade **Vizu**: logotipo nas Configurações, tema novo **Vizu Classic** (Design System Vizu:
  grafite e Ardósia, laranja só como preenchimento) e **Vizu Crispy** (o neon, agora translúcido a 85%). Tema Escuro
  com controles no grafite da Vizu; Padrão Windows com o menu circular claro/escuro e a cor de destaque do sistema.
  Configurações reorganizadas: Ver · Converter · Outros · Sistema · Temas; Relógio e OCR abrem seus subitens ao
  ativar; Roubo de tempo, Foco (Pomodoro, com tempos e cores) e Agenda podem ser desligados; Ctrl+Alt+T funciona
  com o relógio desligado; Pastas Padrão Vizu; qualidade JPG/PDF padrão 75%; interface em Inter (se instalada) e
  relógio em Bahnschrift; a tela esmaece para o preto ao abrir as Configurações (a barra de tarefas continua
  visível). Nome e ícone Vizu em todo o programa; código sem uso removido e programa mais leve.
- **v9.5.2** — **Agenda** no DesktopClock (clique no triângulo): dia e semana, linha do tempo com o horário atual,
  novo evento com título, dia, hora, duração e avisos; clique num evento para editar ou excluir. **Avisos** no canto
  inferior direito 10 min antes (some em 20 s) e 5 min antes (com Ok / Adiar 5 min). Ao anexar o relógio à barra
  de tarefas, o relógio do Windows na bandeja é ocultado (e volta ao desanexar).
- **v9.5.1** — DesktopClock: tamanho 100% / 90% / 70% e opção **Anexar à barra de tarefas** (o relógio fica dentro
  da barra, na cor dela, e desliza para os lados). Agradecimento especial nas Configurações.
- **v9.5** — **Ler texto da tela (OCR)** no DesktopClock: botão ao lado do ChatGPT ou **Ctrl+Alt+T**; a tela congela,
  você arrasta sobre o texto, o OCR do próprio Windows lê (sem internet) e o texto vai para a área de transferência.
  Um cartão oferece tradução para **PT / ES / EN** (Google Tradutor, com o MyMemory de reserva), juntar linhas e
  abrir no Google Tradutor. Configurações › DesktopClock: liga/desliga, quebras de linha e idiomas de leitura.
- **v9.4.3** — Crispy com menos luzes no menu circular (sem a régua na borda); o lado Converter fica com PNG, JPG,
  PDF, WEBP, TIFF e **ZIP** (compacta qualquer seleção); saem BMP, GIF, ICO e as predefinições.
- **v9.4.2** — menu circular redesenhado: Crispy com régua na borda e trecho aceso da fatia ativa; Minimalista
  Escuro no mesmo desenho, sem luzes; Minimalista Claro em porcelana com divisórias que somem e sombra difusa.
  Nomes com letras espaçadas e os lados vizinhos visíveis no centro.
- **v9.4** — **DesktopClock** integrado (ative em Configurações › DesktopClock): relógio na área de trabalho com
  qualquer fonte instalada, agenda com lembretes (Dispensar / +5 min), botão de atalho (ChatGPT ou qualquer app),
  **Pomodoro** por cores (foco/pausa: verde/vermelho, azul/cinza, lima/roxo, âmbar/céu) e **Roubo de tempo**
  (caveira: clique para contar quem te interrompeu, clique de novo para registrar; histórico do dia e da semana,
  planilha CSV). Ícones redesenhados no estilo Feather, ícone novo do programa, fonte Bahnschrift em todo o app
  (ou qualquer fonte do Windows em Configurações › Sistema › Fonte da interface).
- **v9.3** — Crispy cyberpunk: menu circular com disco preto fosco, vincos entre as fatias, centro rebaixado e
  fatia de vidro neon; **cor do neon escolhível** (6 cores prontas ou qualquer RGB) em Configurações › Sistema.
- **v9.2** — temas Minimalista Claro, Minimalista Escuro (preto × chumbo), Padrão Windows (cor de destaque do
  sistema) e Crispy (neon); destaque do menu circular em disco de vidro macio; preview com fundo de vidro;
  controles minimalistas; Pasta Padrão Maker cria "_Morto" dentro de cada pasta; botão direito ›
  **Limpar arquivos temporários** (TEMP com mais de 1 dia → Lixeira, depois oferece esvaziar a Lixeira;
  opção de excluir direto em Configurações › Limpeza).
- **v9.1** — WEBP com compressão máxima (método 6, YUV nítido; gráficos tentam sem perdas e fica o menor arquivo);
  menu circular em vidro líquido com brilho laranja que segue o mouse e granulado; ícones e botões minimalistas;
  temas Windows / Claro / Escuro / **Crispy** (preto chumbo, #FF6633, toque neon, fonte Bahnschrift);
  ícone na área de notificação com Configurações, Reiniciar e Encerrar.
- **v9** — menu circular com lados (converter, ferramentas, PDF, extras, predefinições); WEBP, TIFF, ICO, GIF, BMP;
  redimensionar, recortar, girar, espelhar, cinza, sem metadados; juntar/dividir/extrair PDF mantendo o original; comprimir PDF;
  camadas do PSD, paletas (folha, HEX/CSS, ASE⇄ACO), amostra de fonte; instalador oferece desinstalar ao ser reaberto.
- **v8.1** — instalador próprio (o Vizu se instala; sem NSIS), que não reinicia o Explorer; evita o alarme falso de "PUP" de antivírus.
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
  animação de abertura, `Vizu.exe /peek arquivo` para abrir um arquivo direto.
- **v4.2** — botões de conversão no Alt; 72/300 dpi; PDF comprimido ou sem compressão; ordem do PDF = ordem dos cliques.
- **v4.1** — botões de conversão passam do Shift para o Ctrl; Ctrl+clique reinicia a contagem.
- **v4** — painel de visualização (Alt+P), preview em segundo plano com cache e pré-carregamento dos vizinhos,
  conversão para PNG/JPG/PDF segurando Shift (v4.1: Ctrl, v4.2: Alt) (PDF multipáginas na ordem da seleção), configurações (engrenagem),
  fundo claro/escuro.
- **v3** — preview com zoom/arrastar, fundo xadrez, páginas do InDesign, barra de informações com fontes faltando,
  Enter/Ctrl+C, AI nítido via PDF, novos formatos (EPS completo, SVGZ, CDR, Affinity, XD, Sketch, Krita, Procreate, RAW de câmera),
  instalação sem administrador.
- **v2.1** — Vizu sem hook de teclado (WinEvents + atalho só na lista de arquivos), inicialização pela pasta Inicializar.
- **v2** — Vizu (preview com Espaço).
- **v1.1** — PSD lido sob demanda (arquivos grandes e na nuvem); miniaturas XMP truncadas ignoradas.
- **v1** — thumbnail provider inicial.
</details>

---

<p align="center">Vizu · by CrispyPixel · <a href="LICENSE">licença MIT</a></p>
<p align="center"><sub>O Vizu é gratuito. Você pode usar, copiar, modificar e redistribuir o programa, inclusive em trabalhos comerciais, desde que mantenha o aviso de copyright e a licença MIT (<a href="LICENSE">LICENSE</a>). Não há cobrança nem cadastro.</sub></p>
