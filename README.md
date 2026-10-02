# ExprEvaluator

Um visualizador, passo a passo, de como uma expressão matemática é avaliada.

Digite algo como `1 + (1 + 2) * 3` e o programa mostra, uma etapa de cada vez,
tudo o que acontece com o texto até ele virar `10`: os espaços saindo, os números
ganhando delimitadores, cada operação sendo envolvida pelos próprios parênteses,
agulhas percorrendo a string e medindo a profundidade de cada expressão, e as
expressões mais internas sendo empilhadas e apagadas do texto até não sobrar
nada. No fim, a pilha é consumida, e o resultado de cada passo alimenta o
seguinte.

Foi feito por tédio, a partir de uma ideia antiga. Não tem pretensão de ser
eficiente nem de substituir um parser de verdade. O modelo mental que ele usa é
extremamente arcaico (texto cru, ponteiros de `char` e parênteses), mas também
extremamente intuitivo, e a graça está justamente em ver cada etapa acontecer.

## O que ele entende

- `+`, `-`, `*` e `/`
- `^` para potência
- `$` para raiz quadrada: `$16`, `$(7 + 9)`
- `-` na frente de números, parênteses e raízes
- parênteses em qualquer profundidade

Cada expressão pode ter até 1024 caracteres.

## Uso

Abra `ExprEvaluator.slnx` no Visual Studio, compile e rode. O programa lê uma
expressão por linha, e `exit` encerra.

Durante a visualização:

| Tecla   | Ação                                 |
|---------|--------------------------------------|
| espaço  | próximo passo                        |
| enter   | conclui a etapa atual                |
| `a`     | autoplay                             |
| `+` `-` | acelera e desacelera o autoplay      |
| esc     | sai                                  |

Algumas preferências da visualização também podem ser escolhidas ao abrir o
programa, como a velocidade inicial do autoplay e o que as linhas de
profundidade mostram. `ExprEvaluator --help` lista as opções.
