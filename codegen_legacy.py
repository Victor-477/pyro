# ============================================================
#  Cryo Compiler - CodeGen  (v0.2)
# ============================================================
from ast_nodes import (
    Node, Program,
    StructField, StructDecl, EnumDecl,
    FunctionDecl, VarDecl, ConstDecl, Assignment,
    CompoundAssignment, Increment,
    Return, If, While, For, TryCatch,
    Import, Library, ForeignBlock,
    BinaryExpr, UnaryExpr, CallExpr, MethodCallExpr,
    FieldAccess, IndexAccess, ArrayLiteral, StructInit,
    Identifier, Literal,
)
from typing import List

TYPE_MAP = {
    'int':    'int',
    'number': 'float',
    'string': 'str',
    'bool':   'bool',
    'void':   'None',
}

# Metodos Cryo -> Python
METHOD_MAP = {
    'push':        'append',
    'pop_last':    'pop',
    'remove_item': 'remove',
    'index_of':    'index',
    'contains':    '__contains__',
}


class CodeGenError(Exception):
    pass


class CodeGen:
    def __init__(self):
        self._lines      = []
        self._indent     = 0
        self._has_struct = False
        self._has_enum   = False

    def _pad(self):
        return '    ' * self._indent

    def _emit(self, line=''):
        self._lines.append(self._pad() + line if line else '')

    def _emit_raw(self, text):
        base = None
        for raw in text.split('\n'):
            s = raw.lstrip()
            if not s:
                self._lines.append('')
                continue
            if base is None:
                base = len(raw) - len(s)
            extra = max(0, len(raw) - len(s) - base)
            self._lines.append(self._pad() + '    ' * extra + s)

    def _py_type(self, t):
        if t.endswith('[]'):
            return 'CryoArray'
        return TYPE_MAP.get(t, t)

    # ── scan ────────────────────────────────────────────────

    def _scan(self, stmts):
        for n in stmts:
            if isinstance(n, StructDecl):   self._has_struct = True
            elif isinstance(n, EnumDecl):   self._has_enum   = True
            elif isinstance(n, FunctionDecl): self._scan(n.body)
            elif isinstance(n, If):
                self._scan(n.then_body)
                if n.else_body: self._scan(n.else_body)
            elif isinstance(n, (While, For)): self._scan(n.body)
            elif isinstance(n, TryCatch):
                self._scan(n.try_body)
                if n.catch_body:   self._scan(n.catch_body)
                if n.finally_body: self._scan(n.finally_body)

    # ── generate ────────────────────────────────────────────

    def generate(self, program):
        self._scan(program.statements)
        self._emit("# ================================================")
        self._emit("#  [PYRO] Compilado a partir de codigo-fonte Cryo")
        self._emit("#  Gerado automaticamente - nao edite manualmente")
        self._emit("# ================================================")
        self._emit()
        self._emit("from pyro_runtime import *")
        if self._has_struct:
            self._emit("from pyro_runtime import dataclass")
        if self._has_enum:
            self._emit("from pyro_runtime import Enum")
        self._emit()
        for s in program.statements:
            self._gen(s)
        return '\n'.join(self._lines)

    # ── statements ──────────────────────────────────────────

    def _gen(self, node):
        if isinstance(node, StructDecl):         self._struct(node)
        elif isinstance(node, EnumDecl):         self._enum(node)
        elif isinstance(node, FunctionDecl):     self._fn(node)
        elif isinstance(node, VarDecl):          self._var(node)
        elif isinstance(node, ConstDecl):        self._const(node)
        elif isinstance(node, Assignment):       self._assign(node)
        elif isinstance(node, CompoundAssignment): self._compound(node)
        elif isinstance(node, Increment):        self._incr(node)
        elif isinstance(node, Return):           self._return(node)
        elif isinstance(node, If):               self._if(node)
        elif isinstance(node, While):            self._while(node)
        elif isinstance(node, For):              self._for(node)
        elif isinstance(node, TryCatch):         self._try(node)
        elif isinstance(node, Import):           self._import(node)
        elif isinstance(node, Library):          self._library(node)
        elif isinstance(node, ForeignBlock):     self._foreign(node)
        elif isinstance(node, (CallExpr, MethodCallExpr, BinaryExpr, UnaryExpr)):
            self._emit(self._expr(node))
        else:
            self._emit(f"# [PYRO] UNSUPPORTED: {type(node).__name__}")

    def _struct(self, n):
        self._emit("@dataclass")
        self._emit(f"class {n.name}:")
        self._indent += 1
        if not n.fields: self._emit("pass")
        for f in n.fields:
            self._emit(f"{f.name}: {self._py_type(f.field_type)}")
        self._indent -= 1
        self._emit()

    def _enum(self, n):
        self._emit(f"class {n.name}(Enum):")
        self._indent += 1
        for m in n.members:
            self._emit(f'{m} = "{m}"')
        self._indent -= 1
        self._emit()

    def _fn(self, n):
        params = ', '.join(f"{pn}: {self._py_type(pt)}" for pt, pn in n.params)
        ret = f" -> {self._py_type(n.return_type)}" if n.return_type else ''
        self._emit(f"def {n.name}({params}){ret}:")
        self._indent += 1
        if not n.body: self._emit("pass")
        for s in n.body: self._gen(s)
        self._indent -= 1
        self._emit()

    def _var(self, n):
        t = self._py_type(n.var_type)
        if n.value is not None:
            v = self._expr(n.value)
            if isinstance(n.value, ArrayLiteral):
                v = f"CryoArray({v})"
            self._emit(f"{n.name}: {t} = {v}")
        else:
            self._emit(f"{n.name}: {t}")

    def _const(self, n):
        self._emit(f"{n.name.upper()}: {self._py_type(n.var_type)} = {self._expr(n.value)}  # const")

    def _assign(self, n):
        self._emit(f"{n.name} = {self._expr(n.value)}")

    def _compound(self, n):
        self._emit(f"{n.name} {n.op} {self._expr(n.value)}")

    def _incr(self, n):
        self._emit(f"{n.name} += 1" if n.op == '++' else f"{n.name} -= 1")

    def _return(self, n):
        self._emit("return None" if n.value is None else f"return {self._expr(n.value)}")

    def _if(self, n):
        self._emit(f"if {self._expr(n.condition)}:")
        self._indent += 1
        for s in n.then_body: self._gen(s)
        self._indent -= 1
        if n.else_body:
            # elif chaining
            if len(n.else_body) == 1 and isinstance(n.else_body[0], If):
                inner = n.else_body[0]
                self._emit(f"elif {self._expr(inner.condition)}:")
                self._indent += 1
                for s in inner.then_body: self._gen(s)
                self._indent -= 1
                if inner.else_body:
                    self._emit("else:")
                    self._indent += 1
                    for s in inner.else_body: self._gen(s)
                    self._indent -= 1
            else:
                self._emit("else:")
                self._indent += 1
                for s in n.else_body: self._gen(s)
                self._indent -= 1

    def _while(self, n):
        self._emit(f"while {self._expr(n.condition)}:")
        self._indent += 1
        if not n.body: self._emit("pass")
        for s in n.body: self._gen(s)
        self._indent -= 1

    def _for(self, n):
        if n.init:   self._gen(n.init)
        cond = self._expr(n.condition) if n.condition else "True"
        self._emit(f"while {cond}:")
        self._indent += 1
        if not n.body: self._emit("pass")
        for s in n.body: self._gen(s)
        if n.update: self._gen(n.update)
        self._indent -= 1

    def _try(self, n):
        self._emit("try:")
        self._indent += 1
        if not n.try_body: self._emit("pass")
        for s in n.try_body: self._gen(s)
        self._indent -= 1
        if n.catch_body is not None:
            exc = f" as {n.catch_name}" if n.catch_name else ""
            self._emit(f"except Exception{exc}:")
            self._indent += 1
            if not n.catch_body: self._emit("pass")
            for s in n.catch_body: self._gen(s)
            self._indent -= 1
        if n.finally_body is not None:
            self._emit("finally:")
            self._indent += 1
            if not n.finally_body: self._emit("pass")
            for s in n.finally_body: self._gen(s)
            self._indent -= 1

    def _import(self, n):
        self._emit(f"# [CRYO] import >{n.lang}<")
        self._emit()

    def _library(self, n):
        self._emit(f"import {n.name.lower()}  # [CRYO] library >{n.name}<")

    def _foreign(self, n):
        self._emit(f"# -- [{n.lang} block] --")
        if n.lang.lower() == 'python':
            self._emit_raw(n.code)
        else:
            self._emit(f"__cryo_bridge__(lang={n.lang!r}, code={n.code!r})")
        self._emit(f"# -- [/{n.lang} block] --")
        self._emit()

    # ── expressoes ──────────────────────────────────────────

    def _expr(self, node):
        if isinstance(node, Literal):
            if node.kind == 'null':   return 'None'
            if node.kind == 'bool':   return 'True' if node.value else 'False'
            if node.kind == 'string': return repr(node.value)
            return str(node.value)

        if isinstance(node, Identifier):
            return node.name

        if isinstance(node, BinaryExpr):
            l, r = self._expr(node.left), self._expr(node.right)
            if node.op == '&&': return f"({l} and {r})"
            if node.op == '||': return f"({l} or {r})"
            if node.op == '??': return f"({l} if {l} is not None else {r})"
            return f"({l} {node.op} {r})"

        if isinstance(node, UnaryExpr):
            op = 'not ' if node.op == '!' else node.op
            return f"({op}{self._expr(node.operand)})"

        if isinstance(node, CallExpr):
            args = ', '.join(self._expr(a) for a in node.args)
            return f"{node.callee}({args})"

        if isinstance(node, MethodCallExpr):
            obj    = self._expr(node.obj)
            method = METHOD_MAP.get(node.method, node.method)
            args   = ', '.join(self._expr(a) for a in node.args)
            return f"{obj}.{method}({args})"

        if isinstance(node, FieldAccess):
            obj = self._expr(node.obj)
            if node.field == 'length':
                return f"len({obj})"
            return f"{obj}.{node.field}"

        if isinstance(node, IndexAccess):
            return f"{self._expr(node.obj)}[{self._expr(node.index)}]"

        if isinstance(node, ArrayLiteral):
            return f"[{', '.join(self._expr(e) for e in node.elements)}]"

        if isinstance(node, StructInit):
            fields = ', '.join(f"{k}={self._expr(v)}" for k, v in node.fields)
            return f"{node.struct_name}({fields})"

        raise CodeGenError(f"[CodeGen] Expressao desconhecida: {type(node).__name__}")
