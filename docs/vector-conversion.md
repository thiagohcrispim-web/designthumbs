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

## Motor incluído no instalador

A partir da **9.6.3-beta.2**, o instalador inclui o pacote portátil completo do Inkscape 1.4.2 (Windows x64). O usuário não instala, baixa nem configura outro programa.

O build obtém o arquivo oficial por HTTPS e verifica o SHA-256 fixado em `tools/prepare_vector_engine.py`. O pacote é embutido como recurso do instalador. Durante a instalação:

1. O pacote é extraído em uma pasta de preparação, usando o `tar.exe` nativo do Windows 10 1803+/Windows 11, sem shell ou rede.
2. O instalador verifica executável, biblioteca CDR, licenças e inicialização do motor.
3. A pasta completa é colocada em `vector-engine/inkscape` ao lado do Vizu instalado. Na reinstalação, o motor anterior é preservado até a nova pasta estar pronta; a troca é revertida se falhar.
4. O Vizu prioriza esse motor automaticamente, antes de variáveis de ambiente ou instalações externas. Nenhum ícone ou associação do Inkscape é registrado.
5. A desinstalação remove a pasta privada do motor. A remoção não percorre junctions/symlinks e só aceita caminhos de motor filhos da pasta do aplicativo.

A extração tem uma tela de progresso e pode levar alguns instantes. O instalador é maior porque contém o motor completo; a instalação não depende de conexão à internet. O SHA-256 fixado, o pacote original e a procedência estão em `third_party/inkscape/NOTICE.md`. As licenças originais ficam preservadas dentro do pacote instalado, em `vector-engine/inkscape/share/inkscape/doc`.

O instalador instalado continua contendo os recursos embutidos; portanto o espaço ocupado inclui o arquivo comprimido e a pasta extraída. O ZIP de distribuição contém o instalador completo. Apenas executar um EXE fora da instalação com `/portable` não extrai os recursos automaticamente; use a instalação normal para preparar o motor.

Para desenvolvimento, continuam disponíveis os fallbacks `VIZU_INKSCAPE` e a instalação padrão do Inkscape. A versão distribuída ao usuário usa primeiro o motor privado.

## Build e testes

O build MinGW existente prepara/verifica o pacote do motor e liga `-lxmllite`. O workflow `vector-bundle-tests.yml` testa extração offline, descoberta, conversões SVG/EPS, reinstalação e remoção em uma pasta isolada, sem registrar o Vizu no sistema. O workflow `vector-tests.yml` compila e executa os testes de auditoria no Windows; o workflow de build original continua compilando o aplicativo completo no Linux/MinGW.

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
