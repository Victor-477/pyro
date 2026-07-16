# Pyro — a camada nativa de máquina

**Cryo** é a linguagem de alto nível. **Pyro** é a camada nativa em que a Cryo é
compilada — a parte que fala *diretamente com a máquina*: código nativo, acesso
ao sistema operacional e as *skills* de LLM embutidas no próprio binário.

> Regra mental: você **escreve Cryo**, o compilador **gera Pyro** (código nativo),
> e o Pyro **executa na máquina**. Cryo é o gelo (ergonômico, seguro); Pyro é o
> fogo (rápido, direto ao metal).

## Como funciona o pipeline

```
  app.cryo ──(lexer→parser→AST)──►  gerador de código  ──►  Pyro  ──►  binário
                                          │
                    ┌─────────────────────┼─────────────────────┐
                    ▼                     ▼                      ▼
              Go nativo (.go)       C nativo (.pyro)      x86-64 (.s)
              base atual            runtime C             ABIs win64/sysv
```

O nome do artefato C gerado é `.pyro` justamente porque é a forma "quente"/nativa
do programa. Hoje a **base é o backend Go**: a Cryo baixa para Go idiomático e usa
a stdlib madura (`os`, `os/exec`, `encoding/json`, goroutines) por baixo — sem
runtime externo. A instrumentação de segurança (overflow, divisão por zero,
`assert`, null-safety) é embutida no próprio código gerado.

## Skills nativas de LLM (sem arquivos `.md`)

Ferramentas de LLM costumam descrever *skills* em arquivos `SKILL.md` (markdown com
frontmatter). Para uma linguagem de máquina isso é o oposto de otimizado: exige I/O
de arquivo e parsing de texto em runtime. No Pyro, uma **skill é um construto da
linguagem**, compilado como struct nativo dentro do binário:

```cryo
skill resumir {
    desc:        "Resume um texto em bullets objetivos";
    model:       "gpt-x";
    temperature: 0.2;
    max_tokens:  512;
    tools:       ["contar_palavras"];
}
```

Isso vira, no binário, uma entrada num registro global `map[string]Skill` — zero
markdown, zero leitura de arquivo, introspecção O(1). Campos conhecidos (`desc`,
`model`, `tools`) viram campos tipados; os demais (`temperature`, `max_tokens`, …)
vão para um `Config map[string]string` compacto. O tipo `Skill` é serializável em
JSON nativamente (tags `json`).

### Introspecção nativa

| Função | Retorno | O que faz |
|---|---|---|
| `skills()` | `string[]` | Nomes de todas as skills (ordenados) |
| `skill_get(nome)` | `Skill` | A skill e sua configuração |
| `skill_has(nome)` | `bool` | Se a skill existe |
| `skills_json()` | `string` | Catálogo completo em JSON (para interop/LLM) |

```cryo
Skill s = skill_get("resumir");
print(s.desc);
print(s.model);
print(s.config["temperature"]);     // "0.2"
string catalogo = skills_json();      // exporta tudo em JSON, sem .md
```

Um agente/LLM lê o catálogo com `skills_json()` — um único valor compacto, gerado
em memória a partir de dados que já vivem no binário.

## Acesso direto à máquina

O Pyro expõe *builtins* que conversam com o sistema operacional (baixam para
`os`/`os/exec`/`time` do Go):

| Função | Retorno | Descrição |
|---|---|---|
| `pyro_exec(cmd)` | `string` | Executa um comando de shell e retorna a saída (stdout+stderr). Multiplataforma (`cmd /c` no Windows, `sh -c` nos demais) |
| `pyro_env(nome)` | `string` | Lê uma variável de ambiente |
| `pyro_args()` | `string[]` | Argumentos da linha de comando |
| `pyro_time()` | `int` | Timestamp atual (milissegundos Unix) |
| `pyro_write(s)` | — | Escreve em stdout sem quebra de linha |
| `pyro_read()` | `string` | Lê uma linha de stdin |
| `pyro_exit(cod)` | — | Encerra o processo com o código dado |

```cryo
string usuario = pyro_env("USERNAME");
string saida   = pyro_exec("go version");
int    agora   = pyro_time();
```

Para um agente de IA, `pyro_exec` é a primitiva-chave: a "tool" que a skill declara
pode ser um comando que o agente roda direto na máquina.

## Disponibilidade por backend

`skill`, as funções `skills*`/`skill_*` e os builtins `pyro_*` são recursos da
camada **Pyro sobre Go** (backend padrão). Os backends `c` e `asm` emitem um erro
claro apontando `--backend go` quando encontram esses construtos.

## Exemplo completo

Veja [`examples/example_pyro.cryo`](../examples/example_pyro.cryo): declara duas
skills, faz introspecção nativa e acessa a máquina — tudo compilado num único
binário, sem nenhum arquivo `.md`.
