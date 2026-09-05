Cria a branch:
  git switch -c nova_branch
  ou
  git branch nova_branch
  git switch nova_branch

Renomeia branch
 git branch -m master main

Quando terminar volta para main: 
  git switch main
Mescla:
  git merge nova_branch
  git merge -no-ff nova_branch
Apaga a branch:
  git branch -d nova_branch
Apaga do github:
  git push origin --delete feature/dashboard-web

Restaura para versao antes do commit
  git restore main.c

Releases:
  git tag -a v1.0.0 -m "Primeira versão pública"
  git push origin v1.0.0
  
  No GitHub:
    Vá em Releases
    Create a release
    Escolha a tag v1.0.0
    Escreva as notas
    Publique

Logs:
  git log --oneline
  git log --graph --oneline --decorate --all
  git log --merges
  git log --merges --oneline
  git reflog

Recuperações:
  git switch --detach HASH    // Viaja no tempo
  git tag -a instr-add -m "Implementa instrução ADD"
  git switch --detach instr-add

  git switch main
  git reset --hard HASH       // Volta e apaga o que veio depois (perigoso)