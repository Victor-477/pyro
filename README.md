# PYRO — a linguagem-alvo nativa (`.pyro`)

O **Pyro** é a **linguagem-alvo própria** do sistema: um **bytecode binário** com um
conjunto de instruções (ISA) inventado aqui — **não é** x86, Go nem C. O compilador
(**Burnout**) gera `.pyro` a partir de `.cryo`, e a **VM Pyro** (em Go) carrega e
executa esse bytecode na máquina.

```
  .cryo ──►  Burnout (compilador)  ──►  .pyro (bytecode próprio)  ──►  VM Pyro (Go)  ──►  execução
```

## Conteúdo

| Caminho | Papel |
|---|---|
| `vm/main.go` · `vm/go.mod` | A **VM Pyro** (em Go): carrega, decodifica e executa `.pyro` |
| `PYRO_BYTECODE.md` | Especificação da ISA e do formato do `.pyro` |
| `PYRO.md` | Notas de rumo da camada nativa (histórico) |

## O que a VM suporta

- Escalares: `int`, `number` (float64), `bool`, `string` (tipagem dinâmica).
- Aritmética, comparação, bit a bit, lógica (curto-circuito), unários.
- Variáveis, `if/else`, `while`, `do/while`, `for`, `for-each`, `switch`, ternário,
  `break`/`continue`.
- Funções, recursão, chamadas (quadros de pilha próprios).
- **Containers**: arrays (`[...]`, `push`, indexação, `len`), maps (`{k:v}`, `has`,
  `keys`, indexação), structs (= map de campos, `s.campo`).
- Segurança: divisão por zero e `assert` abortam; índices fora dos limites abortam.

## Por que é sua própria linguagem

- **Roda na máquina** via a VM (portável: um binário Go).
- **Compacta/opaca** ("criptografada") — a seção de código é codificada (XOR rolling).
- **Nativa do sistema**, ótima como dado de treino para agentes de IA (instruções já
  na forma que a máquina executa).
- **Própria** — não derivada de linguagens existentes.

Especificação completa da ISA e do formato em [PYRO_BYTECODE.md](PYRO_BYTECODE.md).
Desassemblador (`.pyro` → texto legível) no compilador: `--backend pyro --dis`.

## Dependências

A VM é **autocontida** (só a biblioteca padrão do Go). O `.pyro` que ela executa é
produzido pelo **Burnout**. Distribuída como repositório próprio (a linguagem-alvo).
