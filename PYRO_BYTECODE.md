# Pyro — especificação do bytecode (`.pyro`)

O `.pyro` é a **linguagem-alvo própria** do sistema: um **bytecode binário** com um
conjunto de instruções (ISA) inventado aqui — **não é** x86, nem Go, nem C. É
gerado pelo **Burnout** (o compilador) e executado pela **VM Pyro** (em Go).

Modelo de execução: **máquina de pilha** (operandos numa pilha; funções com quadros
próprios de variáveis locais).

## Formato do arquivo (little-endian)

```
magic     4    "PYRO"
version   1    0x01
flags     1    bit0 = seção de código codificada (XOR rolling)
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
```

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
