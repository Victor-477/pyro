# Pyro — especificação do bytecode (`.pyro`)

O `.pyro` é a **linguagem-alvo própria** do sistema: um **bytecode binário** com um
conjunto de instruções (ISA) inventado aqui — **não é** x86, nem Go, nem C. É
gerado pelo **Burnout** (o compilador) e executado pela **VM Pyro** (em Go).

Modelo de execução: **máquina de pilha** (operandos numa pilha; funções com quadros
próprios de variáveis locais).

## Formato do arquivo (little-endian)

```
magic     4    "PYRO"
version   1    0x02
flags     1    bit0 = seção de código codificada (XOR rolling)
               bit1 = seção de depuração presente (pc → linha)
nconsts   u16
consts    nconsts × [ tag(1) + payload ]
              tag 1 int64   → 8 bytes
              tag 2 float64 → 8 bytes
              tag 3 string  → u16 len + bytes UTF-8
              tag 4 bool    → 1 byte
nfuncs    u16
funcs     nfuncs × [ nameidx u16, entry u32, nparams u8, nlocals u16 ]
entryfn   u16    índice da função 'main'
codelen   u32
code      codelen bytes   (decodificados na carga se flags bit0)
ndebug    u32            (só se flags bit1)
debug     ndebug × [ pc u32, line u32 ]   tabela pc → linha-fonte
```

> **v2** (a partir da Fase 5): os saltos (`JMP`/`JMPF`/`JMPT` e o `rel` do
> `TRYPUSH`) passaram de `i16` para **`i32`** — sem o limite de ±32 KB de
> código por função. E há a **seção de depuração** opcional: uma tabela
> `pc → linha`, que a VM usa para imprimir *stack traces* legíveis
> (função + linha de cada quadro ativo) ao abortar.

Constantes são deduplicadas. O `nameidx` de cada função referencia uma string no
pool. As strings do pool ficam em claro; **apenas a seção `code` é codificada**.

## Conjunto de instruções (ISA)

Cada instrução = 1 byte de opcode + operandos de tamanho fixo.

| Opcode | Hex | Operando | Efeito na pilha |
|---|---|---|---|
| `HALT`  | 00 | — | encerra |
| `CONST` | 01 | u16 idx | empilha `consts[idx]` |
| `TRUE`/`FALSE`/`NULL` | 02/03/04 | — | empilha literal |
| `POP`   | 05 | — | descarta o topo |
| `LOAD`  | 06 | u16 slot | empilha `local[slot]` |
| `STORE` | 07 | u16 slot | `local[slot] = pop()` |
| `ADD`…`MOD` | 10–14 | — | `pop b, a` → empilha `a op b` |
| `NEG`   | 15 | — | nega o topo |
| `BAND`…`SHR` | 16–1A | — | bit a bit (inteiros) |
| `BNOT`  | 1B | — | complemento de bits |
| `EQ`…`GE` | 20–25 | — | comparação → bool |
| `NOT`   | 26 | — | negação lógica |
| `JMP`   | 30 | i16 rel | salto relativo |
| `JMPF`/`JMPT` | 31/32 | i16 rel | `pop`; salta se falso/verdadeiro |
| `CALL`  | 40 | u16 fn, u8 argc | chama função (novo quadro) |
| `RET`   | 41 | — | retorna o topo ao chamador |
| `PRINT` | 50 | — | imprime `pop()` conforme o tipo |
| `ASSERT`| 51 | — | `pop cond, msg`; aborta se falso |
| `PRINTLN` | 52 | — | imprime linha vazia |
| `NEWARR` | 60 | u16 n | array a partir dos `n` valores do topo |
| `NEWMAP` | 61 | u16 n | map a partir de `n` pares (chave, valor) |
| `INDEX`/`SETIDX` | 62/63 | — | leitura/escrita indexada (com bounds-check) |
| `LEN` | 64 | — | tamanho de string/array/map |
| `APPEND` | 65 | — | `arr.push(v)`; empilha o novo tamanho |
| `HAS`/`KEYS` | 66/67 | — | existência de chave / array de chaves |
| `NATIVE` | 70 | u8 id, u8 argc | chama um builtin nativo da VM (tabela abaixo) |
| `TRYPUSH` | 71 | i16 rel, u16 slot | instala handler de exceção (catch em `rel`; `slot`=var, 0xFFFF=nenhuma) |
| `TRYPOP` | 72 | — | remove o handler do topo (try concluído sem exceção) |
| `THROW` | 73 | — | `pop` valor; desenrola pilha/quadros até o handler mais próximo |
| `COALESCE` | 74 | — | `pop b, a` → `a` se `a != null`, senão `b` (operador `??`) |
| `UNWRAP` | 75 | — | `pop a` → `a` se `a != null`, senão aborta (unwrap `x!`) |

### Builtins nativos (`NATIVE`)

A instrução `NATIVE` consome `argc` argumentos da pilha e empilha o resultado.
A tabela de ids é espelhada entre o gerador (`NATIVES` em `codegen_pyro.py`) e
a VM (`native()` em `vm/main.go`):

| id | nome | id | nome | id | nome |
|---|---|---|---|---|---|
| 0 | `sqrt` | 7 | `round` | 14 | `trim` |
| 1 | `pow` | 8 | `to_string` | 15 | `contains` |
| 2 | `abs` | 9 | `to_int` | 16 | `find` |
| 3 | `min` | 10 | `to_number` | 17 | `replace` |
| 4 | `max` | 11 | `remove` | 18 | `substr` |
| 5 | `floor` | 12 | `upper` | 19 | `split` |
| 6 | `ceil` | 13 | `lower` | 20 | `join` |
| | | | | 21 | `input` |
| | | | | 22 | `json_encode` |
| | | | | 23 | `json_decode` |

Enums não geram código: cada membro (`Nivel_ALTO`) vira uma constante inteira
em tempo de compilação.

Saltos são **relativos** ao fim da própria instrução (`rel = alvo − (pc_após_operando)`).

## Tipagem dinâmica na VM

Os valores carregam o tipo em runtime (int64, float64, bool, string, null). As
operações são resolvidas pela VM:

- `ADD` com algum operando string → **concatenação** (o outro é convertido).
- Aritmética com algum `float` → promoção para float; senão, inteiro.
- `EQ`/`NE` comparam por valor entre tipos compatíveis.
- **Segurança:** `DIV`/`MOD` inteiros por zero **abortam** (`[Cryo Seguranca] DivisaoPorZero`);
  `ASSERT` aborta com a mensagem se a condição for falsa.

## Codificação da seção de código (flags bit0)

Ofuscação leve (**não** é criptografia forte) por XOR *rolling*: chave inicial
`0x5A`, atualizada por byte com `k = (k·31 + 7 + b) & 0xFF`, onde `b` é o byte em
claro. A VM aplica o inverso ao carregar. Serve para tornar o `.pyro` opaco a
leitura casual e compacto; para sigilo real, cifre o artefato à parte.

## Por que um bytecode próprio?

- **Roda na máquina** via a VM Pyro (portável: um binário Go).
- **Compacto/opaco** ("criptografado") por natureza — bom para distribuição.
- **Linguagem nativa do sistema** — instruções bem definidas, ótimas como dados de
  treino para agentes de IA (a IA lê instruções já na forma que a máquina executa).
- **Própria**, não derivada de x86/Go/C.

## Referências no código

- Gerador: [`burnout/codegen_pyro.py`](../burnout/codegen_pyro.py) (opcodes e serialização).
- VM: [`pyro/vm/main.go`](vm/main.go) (carga, decodificação e execução).
