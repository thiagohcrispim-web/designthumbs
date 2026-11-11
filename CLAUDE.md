# Publicação de versões do Vizu

A publicação é feita pelo workflow `.github/workflows/release.yml`.

Quando o usuário pedir uma nova versão:

1. Atualize a versão nos fontes e recursos e gere `dist/Vizu-Setup.exe`.
2. Atualize `latest.json`, incluindo o SHA-256 exato de `dist/Vizu-Setup.exe`, e o histórico no README.
3. Execute `python3 .github/scripts/release.py check` antes de enviar.
4. Envie o código pela branch de trabalho e integre à `main` conforme a autorização do usuário.
5. Aguarde Build, Bundled vector engine tests e Vector conversion tests do mesmo commit da `main`.
6. O GitHub cria a tag `vX.Y.Z`, anexa o instalador verificado e publica a release automaticamente.
7. Confira o workflow Publish Vizu release e o anexo antes de dizer que a publicação terminou.

Não peça ao usuário para criar tags ou anexar instaladores manualmente. Não tente enviar tags pelo proxy do Claude na nuvem. Se a automação falhar, leia o erro e corrija a causa. Nunca reutilize uma versão publicada para um commit diferente: incremente a versão. Não force tags, não sobrescreva anexos diferentes e não adicione tokens pessoais ao repositório.
