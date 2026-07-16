# ============================================================
#  Cryo Compiler - Go Code Generator  (v0.5)
#  .cryo  ->  .go  (Go nativo, compilavel com `go build`)
#
#  Go passa a ser a linguagem-base de compilacao do Cryo: e de
#  alto nivel, multiplataforma, com um unico `go build`, e cobre
#  naturalmente structs, arrays, strings, enums e excecoes.
#  (O backend assembly permanece disponivel para uso futuro.)
# ============================================================
from ast_nodes import *
from typing import List, Dict, Set, Optional


class CodeGenGoError(Exception):
    pass


# ── Mapeamento de tipos Cryo -> Go ──────────────────────────

GO_TYPE: Dict[str, str] = {
    'int':    'int64',
    'number': 'float64',
    'string': 'string',
    'bool':   'bool',
    'void':   '',
}

GO_KEYWORDS = {
    'break', 'case', 'chan', 'const', 'continue', 'default', 'defer',
    'else', 'fallthrough', 'for', 'func', 'go', 'goto', 'if', 'import',
    'interface', 'map', 'package', 'range', 'return', 'select', 'struct',
    'switch', 'type', 'var', 'nil', 'true', 'false', 'len', 'cap', 'make',
    'new', 'append', 'copy', 'delete', 'init', 'main',
}


def gid(name: str) -> str:
    """Evita colisao de identificadores Cryo com palavras-chave Go."""
    return name + '_' if name in GO_KEYWORDS else name


def _split_type_pair(s: str):
    """Divide 'K,V' respeitando aninhamento de <> e []."""
    depth = 0
    for i, c in enumerate(s):
        if c in '<[':
            depth += 1
        elif c in '>]':
            depth -= 1
        elif c == ',' and depth == 0:
            return s[:i].strip(), s[i + 1:].strip()
    raise CodeGenGoError(f"tipo map malformado: '{s}'")


def go_type(t: str) -> str:
    if not t:
        return ''
    if t.endswith('?'):                       # opcional -> ponteiro
        return '*' + go_type(t[:-1])
    if t.startswith('map<') and t.endswith('>'):
        k, v = _split_type_pair(t[4:-1])
        return f"map[{go_type(k)}]{go_type(v)}"
    if t.endswith('[]'):
        return '[]' + go_type(t[:-2])
    return GO_TYPE.get(t, t)   # structs/enums passam direto


def go_field(name: str) -> str:
    """Nome de campo exportado em Go (necessário p/ encoding/json)."""
    return name[:1].upper() + name[1:] if name else name


def is_map(t: str) -> bool:
    return bool(t) and t.startswith('map<') and t.endswith('>')


def is_optional(t: str) -> bool:
    return bool(t) and t.endswith('?')


def elem_type(arr_t: str) -> str:
    if not arr_t:
        return 'unknown'
    if arr_t.endswith('[]'):
        return arr_t[:-2]
    if is_map(arr_t):                          # valor de um map
        return _split_type_pair(arr_t[4:-1])[1]
    return 'unknown'


def map_key_type(t: str) -> str:
    return _split_type_pair(t[4:-1])[0] if is_map(t) else 'unknown'


def zero_value(t: str) -> str:
    """Valor 'null'/zero para um tipo Go."""
    if t == 'int':    return '0'
    if t == 'number': return '0.0'
    if t == 'string': return '""'
    if t == 'bool':   return 'false'
    return 'nil'


# ── Inferencia de tipos (para concat de string, print, etc.) ─

class TypeEnv:
    def __init__(self):
        self._scopes: List[Dict[str, str]] = [{}]
        self._fns:    Dict[str, str] = {}
        self._structs: Dict[str, Dict[str, str]] = {}
        self._enums:  Set[str] = set()

    def push(self): self._scopes.append({})
    def pop(self):  self._scopes.pop()
    def set(self, name, typ): self._scopes[-1][name] = typ

    def get(self, name):
        for s in reversed(self._scopes):
            if name in s:
                return s[name]
        return 'unknown'

    def reg_fn(self, name, ret): self._fns[name] = ret
    def fn_ret(self, name):      return self._fns.get(name, 'unknown')
    def reg_struct(self, name, fields): self._structs[name] = fields
    def struct_field(self, s, f): return self._structs.get(s, {}).get(f, 'unknown')
    def reg_enum(self, name):     self._enums.add(name)
    def is_enum(self, name):      return name in self._enums

    def infer(self, node) -> str:
        if node is None: return 'unknown'
        if isinstance(node, Literal):
            return {'int': 'int', 'float': 'number', 'string': 'string',
                    'bool': 'bool', 'null': 'null'}.get(node.kind, 'unknown')
        if isinstance(node, Identifier):
            return self.get(node.name)
        if isinstance(node, BinaryExpr):
            if node.op in ('==', '!=', '<', '>', '<=', '>=', '&&', '||'):
                return 'bool'
            lt = self.infer(node.left); rt = self.infer(node.right)
            if node.op == '??':
                return lt if lt not in ('null', 'unknown') else rt
            if lt == 'string' or rt == 'string': return 'string'
            if lt == 'number' or rt == 'number': return 'number'
            return lt if lt != 'unknown' else rt
        if isinstance(node, UnaryExpr):
            return 'bool' if node.op == '!' else self.infer(node.operand)
        if isinstance(node, TernaryExpr):
            t = self.infer(node.then_value)
            return t if t not in ('unknown', 'null') else self.infer(node.else_value)
        if isinstance(node, CallExpr):
            builtin = {'sqrt': 'number', 'pow': 'number', 'to_string': 'string',
                       'to_int': 'int', 'to_number': 'number', 'len': 'int',
                       'input': 'string', 'abs': 'number', 'floor': 'number',
                       'ceil': 'number', 'round': 'number', 'json_encode': 'string',
                       'skills': 'string[]', 'skill_get': 'Skill',
                       'skill_has': 'bool', 'skills_json': 'string',
                       'pyro_exec': 'string', 'pyro_env': 'string',
                       'pyro_args': 'string[]', 'pyro_time': 'int',
                       'pyro_read': 'string'}.get(node.callee)
            return builtin or self.fn_ret(node.callee)
        if isinstance(node, StructInit):
            return node.struct_name
        if isinstance(node, ArrayLiteral):
            return 'array'
        if isinstance(node, MapLiteral):
            return 'map'
        if isinstance(node, CastExpr):
            return node.target_type
        if isinstance(node, UnwrapExpr):
            t = self.infer(node.operand)
            return t[:-1] if t.endswith('?') else t
        if isinstance(node, FieldAccess):
            if node.field == 'length': return 'int'
            return self.struct_field(self.infer(node.obj), node.field)
        if isinstance(node, IndexAccess):
            return elem_type(self.infer(node.obj))
        return 'unknown'


# ── CodeGen Go ──────────────────────────────────────────────

class CodeGenGo:
    def __init__(self, safe: bool = True):
        self.te = TypeEnv()
        self._safe = safe
        self._imports: Set[str] = set()
        self._helpers: Set[str] = set()
        self._enum_defs:   List[str] = []
        self._struct_defs: List[str] = []
        self._global_defs: List[str] = []
        self._fn_defs:     List[str] = []
        self._main:        List[str] = []
        self._cur:  List[str] = self._main
        self._indent = 1
        self._safe_stack: List[bool] = []
        self._loop_depth = 0
        self._cur_fn_ret = 'void'
        # ── camada Pyro: skills nativas e acesso à máquina ──
        self._skills: List[SkillDecl] = []
        self._use_skills = False

    @property
    def _safe_mode(self) -> bool:
        return self._safe_stack[-1] if self._safe_stack else self._safe

    # ── emissao ─────────────────────────────────────────────

    def _emit(self, line: str = ''):
        self._cur.append(('\t' * self._indent + line) if line else '')

    # ── entrada principal ───────────────────────────────────

    def generate(self, program: Program) -> str:
        self._pre_scan(program.statements)
        for node in program.statements:
            if isinstance(node, EnumDecl):
                self._cur, self._indent = self._enum_defs, 0
                self._enum(node)
            elif isinstance(node, StructDecl):
                self._cur, self._indent = self._struct_defs, 0
                self._struct(node)
            elif isinstance(node, FunctionDecl):
                self._cur, self._indent = self._fn_defs, 0
                self._fn(node)
            elif isinstance(node, ConstDecl):
                self._cur, self._indent = self._global_defs, 0
                self._const(node)
            elif isinstance(node, SkillDecl):
                self._skills.append(node)   # registrada; emitida em _assemble
            elif isinstance(node, (Import, Library)):
                pass  # dependencias de outras linguagens: ignoradas no Go
            else:
                self._cur, self._indent = self._main, 1
                self._gen(node)
        return self._assemble()

    def _pre_scan(self, stmts):
        # tipo nativo 'Skill' sempre conhecido pelo type-checker
        self.te.reg_struct('Skill', {
            'name': 'string', 'desc': 'string', 'model': 'string',
            'tools': 'string[]', 'config': 'map<string,string>'})
        for n in stmts:
            if isinstance(n, StructDecl):
                self.te.reg_struct(n.name, {f.name: f.field_type for f in n.fields})
            elif isinstance(n, EnumDecl):
                self.te.reg_enum(n.name)
            elif isinstance(n, FunctionDecl):
                self.te.reg_fn(n.name, n.return_type or 'void')
            elif isinstance(n, ConstDecl):
                self.te.set(n.name, n.var_type)

    def _assemble(self) -> str:
        # gera helpers e skills PRIMEIRO: ambos podem registrar imports
        # (fmt, bufio, sort, os/exec...) antes de montarmos o bloco de import.
        helper_lines = self._helper_defs()
        skill_lines = self._skill_defs() if (self._skills or self._use_skills) else []
        out = [
            "// ================================================",
            "// [PYRO] Compilado de Cryo -> Go nativo  (v0.5)",
            "// Compilar: go build arquivo.go   |   Rodar: go run arquivo.go",
            "// ================================================",
            "package main",
            "",
        ]
        if self._imports:
            out.append("import (")
            for imp in sorted(self._imports):
                out.append(f'\t"{imp}"')
            out.append(")")
            out.append("")
        out += helper_lines
        if skill_lines:
            out += skill_lines + [""]
        if self._enum_defs:   out += self._enum_defs + [""]
        if self._struct_defs: out += self._struct_defs + [""]
        if self._global_defs: out += self._global_defs + [""]
        if self._fn_defs:     out += self._fn_defs + [""]
        out.append("func main() {")
        out += self._main
        out.append("}")
        out.append("")
        return '\n'.join(out)

    # ── helpers de runtime (emitidos sob demanda) ───────────

    def _helper_defs(self) -> List[str]:
        H: List[str] = []
        if 'str' in self._helpers:
            self._imports.add('fmt')
            H += ["func cryoStr(v any) string { return fmt.Sprint(v) }", ""]
        if 'or' in self._helpers:
            H += ["func cryoOr[T comparable](a, b T) T {",
                  "\tvar zero T",
                  "\tif a == zero {", "\t\treturn b", "\t}",
                  "\treturn a", "}", ""]
        if 'assert' in self._helpers:
            H += ["func cryoAssert(cond bool, msg string) {",
                  "\tif !cond {", '\t\tpanic("[Cryo Assert] " + msg)', "\t}", "}", ""]
        if 'addovf' in self._helpers:
            H += ["func cryoAddOvf(a, b int64) int64 {",
                  "\ts := a + b",
                  "\tif (b > 0 && s < a) || (b < 0 && s > a) {",
                  '\t\tpanic("[Cryo Seguranca] Overflow: adicao de inteiros")', "\t}",
                  "\treturn s", "}", ""]
        if 'subovf' in self._helpers:
            H += ["func cryoSubOvf(a, b int64) int64 {",
                  "\ts := a - b",
                  "\tif (b < 0 && s < a) || (b > 0 && s > a) {",
                  '\t\tpanic("[Cryo Seguranca] Overflow: subtracao de inteiros")', "\t}",
                  "\treturn s", "}", ""]
        if 'mulovf' in self._helpers:
            H += ["func cryoMulOvf(a, b int64) int64 {",
                  "\tif a == 0 || b == 0 {", "\t\treturn 0", "\t}",
                  "\ts := a * b",
                  "\tif s/b != a {",
                  '\t\tpanic("[Cryo Seguranca] Overflow: multiplicacao de inteiros")', "\t}",
                  "\treturn s", "}", ""]
        if 'idiv' in self._helpers:
            H += ["func cryoIDivChk(a, b int64) int64 {",
                  "\tif b == 0 {",
                  '\t\tpanic("[Cryo Seguranca] DivisaoPorZero: divisao inteira")', "\t}",
                  "\treturn a / b", "}", ""]
        if 'imod' in self._helpers:
            H += ["func cryoIModChk(a, b int64) int64 {",
                  "\tif b == 0 {",
                  '\t\tpanic("[Cryo Seguranca] DivisaoPorZero: modulo")', "\t}",
                  "\treturn a % b", "}", ""]
        if 'absi' in self._helpers:
            H += ["func cryoAbsI(x int64) int64 { if x < 0 { return -x }; return x }", ""]
        if 'jsonenc' in self._helpers:
            H += ["func cryoJSONEncode(v any) string {",
                  "\tb, err := json.Marshal(v)",
                  "\tif err != nil {", '\t\tpanic("[Cryo] json_encode: " + err.Error())', "\t}",
                  "\treturn string(b)", "}", ""]
        if 'ptr' in self._helpers:
            H += ["func cryoPtr[T any](v T) *T { return &v }", ""]
        if 'orptr' in self._helpers:
            H += ["func cryoOrPtr[T any](p *T, d T) T {",
                  "\tif p != nil {", "\t\treturn *p", "\t}",
                  "\treturn d", "}", ""]
        if 'unwrap' in self._helpers:
            H += ["func cryoUnwrap[T any](p *T) T {",
                  "\tif p == nil {", '\t\tpanic("[Cryo Seguranca] NullPointer: unwrap de opcional nulo")', "\t}",
                  "\treturn *p", "}", ""]
        if 'keys' in self._helpers:
            H += ["func cryoKeys[K comparable, V any](m map[K]V) []K {",
                  "\tks := make([]K, 0, len(m))",
                  "\tfor k := range m {", "\t\tks = append(ks, k)", "\t}",
                  "\treturn ks", "}", ""]
        if 'input' in self._helpers:
            self._imports.update(('bufio', 'os', 'fmt', 'strings'))
            H += ["var cryoStdin = bufio.NewReader(os.Stdin)",
                  "func cryoInput(prompt string) string {",
                  "\tif prompt != \"\" { fmt.Print(prompt) }",
                  "\ts, _ := cryoStdin.ReadString('\\n')",
                  "\treturn strings.TrimRight(s, \"\\r\\n\")", "}", ""]
        if 'exec' in self._helpers:
            self._imports.update(('os/exec', 'runtime'))
            H += ["func cryoExec(command string) string {",
                  "\tvar c *exec.Cmd",
                  '\tif runtime.GOOS == "windows" {',
                  '\t\tc = exec.Command("cmd", "/c", command)',
                  "\t} else {",
                  '\t\tc = exec.Command("sh", "-c", command)',
                  "\t}",
                  "\tout, _ := c.CombinedOutput()",
                  "\treturn string(out)", "}", ""]
        return H

    # ── Pyro: skills nativas (compiladas no binário) ────────

    def _skill_defs(self) -> List[str]:
        """Emite o tipo Skill, o registro global e helpers de introspecção."""
        D = ["// [PYRO] Skills nativas de LLM — compactas, sem arquivos .md",
             "type Skill struct {",
             '\tName   string            `json:"name"`',
             '\tDesc   string            `json:"desc"`',
             '\tModel  string            `json:"model"`',
             '\tTools  []string          `json:"tools"`',
             '\tConfig map[string]string `json:"config"`',
             "}", ""]
        # registro global
        entries = []
        for sk in self._skills:
            entries.append(f'\t{self._go_string(sk.name)}: {self._skill_literal(sk)},')
        D.append("var cryoSkills = map[string]Skill{")
        D += entries
        D.append("}")
        D.append("")
        # nomes ordenados (saída estável)
        self._imports.add('sort')
        D += ["func cryoSkillNames() []string {",
              "\tns := make([]string, 0, len(cryoSkills))",
              "\tfor n := range cryoSkills {", "\t\tns = append(ns, n)", "\t}",
              "\tsort.Strings(ns)", "\treturn ns", "}", "",
              "func cryoSkillList() []Skill {",
              "\tout := make([]Skill, 0, len(cryoSkills))",
              "\tfor _, n := range cryoSkillNames() {", "\t\tout = append(out, cryoSkills[n])", "\t}",
              "\treturn out", "}", ""]
        return D

    def _skill_literal(self, sk: SkillDecl) -> str:
        known = dict(sk.fields)
        desc  = self._skill_str(known.get('desc'))
        model = self._skill_str(known.get('model'))
        tools = self._skill_tools(known.get('tools'))
        cfg = []
        for k, v in sk.fields:
            if k in ('desc', 'model', 'tools'):
                continue
            cfg.append(f'{self._go_string(k)}: {self._go_string(self._literal_str(v))}')
        config = "map[string]string{" + ", ".join(cfg) + "}"
        return (f'Skill{{Name: {self._go_string(sk.name)}, Desc: {desc}, '
                f'Model: {model}, Tools: {tools}, Config: {config}}}')

    def _skill_str(self, node) -> str:
        if node is None:
            return '""'
        if isinstance(node, Literal) and node.kind == 'string':
            return self._go_string(node.value)
        return self._expr(node)

    def _skill_tools(self, node) -> str:
        if node is None:
            return "[]string{}"
        if isinstance(node, ArrayLiteral):
            items = ', '.join(self._skill_str(e) for e in node.elements)
            return f"[]string{{{items}}}"
        raise CodeGenGoError("'tools' de uma skill deve ser um array de strings.")

    def _literal_str(self, node) -> str:
        """Converte um literal de config de skill em string (compilado)."""
        if isinstance(node, Literal):
            if node.kind == 'bool':   return 'true' if node.value else 'false'
            if node.kind == 'string': return str(node.value)
            if node.kind == 'float':  return repr(float(node.value))
            return str(node.value)
        if isinstance(node, UnaryExpr) and node.op == '-' \
                and isinstance(node.operand, Literal):
            return '-' + self._literal_str(node.operand)
        raise CodeGenGoError(
            "valores de config de skill devem ser literais (string/número/bool).")

    # ── declaracoes ─────────────────────────────────────────

    def _enum(self, n: EnumDecl):
        self._enum_defs.append(f"type {gid(n.name)} int64")
        self._enum_defs.append("const (")
        for i, m in enumerate(n.members):
            suffix = f" {gid(n.name)} = iota" if i == 0 else ""
            self._enum_defs.append(f"\t{n.name}_{m}{suffix}")
        self._enum_defs.append(")")

    def _struct(self, n: StructDecl):
        # Campos exportados (maiúsculos) + tag json com o nome original,
        # para que encoding/json (json_encode/json_decode) funcione.
        self._struct_defs.append(f"type {gid(n.name)} struct {{")
        for f in n.fields:
            self._struct_defs.append(
                f'\t{go_field(f.name)} {go_type(f.field_type)} `json:"{f.name}"`')
        self._struct_defs.append("}")

    def _fn(self, n: FunctionDecl):
        params = ', '.join(f"{gid(pn)} {go_type(pt)}" for pt, pn in n.params)
        ret = go_type(n.return_type or 'void')
        ret_s = f" {ret}" if ret else ""
        prev_ret = self._cur_fn_ret
        self._cur_fn_ret = n.return_type or 'void'
        self._emit(f"func {gid(n.name)}({params}){ret_s} {{")
        self.te.push()
        for pt, pn in n.params:
            self.te.set(pn, pt)
        self._indent += 1
        for s in n.body:
            self._gen(s)
        self._indent -= 1
        self.te.pop()
        self._emit("}")
        self._emit()
        self._cur_fn_ret = prev_ret

    def _const(self, n: ConstDecl):
        self.te.set(n.name, n.var_type)
        # var de pacote: nao-usado nao e erro em Go
        self._global_defs.append(
            f"var {gid(n.name)} {go_type(n.var_type)} = {self._expr(n.value)}")

    # ── statements ──────────────────────────────────────────

    def _gen(self, node: Node):
        if   isinstance(node, VarDecl):            self._var(node)
        elif isinstance(node, ConstDecl):          self._local_const(node)
        elif isinstance(node, Assignment):         self._assign(node)
        elif isinstance(node, IndexAssignment):    self._index_assign(node)
        elif isinstance(node, CompoundAssignment): self._compound(node)
        elif isinstance(node, Increment):          self._incr(node)
        elif isinstance(node, Return):             self._return(node)
        elif isinstance(node, If):                 self._if(node)
        elif isinstance(node, While):              self._while(node)
        elif isinstance(node, DoWhile):            self._do_while(node)
        elif isinstance(node, For):                self._for(node)
        elif isinstance(node, ForEach):            self._foreach(node)
        elif isinstance(node, Switch):             self._switch(node)
        elif isinstance(node, Break):              self._emit("break")
        elif isinstance(node, Continue):           self._emit("continue")
        elif isinstance(node, Assert):             self._assert(node)
        elif isinstance(node, SafetyBlock):        self._safety(node)
        elif isinstance(node, TryCatch):           self._try(node)
        elif isinstance(node, ForeignBlock):       self._foreign(node)
        elif isinstance(node, (CallExpr, MethodCallExpr)):
            self._emit(self._stmt_call(node))
        else:
            self._emit(f"// [Go] NAO SUPORTADO: {type(node).__name__}")

    def _var(self, n: VarDecl):
        self.te.set(n.name, n.var_type)
        gt = go_type(n.var_type)
        vt = n.var_type
        name = gid(n.name)
        if isinstance(n.value, ArrayLiteral):
            elems = ', '.join(self._expr(e) for e in n.value.elements)
            self._emit(f"{name} := {gt}{{{elems}}}")
        elif isinstance(n.value, MapLiteral):
            self._emit(f"var {name} {gt} = {self._map_literal(n.value, vt)}")
        elif is_map(vt) and n.value is None:
            # map sem valor: inicializa vazio e gravável
            self._emit(f"{name} := {gt}{{}}")
        elif is_optional(vt) and n.value is not None:
            self._emit(f"var {name} {gt} = {self._to_optional(n.value, vt)}")
        elif n.value is not None:
            val = self._expr_typed(n.value, vt)
            self._emit(f"var {name} {gt} = {val}")
        else:
            self._emit(f"var {name} {gt}")
        self._emit(f"_ = {name}")   # Go: locais nao-usados sao erro

    def _to_optional(self, value: Node, opt_type: str) -> str:
        """Coage 'value' para o opcional T?: null->nil; se já é opcional,
        usa direto; senão embrulha o valor base em ponteiro (cryoPtr)."""
        if isinstance(value, Literal) and value.kind == 'null':
            return 'nil'
        if is_optional(self.te.infer(value)):     # já é T? (ex.: chamada que retorna T?)
            return self._expr(value)
        self._helpers.add('ptr')
        base = opt_type[:-1]                      # 'int?' -> 'int'
        base_go = go_type(base)
        inner = self._expr(value)
        if base in ('int', 'number', 'string', 'bool'):
            return f"cryoPtr[{base_go}]({base_go}({inner}))"
        return f"cryoPtr[{base_go}]({inner})"

    def _local_const(self, n: ConstDecl):
        self.te.set(n.name, n.var_type)
        self._emit(f"const {gid(n.name)} {go_type(n.var_type)} = {self._expr(n.value)}")

    def _assign(self, n: Assignment):
        self._emit(f"{gid(n.name)} = {self._expr(n.value)}")

    def _index_assign(self, n: IndexAssignment):
        self._emit(f"{self._expr(n.obj)}[{self._expr(n.index)}] = {self._expr(n.value)}")

    def _compound(self, n: CompoundAssignment):
        self._emit(f"{gid(n.name)} {n.op} {self._expr(n.value)}")

    def _incr(self, n: Increment):
        self._emit(f"{gid(n.name)}{n.op}")

    def _return(self, n: Return):
        if n.value is None:
            self._emit("return")
        elif is_optional(self._cur_fn_ret):
            self._emit(f"return {self._to_optional(n.value, self._cur_fn_ret)}")
        else:
            self._emit(f"return {self._expr(n.value)}")

    def _if(self, n: If):
        self._emit(f"if {self._expr(n.condition)} {{")
        self._indent += 1
        self.te.push()
        for s in n.then_body: self._gen(s)
        self.te.pop()
        self._indent -= 1
        if n.else_body:
            if len(n.else_body) == 1 and isinstance(n.else_body[0], If):
                inner = n.else_body[0]
                self._emit(f"}} else if {self._expr(inner.condition)} {{")
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
        self._emit(f"for {self._expr(n.condition)} {{")
        self._indent += 1
        self._loop_depth += 1
        self.te.push()
        for s in n.body: self._gen(s)
        self.te.pop()
        self._loop_depth -= 1
        self._indent -= 1
        self._emit("}")

    def _do_while(self, n: DoWhile):
        self._emit("for {")
        self._indent += 1
        self._loop_depth += 1
        self.te.push()
        for s in n.body: self._gen(s)
        self.te.pop()
        self._emit(f"if !({self._expr(n.condition)}) {{")
        self._indent += 1
        self._emit("break")
        self._indent -= 1
        self._emit("}")
        self._loop_depth -= 1
        self._indent -= 1
        self._emit("}")

    def _for(self, n: For):
        init = self._for_part(n.init) if n.init else ''
        cond = self._expr(n.condition) if n.condition else ''
        upd  = self._for_part(n.update) if n.update else ''
        self.te.push()
        # declara a variavel de init no escopo antes de emitir
        self._emit(f"for {init}; {cond}; {upd} {{")
        self._indent += 1
        self._loop_depth += 1
        for s in n.body: self._gen(s)
        self._loop_depth -= 1
        self._indent -= 1
        self.te.pop()
        self._emit("}")

    def _foreach(self, n: ForEach):
        self.te.push()
        self.te.set(n.var_name, n.var_type)
        self._emit(f"for _, {gid(n.var_name)} := range {self._expr(n.iterable)} {{")
        self._indent += 1
        self._loop_depth += 1
        self._emit(f"_ = {gid(n.var_name)}")
        for s in n.body: self._gen(s)
        self._loop_depth -= 1
        self._indent -= 1
        self.te.pop()
        self._emit("}")

    def _for_part(self, node: Node) -> str:
        if isinstance(node, VarDecl):
            self.te.set(node.name, node.var_type)
            val = self._expr(node.value) if node.value else zero_value(node.var_type)
            # for-init exige ':='; tipamos int/number explicitamente para
            # evitar 'int' inferido colidir com int64 no resto do sistema
            gt = go_type(node.var_type)
            if node.var_type in ('int', 'number'):
                val = f"{gt}({val})"
            return f"{gid(node.name)} := {val}"
        if isinstance(node, Assignment):
            return f"{gid(node.name)} = {self._expr(node.value)}"
        if isinstance(node, CompoundAssignment):
            return f"{gid(node.name)} {node.op} {self._expr(node.value)}"
        if isinstance(node, Increment):
            return f"{gid(node.name)}{node.op}"
        return self._expr(node)

    def _switch(self, n: Switch):
        self._emit(f"switch {self._expr(n.subject)} {{")
        for case in n.cases:
            vals = ', '.join(self._expr(v) for v in case.values)
            self._emit(f"case {vals}:")
            self._indent += 1
            self.te.push()
            for s in case.body: self._gen(s)
            self.te.pop()
            self._indent -= 1
        if n.default_body is not None:
            self._emit("default:")
            self._indent += 1
            self.te.push()
            for s in n.default_body: self._gen(s)
            self.te.pop()
            self._indent -= 1
        self._emit("}")

    def _assert(self, n: Assert):
        self._helpers.add('assert')
        cond = self._expr(n.condition)
        msg = self._expr(n.message) if n.message is not None \
            else f'"assert falhou (linha {n.line})"'
        self._emit(f"cryoAssert({cond}, {msg})")

    def _safety(self, n: SafetyBlock):
        tag = 'safe' if n.safe else 'unsafe'
        self._emit(f"{{ // bloco {tag}")
        self._indent += 1
        self._safe_stack.append(n.safe)
        self.te.push()
        for s in n.body: self._gen(s)
        self.te.pop()
        self._safe_stack.pop()
        self._indent -= 1
        self._emit("}")

    def _try(self, n: TryCatch):
        # Go nao tem excecoes: usa closure + defer/recover.
        self._emit("func() {")
        self._indent += 1
        if n.catch_body is not None or n.finally_body:
            self._emit("defer func() {")
            self._indent += 1
            if n.catch_body is not None:
                self._helpers.add('str')
                self._emit("if r := recover(); r != nil {")
                self._indent += 1
                var = n.catch_name or "_cryo_err"
                self._emit(f"{gid(var)} := cryoStr(r)")
                self._emit(f"_ = {gid(var)}")
                self.te.push()
                self.te.set(var, 'string')
                for s in n.catch_body: self._gen(s)
                self.te.pop()
                self._indent -= 1
                self._emit("}")
            if n.finally_body:
                self.te.push()
                for s in n.finally_body: self._gen(s)
                self.te.pop()
            self._indent -= 1
            self._emit("}()")
        self.te.push()
        for s in n.try_body: self._gen(s)
        self.te.pop()
        self._indent -= 1
        self._emit("}()")

    def _foreign(self, n: ForeignBlock):
        if n.lang.lower() == 'go':
            self._emit("// -- [bloco Go] --")
            for line in n.code.strip().split('\n'):
                self._emit(line.strip())
            self._emit("// -- [/bloco Go] --")
        else:
            self._emit(f"// [Cryo] bloco >{n.lang}< omitido no backend Go "
                       f"(use print(...) ou >Go( ... ))")

    # ── expressoes ──────────────────────────────────────────

    def _expr_typed(self, node: Node, target: str) -> str:
        """Expressao com conhecimento do tipo alvo (trata null)."""
        if isinstance(node, Literal) and node.kind == 'null':
            return zero_value(target)
        return self._expr(node)

    def _expr(self, node: Node) -> str:
        if isinstance(node, Literal):
            if node.kind == 'null':   return 'nil'
            if node.kind == 'bool':   return 'true' if node.value else 'false'
            if node.kind == 'string': return self._go_string(node.value)
            if node.kind == 'int':    return str(node.value)
            if node.kind == 'float':  return repr(float(node.value))
            return str(node.value)

        if isinstance(node, Identifier):
            return gid(node.name)

        if isinstance(node, BinaryExpr):
            return self._binary(node)

        if isinstance(node, TernaryExpr):
            return self._ternary(node)

        if isinstance(node, UnaryExpr):
            op = {'!': '!', '~': '^', '-': '-'}.get(node.op, node.op)
            return f"({op}{self._expr(node.operand)})"

        if isinstance(node, CallExpr):
            return self._call(node)

        if isinstance(node, MethodCallExpr):
            return self._method(node)

        if isinstance(node, FieldAccess):
            obj = self._expr(node.obj)
            if node.field == 'length':
                return f"int64(len({obj}))"
            return f"{obj}.{go_field(node.field)}"

        if isinstance(node, IndexAccess):
            return f"{self._expr(node.obj)}[{self._expr(node.index)}]"

        if isinstance(node, ArrayLiteral):
            elems = ', '.join(self._expr(e) for e in node.elements)
            return f"[]any{{{elems}}}"   # contexto sem tipo: fallback

        if isinstance(node, MapLiteral):
            return self._map_literal(node, None)

        if isinstance(node, StructInit):
            fields = ', '.join(
                f"{go_field(k)}: {self._expr(v)}" for k, v in node.fields)
            return f"{gid(node.struct_name)}{{{fields}}}"

        if isinstance(node, CastExpr):
            return self._cast(node)

        if isinstance(node, UnwrapExpr):
            self._helpers.add('unwrap')
            return f"cryoUnwrap({self._expr(node.operand)})"

        return f"/* EXPR? {type(node).__name__} */"

    def _map_literal(self, node: MapLiteral, map_type: Optional[str]) -> str:
        if map_type and is_map(map_type):
            gt = go_type(map_type)
        else:
            gt = "map[any]any"   # sem tipo alvo: fallback genérico
        pairs = ', '.join(f"{self._expr(k)}: {self._expr(v)}" for k, v in node.pairs)
        return f"{gt}{{{pairs}}}"

    def _cast(self, node: CastExpr) -> str:
        target = node.target_type
        gt = go_type(target)
        inner = node.expr
        # json_decode(s) as T  ->  Unmarshal tipado
        if isinstance(inner, CallExpr) and inner.callee == 'json_decode':
            self._imports.add('encoding/json')
            src = self._expr(inner.args[0]) if inner.args else '""'
            return (f"func() {gt} {{ var _v {gt}; "
                    f"_ = json.Unmarshal([]byte({src}), &_v); return _v }}()")
        # conversões numéricas
        if target in ('int', 'number'):
            return f"{gt}({self._expr(inner)})"
        # asserção de tipo (any -> T)
        return f"{self._expr(inner)}.({gt})"

    def _binary(self, node: BinaryExpr) -> str:
        lt = self.te.infer(node.left)
        rt = self.te.infer(node.right)
        l  = self._expr(node.left)
        r  = self._expr(node.right)
        op = node.op

        if op == '&&': return f"({l} && {r})"
        if op == '||': return f"({l} || {r})"
        if op == '??':
            if is_optional(lt):
                self._helpers.add('orptr')
                return f"cryoOrPtr({l}, {r})"
            self._helpers.add('or')
            return f"cryoOr({l}, {r})"

        # concatenacao de string (converte operando nao-string)
        if op == '+' and (lt == 'string' or rt == 'string'):
            ls = l if lt == 'string' else self._to_str(l, node.left)
            rs = r if rt == 'string' else self._to_str(r, node.right)
            return f"({ls} + {rs})"

        # bit a bit e shift: diretos
        if op in ('&', '|', '^', '<<', '>>'):
            return f"({l} {op} {r})"

        # instrumentacao de seguranca (inteiros)
        both_int = (lt == 'int' and rt == 'int')
        if both_int and op in ('+', '-', '*') and self._safe_mode:
            fn = {'+': 'cryoAddOvf', '-': 'cryoSubOvf', '*': 'cryoMulOvf'}[op]
            self._helpers.add({'+': 'addovf', '-': 'subovf', '*': 'mulovf'}[op])
            return f"{fn}({l}, {r})"
        if both_int and op == '/':
            self._helpers.add('idiv'); return f"cryoIDivChk({l}, {r})"
        if both_int and op == '%':
            self._helpers.add('imod'); return f"cryoIModChk({l}, {r})"

        return f"({l} {op} {r})"

    def _ternary(self, node: TernaryExpr) -> str:
        # Go nao tem ?:; usa IIFE com tipo inferido (avaliacao preguicosa)
        t = self.te.infer(node.then_value)
        gt = go_type(t) if t not in ('unknown', 'null', 'array') else 'any'
        cond = self._expr(node.condition)
        a = self._expr(node.then_value)
        b = self._expr(node.else_value)
        return f"func() {gt} {{ if {cond} {{ return {a} }}; return {b} }}()"

    def _to_str(self, expr: str, node: Node) -> str:
        self._helpers.add('str')
        return f"cryoStr({expr})"

    def _call(self, node: CallExpr) -> str:
        c = node.callee
        a = node.args
        if c == 'print':
            self._imports.add('fmt')
            if not a: return "fmt.Println()"
            return f"fmt.Println({self._expr(a[0])})"
        if c == 'sqrt':
            self._imports.add('math'); return f"math.Sqrt({self._expr(a[0])})"
        if c == 'pow':
            self._imports.add('math')
            return f"math.Pow({self._expr(a[0])}, {self._expr(a[1])})"
        if c in ('abs', 'fabs'):
            t = self.te.infer(a[0])
            if t == 'int':
                self._helpers.add('absi'); return f"cryoAbsI({self._expr(a[0])})"
            self._imports.add('math'); return f"math.Abs({self._expr(a[0])})"
        if c in ('min', 'max') and len(a) == 2:
            # builtins nativos do Go (>=1.21): funcionam p/ int64 e float64
            return f"{c}({self._expr(a[0])}, {self._expr(a[1])})"
        if c == 'floor':
            self._imports.add('math'); return f"math.Floor({self._expr(a[0])})"
        if c == 'ceil':
            self._imports.add('math'); return f"math.Ceil({self._expr(a[0])})"
        if c == 'round':
            self._imports.add('math'); return f"math.Round({self._expr(a[0])})"
        if c == 'to_string':
            self._helpers.add('str'); return f"cryoStr({self._expr(a[0])})"
        if c == 'to_int':
            return f"int64({self._expr(a[0])})"
        if c == 'to_number':
            return f"float64({self._expr(a[0])})"
        if c == 'len':
            return f"int64(len({self._expr(a[0])}))"
        if c == 'input':
            self._helpers.add('input')
            prompt = self._expr(a[0]) if a else '""'
            return f"cryoInput({prompt})"
        if c == 'throw':
            return f"panic({self._expr(a[0])})"
        # ── JSON ──
        if c == 'json_encode':
            self._imports.add('encoding/json')
            self._helpers.add('jsonenc')
            return f"cryoJSONEncode({self._expr(a[0])})"
        if c == 'json_decode':
            raise CodeGenGoError(
                "json_decode(s) exige um tipo alvo: use 'json_decode(s) as Tipo'.")
        # ── mapas ──
        if c == 'has' and len(a) == 2:
            # has(map, chave) -> existência
            return f"func() bool {{ _, ok := {self._expr(a[0])}[{self._expr(a[1])}]; return ok }}()"
        if c == 'remove' and len(a) == 2:
            return f"delete({self._expr(a[0])}, {self._expr(a[1])})"
        if c == 'keys' and len(a) == 1:
            self._helpers.add('keys')
            return f"cryoKeys({self._expr(a[0])})"
        # ── Pyro: introspecção de skills nativas (sem arquivos .md) ──
        if c == 'skills':
            self._use_skills = True
            return "cryoSkillNames()"
        if c == 'skill_get' and len(a) == 1:
            self._use_skills = True
            return f"cryoSkills[{self._expr(a[0])}]"
        if c == 'skill_has' and len(a) == 1:
            self._use_skills = True
            return f"func() bool {{ _, ok := cryoSkills[{self._expr(a[0])}]; return ok }}()"
        if c == 'skills_json':
            self._use_skills = True
            self._imports.add('encoding/json')
            self._helpers.add('jsonenc')
            return "cryoJSONEncode(cryoSkillList())"
        # ── Pyro: acesso direto à máquina ──
        if c == 'pyro_exec' and len(a) == 1:
            self._helpers.add('exec')
            return f"cryoExec({self._expr(a[0])})"
        if c == 'pyro_env' and len(a) == 1:
            self._imports.add('os')
            return f"os.Getenv({self._expr(a[0])})"
        if c == 'pyro_args':
            self._imports.add('os')
            return "os.Args"
        if c == 'pyro_exit' and len(a) == 1:
            self._imports.add('os')
            return f"os.Exit(int({self._expr(a[0])}))"
        if c == 'pyro_time':
            self._imports.add('time')
            return "time.Now().UnixMilli()"
        if c == 'pyro_write' and len(a) == 1:
            self._imports.add('fmt')
            return f"fmt.Print({self._expr(a[0])})"
        if c == 'pyro_read':
            self._helpers.add('input')
            return 'cryoInput("")'
        args = ', '.join(self._expr(x) for x in a)
        return f"{gid(c)}({args})"

    def _method(self, node: MethodCallExpr) -> str:
        obj = self._expr(node.obj)
        m = node.method
        args = [self._expr(x) for x in node.args]
        if m in ('length', 'size'):
            return f"int64(len({obj}))"
        if m == 'upper':
            self._imports.add('strings'); return f"strings.ToUpper({obj})"
        if m == 'lower':
            self._imports.add('strings'); return f"strings.ToLower({obj})"
        if m == 'contains':
            self._imports.add('strings')
            arg = args[0] if args else '""'
            return f"strings.Contains({obj}, {arg})"
        if m == 'slice':
            s, e = (args + ['0', '0'])[:2]
            return f"{obj}[{s}:{e}]"
        if m == 'pop_last':
            return f"{obj}[len({obj})-1]"
        # fallback: metodo desconhecido
        return f"{obj}.{gid(m)}({', '.join(args)})"

    def _stmt_call(self, node) -> str:
        """Chamada em posicao de statement (trata push -> append)."""
        if isinstance(node, MethodCallExpr) and node.method == 'push':
            obj = self._expr(node.obj)
            arg = self._expr(node.args[0]) if node.args else 'nil'
            return f"{obj} = append({obj}, {arg})"
        return self._expr(node)

    # ── util ────────────────────────────────────────────────

    @staticmethod
    def _go_string(s: str) -> str:
        esc = (s.replace('\\', '\\\\').replace('"', '\\"')
                .replace('\n', '\\n').replace('\t', '\\t').replace('\r', '\\r'))
        return f'"{esc}"'
