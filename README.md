# PYRO — back-end nativo (código de máquina)

O **PYRO** é a camada "quente": recebe a AST do CRYO e **gera código nativo**,
falando direto com a máquina. Inclui os geradores de código, o runtime C e a
camada de máquina (skills nativas de LLM + acesso ao sistema).

## Conteúdo

| Arquivo | Papel |
|---|---|
| `codegen_go.py` | Backend **Go** (base atual) — linguagem completa + skills/máquina |
| `codegen_c.py` | Backend C nativo (+ instrumentação de segurança) |
| `codegen_asm.py` | Backend x86-64 (ABIs System V e Win64) |
| `codegen_legacy.py` | Backend Python legado (não usado pela CLI) |
| `runtime/cryo_runtime.c/.h` | Runtime C compartilhado (backends C/asm) |
| `PYRO.md` | Explicação da camada Pyro: pipeline, skills e acesso à máquina |

## Camada de máquina (skills + OS)

Skills de LLM são **compiladas no binário** (sem arquivos `.md`), com introspecção
nativa (`skills()`, `skill_get()`, `skills_json()`), e há builtins de acesso direto
à máquina (`pyro_exec`, `pyro_env`, `pyro_args`, `pyro_time`, …). Ver [PYRO.md](PYRO.md).

## Dependências

PYRO depende de `ast_nodes.py` do **CRYO** (importado como `from ast_nodes import *`).
Será distribuído como repositório próprio, consumindo o CRYO como dependência.
