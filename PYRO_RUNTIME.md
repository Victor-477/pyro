# Pyro Runtime — Especificação do runtime mínimo

Este documento especifica o **runtime** da linguagem-alvo Pyro: o modelo de
valores, o gerenciamento de memória, a semântica de containers/strings, a
E/S, os builtins nativos (`NATIVE`) e o contrato de erros/abortos.

O runtime é **independente do motor** de execução. Hoje há duas
implementações do runtime que devem ser **semanticamente idênticas**:

| Implementação | Arquivos | Toolchain |
|---|---|---|
| Runtime C (isolado) | `pyro/vm/pyro_runtime.h` + `pyro_runtime.c` | gcc / MinGW / MSVC |
| Runtime Go (embutido na VM Go) | `pyro/vm/main.go` | Go |

A VM em C (`pyro/vm/main.c`) é apenas o **motor** (carregamento, decodificação
e laço de despacho); ela depende somente de `pyro_runtime.h`. Futuros alvos
(ex.: código C nativo, WASM) reutilizam o mesmo runtime e herdam esta
semântica sem reimplementá-la.

> A ISA (opcodes, formato `.pyro`) é especificada em [`PYRO_BYTECODE.md`](PYRO_BYTECODE.md).
> Aqui tratamos apenas do runtime.

---

## 1. Modelo de valores

Um `Value` é dinâmico e carrega sua tag de tipo em runtime:

| Tipo | Representação | Observações |
|---|---|---|
| `int` | inteiro com sinal de 64 bits (`int64`) | overflow de `+ - *` aborta (fail-fast) |
| `float` | ponto flutuante de 64 bits (`double`) | |
| `bool` | booleano | |
| `string` | UTF-8, imutável, contada por referência | comprimento em **bytes** |
| `null` | ausência de valor | |
| `array` | lista contígua de `Value`, contada por referência | |
| `map` | tabela hash `Value → Value`, contada por referência | chaves por igualdade de valor |

Não há um tipo `struct` distinto: **structs e variantes de enum são maps** de
chaves string (ex.: `Ok(v)` → `{"tag": "Ok", "val0": v}`). Enums sem dados são
constantes inteiras resolvidas em tempo de compilação.

### Promoção numérica
Operações mistas `int`/`float` promovem o inteiro a `float`; o resultado é
`float`. Entre dois `int`, a aritmética é inteira (divisão trunca em direção a
zero).

### Igualdade (`value_eq`)
- Mesma categoria numérica compara por valor (com promoção `int`/`float`).
- `string` compara por conteúdo; `bool` por valor; `null == null` é verdadeiro.
- `array`/`map` comparam por **identidade de referência**.

### Veracidade (`value_truthy`)
`false`, `null`, `0` (int), `0.0` (float) e `""` são falsos; o resto é verdadeiro.

---

## 2. Gerenciamento de memória (contagem de referências)

`string`, `array` e `map` são objetos contados por referência (`ref_count`).
`int`, `float`, `bool` e `null` são valores imediatos (sem alocação).

Invariantes:

- **`retain_value(v)`** incrementa o `ref_count` do objeto (no-op para imediatos).
- **`release_value(v)`** decrementa; ao chegar a zero, libera o objeto e
  **release recursivo** de seus elementos (array) ou pares chave/valor (map).
- Um objeto recém-criado nasce com `ref_count = 1`.
- Ao **guardar** um valor num slot/container que passa a possuí-lo, faz-se
  `retain`; ao **sobrescrever/descartar**, faz-se `release`.
- Não há coletor de ciclos: como arrays/maps comparam por identidade e a
  linguagem não expõe mutação que crie ciclos de posse, refcount é suficiente.

> A semântica de referência de arrays/maps (aliasing por referência) é parte
> do contrato: passar um array a uma função compartilha o mesmo objeto.

---

## 3. Strings

- Imutáveis; comprimento medido em **bytes** (`len(s)`).
- Indexação `s[i]` devolve o byte `i` como string de 1 caractere; índice fora
  da faixa **aborta** (ver §6).
- Concatenação `a + b` com pelo menos um operando string converte o outro via
  `value_to_string` e produz uma nova string.
- `value_to_string` define a forma textual canônica de cada tipo (usada por
  `print`, concatenação e `json_encode` de chaves).

---

## 4. Containers

### Arrays
- `arr.push(v)` acrescenta ao fim (cresce a capacidade conforme necessário).
- `len(arr)` devolve o número de elementos.
- `arr[i]` lê/escreve por índice; fora da faixa **aborta** (fail-fast).

### Maps
- Tabela hash com redimensionamento; chaves comparadas por `value_eq`.
- `m[k]` lê (chave ausente → `null`) e escreve; `has(m, k)` testa a presença;
  `remove(m, k)` remove; `keys(m)` devolve as chaves **ordenadas pela forma
  textual** (determinístico — garante paridade entre implementações).

---

## 5. E/S e builtins nativos (`NATIVE`)

A instrução `NATIVE id, argc` consome `argc` valores da pilha e empilha o
resultado. A tabela de ids é **espelhada** entre o gerador
(`NATIVES` em `burnout/codegen_pyro.py`) e cada runtime:

| id | nome | id | nome | id | nome |
|---|---|---|---|---|---|
| 0 | `sqrt` | 9 | `to_int` | 18 | `substr` |
| 1 | `pow` | 10 | `to_number` | 19 | `split` |
| 2 | `abs` | 11 | `remove` | 20 | `join` |
| 3 | `min` | 12 | `upper` | 21 | `input` |
| 4 | `max` | 13 | `lower` | 22 | `json_encode` |
| 5 | `floor` | 14 | `trim` | 23 | `json_decode` |
| 6 | `ceil` | 15 | `contains` | 24 | `http_get` |
| 7 | `round` | 16 | `find` | 25 | `http_post` |
| 8 | `to_string` | 17 | `replace` | 26 | `sleep` |

- **`input(prompt)`** lê uma linha de stdin (I/O).
- **`json_encode`/`json_decode`** serializam/desserializam a árvore de valores
  (chaves de objeto viram maps; inteiros JSON viram `int`).
- **`http_get`/`http_post`** fazem requisições de rede; **`sleep(ms)`** pausa.
- `to_int`/`to_number` de string não numérica **abortam** (fail-fast).

### Política de sandbox
O runtime expõe `pyro_sandboxed` (ligado pelo host via flag `bit2` do `.pyro`
ou `PYRO_SANDBOX=1`). Quando ativo, os nativos de **rede** (`http_get`,
`http_post`) são recusados com abort de segurança. `sleep` permanece liberado.

---

## 6. Contrato de erros e abortos

Há dois regimes:

- **Fail-fast (não capturável)** — segurança de baixo nível: overflow de
  inteiro, divisão/módulo por zero, índice fora da faixa (array/string),
  `to_int`/`to_number` inválidos, sandbox. Chamam `fatal()`.
- **Capturável (`try`/`catch`)** — `throw`, `assert` que falha e `unwrap`
  (`x!`) de `null`. Levantam uma exceção; se não houver handler ativo, viram
  `fatal()`.

`fatal(msg)` é o **callback do host**: imprime `"[Pyro VM] " + msg` em stderr,
seguido do stack trace (se houver seção de depuração), e encerra com código 1.
As mensagens são padronizadas e **idênticas** entre as implementações:

```
[Pyro VM] [Cryo Seguranca] DivisaoPorZero: divisão inteira
  stack trace (mais recente primeiro):
    em divide (linha 2)
    em main (linha 8)
```

Mensagens canônicas (prefixo `[Cryo Seguranca]` para segurança):
`DivisaoPorZero: divisão inteira` / `: módulo`; `Overflow: INT64_MIN / -1`;
`IndexError: índice N fora dos limites (len=M)` (array get), `IndexError:
índice N fora dos limites` (array set), `IndexError: índice de string fora dos
limites`; `unwrap de valor nulo`; `to_int: '…' não é um inteiro válido`;
`to_number: '…' não é um número válido`; `Sandbox: http_get() bloqueado por
política de sandbox`. Fora de segurança: `[Cryo Assert] <msg>` e `exceção não
capturada: <valor>`.

---

## 7. Fronteira runtime ↔ host

O runtime é agnóstico ao motor; ele só depende de:

| Símbolo | Direção | Papel |
|---|---|---|
| `void fatal(const char* msg)` | host → runtime | aborta com mensagem + stack trace |
| `bool pyro_sandboxed` | host define, runtime lê | política de sandbox |

Tudo o mais (pilha de operandos, quadros de chamada, handlers de exceção,
seção de depuração, decodificação e despacho) pertence ao **motor** e não é
visível ao runtime. Assim, qualquer motor (VM de pilha, tradutor para C
nativo, etc.) que forneça esses dois símbolos obtém semântica idêntica.
