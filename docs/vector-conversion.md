# Conversão vetorial CDR/SVG → SVG/EPS

## Problema corrigido

O fluxo anterior `jobs::Run → Frames → RenderFile → ops::SaveSvg/SaveEps` renderizava o documento antes de exportar. `SaveSvg` criava um `<image>` com PNG em base64; `SaveEps` escrevia um JPEG com o operador PostScript `image`. Nenhum deles preservava os objetos originais.

Isso explica o EPS como imagem. O SVG em branco relatado pelo usuário não foi reproduzido com o arquivo original; a causa específica pode estar na prévia/alpha ou na leitura da imagem embutida. A alteração elimina essa dependência para as exportações vetoriais.

## Novo fluxo

`jobs::Run → vec::Convert → Inkscape (arquivo original) → SVG auditado → SVG/EPS`

- Implementação C++ em `VectorConvert.h`; sem Python, servidor ou interface adicional.
- O caminho vetorial acontece antes de `Frames()`. As miniaturas e os outros formatos continuam no fluxo existente.
- SVG/EPS só são oferecidos no menu para seleções inteiramente compostas por CDR/SVG. Não há vetorização automática de fotos, thumbnails, PSD, AI ou PDF nesta implementação.
- Em Configurações, **Texto em SVG / EPS** permite escolher texto editável ou curvas. As fontes originais precisam estar instaladas; curvas não corrigem uma substituição ocorrida durante a importação.
- O SVG é lido com XmlLite, DTD proibido, limite de profundidade e verificação de objetos desenháveis/referências. Bitmaps, scripts, estilos em blocos e recursos externos são recusados.
- EPS bloqueia transparências, filtros, máscaras, padrões, gradientes e múltiplas páginas detectadas. Alguns gradientes poderiam ser vetoriais; aqui são recusados de modo conservador. Não se usa achatamento raster.
- O corpo do EPS gerado é inspecionado para operadores de imagem. Essa inspeção não é um interpretador PostScript nem certificação de conformidade.
- Processos ocultos, argumentos escapados sem shell, stdout/stderr capturados, Job Object e timeout por etapa. Uma cópia temporária do original é usada durante todo o processamento.
- O destino existente nunca é substituído. Um relatório `<saída>.vizu.txt` acompanha a saída quando puder ser gravado; erros vão para um diagnóstico no Vizu.

## Edições e motor vetorial (9.8)

O instalador (~2 MB) oferece três edições, gravadas em `HKCU\Software\DesignThumbs\Edition`:
**0 = Vizu Lite** (prévia com Espaço e miniaturas), **1 = Vizu** (+ menu circular, conversões, OCR, relógio),
**2 = Vizu Ultra** (+ SVG/EPS vetorial). Sem a chave (atualizações antigas), quem tinha `VectorConverterInstalled=1`
vira Ultra e os demais Vizu: nenhuma atualização silenciosa baixa o motor sem o usuário ter escolhido.

No Ultra, o instalador baixa o Inkscape 1.4.2 portátil oficial de `media.inkscape.org`, confere o SHA-256 fixado em
`VectorBundle.h`, extrai com o `tar.exe` do Windows numa pasta de preparação, verifica e move para
`vector-engine/inkscape`. Se o motor já está na versão certa (`VIZU-BUNDLE.txt`), atualizações não baixam nem extraem
de novo. Trocar para Lite/Vizu remove o motor. A desinstalação remove a pasta privada do motor.

Auditoria: bitmaps embutidos (`data:image/png|jpeg`) são aceitos e mantidos como imagem; referências externas continuam
recusadas. No EPS, gradientes são permitidos (o Cairo os escreve como shading PostScript); transparências, filtros,
máscaras e padrões continuam bloqueados. Erros de conversão aparecem como aviso curto; o detalhe vai para
`%TEMP%\Vizu-conversao.txt`.

## Build e testes

O build MinGW liga `-lxmllite -lbcrypt`; o motor não é embutido. O workflow `vector-bundle-tests.yml` testa download com SHA-256, extração, descoberta, conversões SVG/EPS, reinstalação e remoção em uma pasta isolada, sem registrar o Vizu no sistema. O workflow `vector-tests.yml` compila e executa os testes de auditoria no Windows; o workflow de build original continua compilando o aplicativo completo no Linux/MinGW.

Em um Developer Command Prompt do Visual Studio:

```bat
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /DUNICODE /D_UNICODE tests\vector_tests.cpp /Fe:vector_tests.exe /link shlwapi.lib ole32.lib xmllite.lib
vector_tests.exe
vector_tests.exe "C:\Program Files\Inkscape\bin\inkscape.exe" "C:\amostras\vetores.svg" "C:\amostras\saida-nova"
```

O último comando é opcional e testa o motor real, gera `.svg`/`.eps` e verifica que não podem ser sobrescritos. Use uma base de saída inexistente e um SVG sem efeitos incompatíveis com EPS. O CDR pode ser usado no lugar do SVG, mas o resultado depende do conteúdo e do motor.

`Extra.h` também renomeia uma variável local chamada `small`, para evitar colisão com a macro do Windows SDK ao compilar com MSVC; o comportamento da prévia de fontes é o mesmo.

## Limites de fidelidade

Inkscape/libcdr interpreta os dados CDR. A aplicação valida o resultado, mas não vê os objetos que o importador eventualmente omitiu. A cobertura de versões CDR, efeitos proprietários, fontes, cores CMYK/especiais/ICC e multipágina não está certificada. A saída SVG não garante separações de cor de pré-impressão. EPS pode reorganizar grupos e textos.

Foram verificados auditoria, conversão real de um exemplo SVG para SVG/EPS, bloqueio de bitmaps e proteção contra sobrescrita. Ainda é necessário testar o CDR do usuário, comparar com uma exportação do CorelDRAW e abrir o resultado no CorelDRAW/Illustrator. Não atribuir a este patch uma certificação de fidelidade entre versões CDR.

Fontes: [CLI do Inkscape](https://wiki.inkscape.org/wiki/Using_the_Command_Line), [libcdr](https://github.com/LibreOffice/libcdr), [XmlLite](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ms752872(v=vs.85)).
