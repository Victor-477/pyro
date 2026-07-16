# ============================================================
#  Cryo Compiler - C Code Generator  (v0.3)
#  .cryo  ->  .pyro  (C nativo, compilavel com gcc/clang)
# ============================================================
from ast_nodes import *
from typing import List, Dict, Optional, Set


class CodeGenError(Exception):
    pass

# ── Mapeamento de tipos Cryo -> C ───────────────────────────

C_TYPE: Dict[str, str] = {
    'int':    'int64_t',
    'number': 'double',
    'string': 'char*',
    'bool':   'bool',
    'void':   'void',
    'null':   'void*',
}

def c_type(t: str) -> str:
    if t and (t.startswith('map<') or t.endswith('?')):
        raise CodeGenError(
            f"tipo '{t}' (map/opcional) ainda não é suportado no backend C; "
            f"use --backend go.")
    if t and t.endswith('[]'):
        return 'CryoArray*'
    return C_TYPE.get(t, t)          # tipos de struct passam diretamente

def elem_type(arr_t: str) -> str:
    return arr_t[:-2] if arr_t.endswith('[]') else 'unknown'


# ── Inferencia de tipos ──────────────────────────────────────

class TypeEnv:
    def __init__(self):
        self._scopes: List[Dict[str, str]] = [{}]
        self._fns:    Dict[str, str] = {}
        self._structs:Dict[str, Dict[str, str]] = {}
        self._enums:  Set[str] = set()

    def push(self): self._scopes.append({})
    def pop(self):  self._scopes.pop()

    def set(self, name: str, typ: str): self._scopes[-1][name] = typ

    def get(self, name: str) -> str:
        for s in reversed(self._scopes):
            if name in s:
                return s[name]
        return 'unknown'

    def reg_fn(self, name: str, ret: str): self._fns[name] = ret
    def fn_ret(self, name: str) -> str:    return self._fns.get(name, 'unknown')

    def reg_struct(self, name: str, fields: Dict[str, str]):
        self._structs[name] = fields

    def struct_field(self, struct: str, field: str) -> str:
        return self._structs.get(struct, {}).get(field, 'unknown')

    def reg_enum(self, name: str): self._enums.add(name)
    def is_enum(self, name: str) -> bool: return name in self._enums

    def infer(self, node) -> str:
        if node is None: return 'unknown'
        if isinstance(node, Literal):
            return {'int': 'int', 'float': 'number',
                    'string': 'string', 'bool': 'bool',
                    'null': 'null'}.get(node.kind, 'unknown')
        if isinstance(node, Identifier):
            return self.get(node.name)
        if isinstance(node, BinaryExpr):
            if node.op in ('==', '!=', '<', '>', '<=', '>=', '&&', '||'):
                return 'bool'
            lt = self.infer(node.left)
            rt = self.infer(node.right)
            if lt == 'string' or rt == 'string': return 'string'
            if lt == 'number' or rt == 'number': return 'number'
            return lt if lt != 'unknown' else rt
        if isinstance(node, UnaryExpr):
            return 'bool' if node.op == '!' else self.infer(node.operand)
        if isinstance(node, TernaryExpr):
            t = self.infer(node.then_value)
            return t if t != 'unknown' else self.infer(node.else_value)
        if isinstance(node, CallExpr):
            if node.callee in ('floor', 'ceil', 'round', 'min', 'max'):
                if node.callee in ('min', 'max'):
                    return self.infer(node.args[0]) if node.args else 'number'
                return 'number'
            return self.fn_ret(node.callee)
        if isinstance(node, StructInit):
            return node.struct_name
        if isinstance(node, ArrayLiteral):
            return 'array'
        if isinstance(node, FieldAccess):
            ot = self.infer(node.obj)
            if node.field == 'length': return 'int'
            return self.struct_field(ot, node.field)
        if isinstance(node, IndexAccess):
            at = self.infer(node.obj)
            return elem_type(at)
        return 'unknown'


# ── CodeGen C ────────────────────────────────────────────────

_PUSH_FN = {
    'int': 'cryo_push_i64', 'number': 'cryo_push_f64',
    'string': 'cryo_push_str', 'bool': 'cryo_push_bool',
}
_GET_FN = {
    'int': 'cryo_get_i64', 'number': 'cryo_get_f64',
    'string': 'cryo_get_str', 'bool': 'cryo_get_bool',
}


class CodeGenC:
    def __init__(self, safe: bool = True):
        self.te = TypeEnv()
        self._indent = 0
        self._type_decls:   List[str] = []
        self._fwd_decls:    List[str] = []
        self._global_decls: List[str] = []
        self._fn_defs:      List[str] = []
        self._main_stmts:   List[str] = []
        self._cur:          List[str] = self._main_stmts
        # ── seguranca ──
        self._safe_default = safe          # modo --safe global
        self._safe_stack:  List[bool] = [] # override por blocos safe/unsafe
        self._loop_depth = 0               # valida break/continue
        self._fe = 0                       # contador de indices de for-each

    @property
    def _safe(self) -> bool:
        return self._safe_stack[-1] if self._safe_stack else self._safe_default

    # ── emissao ──────────────────────────────────────────────

    def _pad(self) -> str:
        return '    ' * self._indent

    def _emit(self, line: str = ''):
        self._cur.append(self._pad() + line if line else '')

    # ── entrada principal ────────────────────────────────────

    def generate(self, program: Program) -> str:
        self._pre_scan(program.statements)

        for node in program.statements:
            if isinstance(node, (StructDecl, EnumDecl)):
                self._cur = self._type_decls
                self._indent = 0
                self._gen(node)
            elif isinstance(node, FunctionDecl):
                self._cur = self._fn_defs
                self._indent = 0
                self._gen(node)
            elif isinstance(node, ConstDecl):
                self._cur = self._global_decls
                self._indent = 0
                self._gen(node)
            elif isinstance(node, (Import, Library)):
                self._cur = self._global_decls
                self._indent = 0
                self._gen(node)
            else:
                self._cur = self._main_stmts
                self._indent = 1
                self._gen(node)

        return self._assemble()

    def _pre_scan(self, stmts: List[Node]):
        """Primeiro passo: registrar tipos e gerar prototipos."""
        for n in stmts:
            if isinstance(n, StructDecl):
                self.te.reg_struct(n.name, {f.name: f.field_type for f in n.fields})
            elif isinstance(n, EnumDecl):
                self.te.reg_enum(n.name)
            elif isinstance(n, FunctionDecl):
                ret = n.return_type or 'void'
                self.te.reg_fn(n.name, ret)
                params_c = ', '.join(
                    f"{c_type(pt)} {pn}" for pt, pn in n.params
                ) or 'void'
                self._fwd_decls.append(f"{c_type(ret)} {n.name}({params_c});")
            elif isinstance(n, ConstDecl):
                self.te.set(n.name, n.var_type)

    def _assemble(self) -> str:
        lines = [
            "/* ================================================",
            " * [PYRO] Compilado de Cryo -> C nativo  (v0.3)",
            " * Compilar: gcc -O2 arquivo.pyro cryo_runtime.c -lm -o programa",
            " * ================================================ */",
            "",
            '#include "cryo_runtime.h"',
            "",
        ]
        if self._type_decls:
            lines += ["/* -- Tipos -- */", ""] + self._type_decls + [""]
        if self._fwd_decls:
            lines += ["/* -- Prototipos -- */", ""] + self._fwd_decls + [""]
        if self._global_decls:
            lines += ["/* -- Globais -- */", ""] + self._global_decls + [""]
        if self._fn_defs:
            lines += ["/* -- Funcoes -- */", ""] + self._fn_defs + [""]
        lines += [
            "/* -- Entrada principal -- */",
            "int main(void) {",
        ] + self._main_stmts + [
            "    return 0;",
            "}",
            "",
        ]
        return '\n'.join(lines)

    # ── statements ──────────────────────────────────────────

    def _gen(self, node: Node):
        if   isinstance(node, StructDecl):         self._struct(node)
        elif isinstance(node, EnumDecl):            self._enum(node)
        elif isinstance(node, FunctionDecl):        self._fn(node)
        elif isinstance(node, VarDecl):             self._var(node)
        elif isinstance(node, ConstDecl):           self._const(node)
        elif isinstance(node, Assignment):          self._assign(node)
        elif isinstance(node, CompoundAssignment):  self._compound(node)
        elif isinstance(node, Increment):           self._incr(node)
        elif isinstance(node, Return):              self._return(node)
        elif isinstance(node, If):                  self._if(node)
        elif isinstance(node, While):               self._while(node)
        elif isinstance(node, DoWhile):             self._do_while(node)
        elif isinstance(node, For):                 self._for(node)
        elif isinstance(node, ForEach):             self._foreach(node)
        elif isinstance(node, TryCatch):            self._try(node)
        elif isinstance(node, Break):               self._break(node)
        elif isinstance(node, Continue):            self._continue(node)
        elif isinstance(node, Switch):              self._switch(node)
        elif isinstance(node, Assert):              self._assert(node)
        elif isinstance(node, SafetyBlock):         self._safety(node)
        elif isinstance(node, Import):              self._import(node)
        elif isinstance(node, Library):             self._library(node)
        elif isinstance(node, ForeignBlock):        self._foreign(node)
        elif isinstance(node, (IndexAssignment, MapLiteral, CastExpr, UnwrapExpr)):
            raise CodeGenError(
                f"'{type(node).__name__}' (map/JSON/opcional) ainda não é "
                f"suportado no backend C; use --backend go.")
        elif isinstance(node, SkillDecl):
            raise CodeGenError(
                "declaração 'skill' faz parte da camada Pyro e só existe no "
                "backend Go; use --backend go.")
        elif isinstance(node, (CallExpr, MethodCallExpr)):
            self._emit(self._expr(node) + ';')
        else:
            self._emit(f"/* UNSUPPORTED: {type(node).__name__} */")

    def _struct(self, n: StructDecl):
        self._emit(f"typedef struct {{")
        for f in n.fields:
            self._emit(f"    {c_type(f.field_type)} {f.name};")
        self._emit(f"}} {n.name};")
        self._emit()

    def _enum(self, n: EnumDecl):
        members = ', '.join(f"{n.name}_{m}" for m in n.members)
        self._emit(f"typedef enum {{ {members} }} {n.name};")
        self._emit()

    def _fn(self, n: FunctionDecl):
        ret = c_type(n.return_type or 'void')
        params = ', '.join(
            f"{c_type(pt)} {pn}" for pt, pn in n.params
        ) or 'void'
        self._emit(f"{ret} {n.name}({params}) {{")
        self._indent = 1
        self.te.push()
        for pt, pn in n.params:
            self.te.set(pn, pt)
        for s in n.body:
            self._gen(s)
        self.te.pop()
        self._indent = 0
        self._emit("}")
        self._emit()

    def _var(self, n: VarDecl):
        self.te.set(n.name, n.var_type)
        t = c_type(n.var_type)
        if isinstance(n.value, ArrayLiteral):
            et = elem_type(n.var_type)
            self._emit(f"{t} {n.name} = cryo_array_new();")
            for elem in n.value.elements:
                fn = _PUSH_FN.get(et, 'cryo_array_push')
                self._emit(f"{fn}({n.name}, {self._expr(elem)});")
        elif n.value is not None:
            self._emit(f"{t} {n.name} = {self._expr(n.value)};")
        else:
            self._emit(f"{t} {n.name};")

    def _const(self, n: ConstDecl):
        self.te.set(n.name, n.var_type)
        t = c_type(n.var_type)
        self._emit(f"static const {t} {n.name} = {self._expr(n.value)};")

    def _assign(self, n: Assignment):
        self._emit(f"{n.name} = {self._expr(n.value)};")

    def _compound(self, n: CompoundAssignment):
        self._emit(f"{n.name} {n.op} {self._expr(n.value)};")

    def _incr(self, n: Increment):
        self._emit(f"{n.name}{n.op};")

    def _return(self, n: Return):
        if n.value is None:
            self._emit("return;")
        else:
            self._emit(f"return {self._expr(n.value)};")

    def _if(self, n: If):
        self._emit(f"if ({self._expr(n.condition)}) {{")
        self._indent += 1
        self.te.push()
        for s in n.then_body: self._gen(s)
        self.te.pop()
        self._indent -= 1
        if n.else_body:
            # elif chaining
            if len(n.else_body) == 1 and isinstance(n.else_body[0], If):
                inner = n.else_body[0]
                self._emit(f"}} else if ({self._expr(inner.condition)}) {{")
                self._indent += 1
                self.te.push()
                for s in inner.then_body: self._gen(s)
                self.te.pop()
                self._indent -= 1
                if inner.else_body:
                    self._emit("} else {")
                    self._indent += 1
                    self.te.push()
                    for s in inner.else_body: self._gen(s)
                    self.te.pop()
                    self._indent -= 1
                self._emit("}")
            else:
                self._emit("} else {")
                self._indent += 1
                self.te.push()
                for s in n.else_body: self._gen(s)
                self.te.pop()
                self._indent -= 1
                self._emit("}")
        else:
            self._emit("}")

    def _while(self, n: While):
        self._emit(f"while ({self._expr(n.condition)}) {{")
        self._indent += 1
        self.te.push()
        self._loop_depth += 1
        for s in n.body: self._gen(s)
        self._loop_depth -= 1
        self.te.pop()
        self._indent -= 1
        self._emit("}")

    def _for(self, n: For):
        init_s = self._for_part(n.init)   if n.init      else ''
        cond_s = self._expr(n.condition)  if n.condition  else '1'
        upd_s  = self._for_part(n.update) if n.update     else ''
        self._emit(f"for ({init_s}; {cond_s}; {upd_s}) {{")
        self._indent += 1
        self.te.push()
        self._loop_depth += 1
        for s in n.body: self._gen(s)
        self._loop_depth -= 1
        self.te.pop()
        self._indent -= 1
        self._emit("}")

    def _do_while(self, n: DoWhile):
        self._emit("do {")
        self._indent += 1
        self.te.push()
        self._loop_depth += 1
        for s in n.body: self._gen(s)
        self._loop_depth -= 1
        self.te.pop()
        self._indent -= 1
        self._emit(f"}} while ({self._expr(n.condition)});")

    def _foreach(self, n: ForEach):
        idx = f"_fe{self._fe}"; self._fe += 1
        arr = self._expr(n.iterable)
        et  = n.var_type
        get = _GET_FN.get(et, 'cryo_array_get')
        self._emit(f"for (int64_t {idx} = 0; {idx} < ({arr})->length; {idx}++) {{")
        self._indent += 1
        self.te.push()
        self.te.set(n.var_name, et)
        self._emit(f"{c_type(et)} {n.var_name} = {get}({arr}, {idx});")
        self._loop_depth += 1
        for s in n.body: self._gen(s)
        self._loop_depth -= 1
        self.te.pop()
        self._indent -= 1
        self._emit("}")

    def _break(self, n: Break):
        if self._loop_depth == 0:
            raise CodeGenError("'break' fora de um laco")
        self._emit("break;")

    def _continue(self, n: Continue):
        if self._loop_depth == 0:
            raise CodeGenError("'continue' fora de um laco")
        self._emit("continue;")

    def _switch(self, n: Switch):
        sub_t = self.te.infer(n.subject)
        # switch em string nao existe em C: desdobra em if/else encadeado.
        if sub_t == 'string':
            self._switch_as_if(n)
            return
        self._emit(f"switch ({self._expr(n.subject)}) {{")
        self._indent += 1
        # cada case dentro de switch quebra por padrao (sem fall-through implicito)
        self._loop_depth += 1  # permite 'break' dentro de case
        for case in n.cases:
            for v in case.values:
                self._emit(f"case {self._expr(v)}:")
            self._indent += 1
            self.te.push()
            for s in case.body: self._gen(s)
            self.te.pop()
            if not self._terminates(case.body):
                self._emit("break;")
            self._indent -= 1
        if n.default_body is not None:
            self._emit("default:")
            self._indent += 1
            self.te.push()
            for s in n.default_body: self._gen(s)
            self.te.pop()
            if not self._terminates(n.default_body):
                self._emit("break;")
            self._indent -= 1
        self._loop_depth -= 1
        self._indent -= 1
        self._emit("}")

    @staticmethod
    def _terminates(body: List[Node]) -> bool:
        """True se o bloco termina com return/break/continue (sem fall-through)."""
        return bool(body) and isinstance(body[-1], (Return, Break, Continue))

    def _switch_as_if(self, n: Switch):
        subj = self._expr(n.subject)
        first = True
        for case in n.cases:
            conds = ' || '.join(f"cryo_str_eq({subj}, {self._expr(v)})"
                                for v in case.values)
            kw = 'if' if first else '} else if'
            self._emit(f"{kw} ({conds}) {{")
            self._indent += 1
            self.te.push()
            for s in case.body: self._gen(s)
            self.te.pop()
            self._indent -= 1
            first = False
        if n.default_body is not None:
            self._emit("} else {" if not first else "if (1) {")
            self._indent += 1
            self.te.push()
            for s in n.default_body: self._gen(s)
            self.te.pop()
            self._indent -= 1
        if not first or n.default_body is not None:
            self._emit("}")

    def _assert(self, n: Assert):
        cond = self._expr(n.condition)
        if n.message is not None:
            msg = self._expr(n.message)
        else:
            msg = f'"assert falhou (linha {n.line})"'
        self._emit(f"cryo_assert({cond}, {msg});")

    def _safety(self, n: SafetyBlock):
        tag = 'safe' if n.safe else 'unsafe'
        self._emit(f"{{  /* [CRYO] bloco {tag} */")
        self._indent += 1
        self._safe_stack.append(n.safe)
        self.te.push()
        for s in n.body: self._gen(s)
        self.te.pop()
        self._safe_stack.pop()
        self._indent -= 1
        self._emit("}")

    def _for_part(self, node: Node) -> str:
        if isinstance(node, VarDecl):
            self.te.set(node.name, node.var_type)
            val = self._expr(node.value) if node.value else '0'
            return f"{c_type(node.var_type)} {node.name} = {val}"
        if isinstance(node, Assignment):
            return f"{node.name} = {self._expr(node.value)}"
        if isinstance(node, CompoundAssignment):
            return f"{node.name} {node.op} {self._expr(node.value)}"
        if isinstance(node, Increment):
            return f"{node.name}{node.op}"
        return self._expr(node)

    def _try(self, n: TryCatch):
        # CRYO_TRY macro opens:  if (!setjmp(...)) { active=true;
        self._emit("CRYO_TRY")
        self._indent += 1
        self.te.push()
        for s in n.try_body: self._gen(s)
        self.te.pop()
        self._indent -= 1
        if n.catch_body is not None:
            var = n.catch_name or '_cryo_err'
            self._emit(f"CRYO_CATCH({var})")
            self._indent += 1
            self.te.push()
            self.te.set(var, 'string')
            for s in n.catch_body: self._gen(s)
            self.te.pop()
            self._indent -= 1
        self._emit("CRYO_END_CATCH")
        # Finally: always emitted after try/catch block
        if n.finally_body:
            self._emit("CRYO_FINALLY {")
            self._indent += 1
            self.te.push()
            for s in n.finally_body: self._gen(s)
            self.te.pop()
            self._indent -= 1
            self._emit("}")

    def _import(self, n: Import):
        self._emit(f"/* [CRYO] import >{n.lang}< */")

    def _library(self, n: Library):
        lib = n.name.lower()
        self._emit(f'#include <{lib}.h>  /* [CRYO] library >{n.name}< */')

    def _foreign(self, n: ForeignBlock):
        if n.lang.lower() == 'c':
            self._emit("/* -- [C block] -- */")
            for line in n.code.strip().split('\n'):
                self._emit(line.rstrip())
            self._emit("/* -- [/C block] -- */")
        else:
            self._emit(f"/* [CRYO] bloco >{n.lang}< ignorado no backend C */")

    # ── expressoes ──────────────────────────────────────────

    def _expr(self, node: Node) -> str:
        if isinstance(node, (MapLiteral, CastExpr, UnwrapExpr)):
            raise CodeGenError(
                f"'{type(node).__name__}' (map/JSON/opcional) ainda não é "
                f"suportado no backend C; use --backend go.")
        if isinstance(node, Literal):
            if node.kind == 'null':   return 'NULL'
            if node.kind == 'bool':   return 'true' if node.value else 'false'
            if node.kind == 'string': return f'"{node.value}"'
            if node.kind == 'int':    return str(node.value)
            if node.kind == 'float':  return repr(float(node.value))
            return str(node.value)

        if isinstance(node, Identifier):
            return node.name

        if isinstance(node, BinaryExpr):
            return self._binary(node)

        if isinstance(node, UnaryExpr):
            op = '!' if node.op == '!' else node.op
            return f"({op}{self._expr(node.operand)})"

        if isinstance(node, TernaryExpr):
            return (f"({self._expr(node.condition)} ? "
                    f"{self._expr(node.then_value)} : "
                    f"{self._expr(node.else_value)})")

        if isinstance(node, CallExpr):
            return self._call(node)

        if isinstance(node, MethodCallExpr):
            return self._method(node)

        if isinstance(node, FieldAccess):
            obj = self._expr(node.obj)
            ot  = self.te.infer(node.obj)
            if node.field == 'length':
                # array length
                return f"(({obj})->length)"
            # struct field: pointer or value?
            return f"{obj}.{node.field}"

        if isinstance(node, IndexAccess):
            obj = self._expr(node.obj)
            idx = self._expr(node.index)
            at  = self.te.infer(node.obj)
            et  = elem_type(at)
            fn  = _GET_FN.get(et, 'cryo_array_get')
            return f"{fn}({obj}, {idx})"

        if isinstance(node, ArrayLiteral):
            return "/* inline-array */"

        if isinstance(node, StructInit):
            # C99 compound literal: (TypeName){ .field = val, ... }
            fields = ', '.join(f".{k} = {self._expr(v)}" for k, v in node.fields)
            return f"({node.struct_name}){{{fields}}}"

        return f"/* UNKNOWN_EXPR({type(node).__name__}) */"

    def _binary(self, node: BinaryExpr) -> str:
        lt = self.te.infer(node.left)
        rt = self.te.infer(node.right)
        l  = self._expr(node.left)
        r  = self._expr(node.right)

        if node.op == '&&': return f"({l} && {r})"
        if node.op == '||': return f"({l} || {r})"
        if node.op == '??': return f"(({l}) != NULL ? ({l}) : ({r}))"

        # Concatenacao de strings
        if node.op == '+' and (lt == 'string' or rt == 'string'):
            ls = l if lt == 'string' else self._to_str(l, lt)
            rs = r if rt == 'string' else self._to_str(r, rt)
            return f"cryo_str_concat({ls}, {rs})"

        # Comparacao de strings
        if node.op == '==' and (lt == 'string' or rt == 'string'):
            return f"cryo_str_eq({l}, {r})"
        if node.op == '!=' and (lt == 'string' or rt == 'string'):
            return f"(!cryo_str_eq({l}, {r}))"

        # ── Operadores bit a bit (apenas inteiros) ──
        if node.op in ('&', '|', '^', '<<', '>>'):
            return f"({l} {node.op} {r})"

        # ── Instrumentacao de seguranca em inteiros ──
        both_int = (lt == 'int' and rt == 'int')
        if self._safe and both_int:
            if node.op == '+': return f"cryo_add_ovf({l}, {r})"
            if node.op == '-': return f"cryo_sub_ovf({l}, {r})"
            if node.op == '*': return f"cryo_mul_ovf({l}, {r})"
            if node.op == '/': return f"cryo_idiv_chk({l}, {r})"
            if node.op == '%': return f"cryo_imod_chk({l}, {r})"
        # Divisao/modulo por zero: sempre protegidos em inteiros
        elif both_int and node.op in ('/', '%'):
            fn = 'cryo_idiv_chk' if node.op == '/' else 'cryo_imod_chk'
            return f"{fn}({l}, {r})"

        return f"({l} {node.op} {r})"

    def _call(self, node: CallExpr) -> str:
        callee = node.callee
        args   = node.args

        # camada Pyro (skills/máquina): apenas backend Go
        if callee.startswith('pyro_') or callee in (
                'skills', 'skill_get', 'skill_has', 'skills_json', 'json_encode'):
            raise CodeGenError(
                f"'{callee}()' faz parte da camada Pyro/JSON e só existe no "
                f"backend Go; use --backend go.")

        # ── built-ins ──
        if callee == 'print':
            return self._gen_print(args)
        if callee == 'sqrt':
            return f"cryo_sqrt({self._expr(args[0])})"
        if callee == 'pow':
            return f"cryo_pow({self._expr(args[0])}, {self._expr(args[1])})"
        if callee in ('abs', 'fabs'):
            t = self.te.infer(args[0])
            fn = 'cryo_abs_f' if t == 'number' else 'cryo_abs_i'
            return f"{fn}({self._expr(args[0])})"
        if callee in ('min', 'max') and len(args) == 2:
            t = self.te.infer(args[0])
            suf = 'i' if t == 'int' else 'f'
            return f"cryo_{callee}_{suf}({self._expr(args[0])}, {self._expr(args[1])})"
        if callee == 'floor':
            return f"cryo_floor({self._expr(args[0])})"
        if callee == 'ceil':
            return f"cryo_ceil({self._expr(args[0])})"
        if callee == 'round':
            return f"cryo_round({self._expr(args[0])})"
        if callee == 'to_string':
            a = args[0]
            return self._to_str(self._expr(a), self.te.infer(a))
        if callee == 'to_int':
            return f"cryo_to_int({self._expr(args[0])})"
        if callee == 'to_number':
            return f"cryo_to_num({self._expr(args[0])})"
        if callee == 'len':
            a = args[0]
            t = self.te.infer(a)
            if t == 'string':
                return f"cryo_str_len({self._expr(a)})"
            return f"(({self._expr(a)})->length)"
        if callee == 'input':
            prompt = self._expr(args[0]) if args else '""'
            return f"cryo_input({prompt})"
        if callee == 'throw':
            return f"CRYO_THROW({self._expr(args[0])})"

        # Funcao definida pelo usuario
        args_str = ', '.join(self._expr(a) for a in args)
        return f"{callee}({args_str})"

    def _gen_print(self, args: List[Node]) -> str:
        if not args:
            return 'cryo_print_newline()'
        arg = args[0]
        typ = self.te.infer(arg)
        e   = self._expr(arg)
        if typ == 'string': return f'cryo_print_str({e})'
        if typ == 'int':    return f'cryo_print_i64({e})'
        if typ == 'number': return f'cryo_print_f64({e})'
        if typ == 'bool':   return f'cryo_print_bool({e})'
        # fallback: converte para string
        return f'cryo_print_str({self._to_str(e, typ)})'

    def _method(self, node: MethodCallExpr) -> str:
        obj  = self._expr(node.obj)
        at   = self.te.infer(node.obj)
        et   = elem_type(at)
        args = [self._expr(a) for a in node.args]
        m    = node.method

        if m == 'push':
            fn = _PUSH_FN.get(et, 'cryo_array_push')
            return f"{fn}({obj}, {args[0] if args else '0'})"
        if m in ('length', 'size'):
            return f"(({obj})->length)"
        if m == 'pop_last':
            fn = _GET_FN.get(et, 'cryo_array_get')
            return f"{fn}({obj}, ({obj})->length - 1)"
        if m == 'slice':
            s, e = (args + ['0', '0'])[:2]
            return f"cryo_array_slice({obj}, {s}, {e})"
        if m == 'upper':
            return f"cryo_str_upper({obj})"
        if m == 'lower':
            return f"cryo_str_lower({obj})"
        if m == 'contains':
            empty = '""'
            arg0 = args[0] if args else empty
            return f"cryo_str_contains({obj}, {arg0})"

        args_str = ', '.join(args)
        return f"{obj}.{m}({args_str})"

    def _to_str(self, expr: str, typ: str) -> str:
        if typ == 'string': return expr
        if typ == 'int':    return f"cryo_i64_to_str({expr})"
        if typ == 'number': return f"cryo_f64_to_str({expr})"
        if typ == 'bool':   return f"cryo_bool_to_str({expr})"
        return f"cryo_i64_to_str((int64_t)({expr}))"
