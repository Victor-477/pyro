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
| `PYRO.md` | Visão da camada nativa (pipeline, skills, acesso à máquina) |

## Por que é sua própria linguagem

- **Roda na máquina** via a VM (portável).
- **Compacta/opaca** ("criptografada") por natureza — a seção de código é codificada.
- **Nativa do sistema**, ótima como dado de treino para agentes de IA (instruções já
  na forma que a máquina executa).
- **Própria** — não derivada de linguagens existentes.

Detalhes completos em [PYRO_BYTECODE.md](PYRO_BYTECODE.md).

## Dependências

A VM é **autocontida** (só Go padrão). O `.pyro` que ela executa é produzido pelo
**Burnout**. Será distribuído como repositório próprio (a linguagem-alvo + a VM).
