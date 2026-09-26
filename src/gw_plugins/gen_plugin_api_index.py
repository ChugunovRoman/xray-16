"""Generates wiki/doc/plugins/api/all.md: the full list of the Plugin API, built from the public headers.

Usage (from the GW folder):
    python xray-16/src/gw_plugins/gen_plugin_api_index.py [--headers DIR] [--out FILE] [--check]

    --headers  folder with gwp_api.h and gwp.hpp (default: ../xrAddonHost/include/gwp next to this script)
    --out      page to write (default: <GW>/wiki/doc/plugins/api/all.md)
    --check    do not write; exit 1 when the page is out of date (for CI)

Runs automatically from build_plugins.cmd (same folder) before every plugins build.
The wiki navigation block is built by <GW>/tools/wiki_plugins_toc.py; without the wiki or that
script the page is skipped (engine-only checkouts such as the engine CI).

What is listed: GwpEngineApi functions and header fields, the entry point, GwpPluginDesc fields,
types, enums, GWP_* macros, public methods of the C++ helpers in gwp.hpp.

Tags (in the comment right above a declaration, valid up to the next empty line):
    @group <name>    API group; must be a key of GROUPS below (= a wiki page)
    @thread main|any main: main thread only; any: callable from any thread
Every GwpEngineApi function and every public method of a gwp.hpp class must have both tags.
Errors are printed as "file(line): error GWPDOC: ..." so Visual Studio shows them in the Error List;
exit code 2. Unknown declarations inside the parsed structs/classes are errors too, so nothing is
silently missing from the page.

Adding a group: add it to GROUPS, create the wiki page, add the page to tools/wiki_plugins_toc.py.
"""

import argparse
import os
import re
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))  # xray-16/src/gw_plugins
ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "..", ".."))  # GW: xray-16, wiki, tools side by side
DEFAULT_HEADERS = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "xrAddonHost", "include", "gwp"))
DEFAULT_OUT = os.path.join(ROOT, "wiki", "doc", "plugins", "api", "all.md")
TOC_SCRIPT_DIR = os.path.join(ROOT, "tools")  # wiki_plugins_toc.py: navigation block of wiki pages
PAGE_REL = "api/all.md"  # path of the page inside doc/plugins, for the TOC generator
BASE = "https://gitlab.com/great-war/wiki/-/tree/master/doc/plugins/"

# @group tag -> (title, page inside doc/plugins)
GROUPS = {
    "core": ("Базовые функции", "api/core.md"),
    "logger": ("Форматированный лог", "api/logger.md"),
}

THREADS = {
    "main": "главный",
    "any": "любой",
}

# Macros that are not part of the API (include guards).
IGNORED_MACROS = {"GWP_API_H", "GWP_HPP"}


class ApiError(Exception):
    def __init__(self, path, line, message):
        super().__init__(message)
        self.path = path
        self.line = line
        self.message = message

    def __str__(self):
        # Visual Studio parses "file(line): error CODE: text" into the Error List (double click opens the line).
        return "%s(%d): error GWPDOC: %s" % (os.path.normpath(self.path), self.line, self.message)


# ---------------------------------------------------------------------------------------------
# Source reading
# ---------------------------------------------------------------------------------------------

class SourceLine:
    """One physical line split into code (comments and string literals removed) and comment text."""

    def __init__(self, number, code, comment):
        self.number = number
        self.code = code
        self.comment = comment


def split_source(text, keep_strings=False):
    """Splits C/C++ source into SourceLine objects. Comments go to `comment`. String/char literals are
    blanked in `code` (so braces inside them do not count) unless keep_strings is set (macro values)."""
    result = []
    in_block = False
    for number, raw in enumerate(text.split("\n"), start=1):
        code, comment = [], []
        i = 0
        while i < len(raw):
            if in_block:
                end = raw.find("*/", i)
                if end < 0:
                    comment.append(raw[i:])
                    i = len(raw)
                else:
                    comment.append(raw[i:end])
                    i = end + 2
                    in_block = False
                continue
            ch = raw[i]
            if raw.startswith("/*", i):
                in_block = True
                i += 2
            elif raw.startswith("//", i):
                comment.append(raw[i + 2:])
                i = len(raw)
            elif ch in "\"'":
                j = i + 1
                while j < len(raw) and raw[j] != ch:
                    j += 2 if raw[j] == "\\" else 1
                literal = raw[i:j + 1]
                code.append(literal if keep_strings else ch + " " * (len(literal) - 2) + ch)
                i = j + 1
            else:
                code.append(ch)
                i += 1
        result.append(SourceLine(number, "".join(code).rstrip(), " ".join(c.strip() for c in comment).strip()))
    return result


def normalize(text):
    return re.sub(r"\s+", " ", text).strip()


def parse_tags(comment):
    group = re.search(r"@group\s+([a-z_][a-z0-9_]*)", comment)
    thread = re.search(r"@thread\s+([a-z_]+)\b", comment)
    if not group and not thread:
        return None
    return {"group": group.group(1) if group else None, "thread": thread.group(1) if thread else None}


def require_tags(tags, path, line, what):
    if not tags or not tags.get("group") or not tags.get("thread"):
        raise ApiError(path, line, "%s has no '@group <name> @thread main|any' tags in the comment above it" % what)
    if tags["group"] not in GROUPS:
        raise ApiError(path, line, "%s: unknown @group '%s' (known: %s); add it to GROUPS in %s"
                       % (what, tags["group"], ", ".join(sorted(GROUPS)), os.path.basename(__file__)))
    if tags["thread"] not in THREADS:
        raise ApiError(path, line, "%s: @thread must be 'main' or 'any', got '%s'" % (what, tags["thread"]))


def find_line(lines, pattern, path, what):
    for index, line in enumerate(lines):
        if re.search(pattern, line.code):
            return index
    raise ApiError(path, 1, "cannot find %s" % what)


# ---------------------------------------------------------------------------------------------
# gwp_api.h
# ---------------------------------------------------------------------------------------------

FUNC_PTR_RE = re.compile(r"^(?P<ret>.+?)\s*\(\s*GWP_CALL\s*\*\s*(?P<name>\w+)\s*\)\s*\((?P<args>.*)\)\s*;$")
FIELD_RE = re.compile(r"^(?P<type>[\w\s\*]+?)\s*\b(?P<name>\w+)\s*;$")


def parse_struct(lines, path, struct_name, need_tags):
    """Fields and function pointers of `typedef struct <name> { ... } <name>;` in declaration order."""
    start = find_line(lines, r"^\s*typedef\s+struct\s+%s\s*$" % struct_name, path, "struct " + struct_name)
    items = []
    pending_tags = None
    statement, statement_line, statement_tags = "", 0, None
    index = start + 1
    if lines[index].code.strip() != "{":
        raise ApiError(path, lines[index].number, "expected '{' after 'typedef struct %s'" % struct_name)
    index += 1
    while index < len(lines):
        line = lines[index]
        code = line.code.strip()
        if re.match(r"^\}\s*%s\s*;$" % struct_name, code):
            return items
        if not code:
            if line.comment:
                # A comment with tags replaces the pending tags; a comment without tags keeps them.
                pending_tags = parse_tags(line.comment) or pending_tags
            elif not statement:
                pending_tags = None  # empty line ends the scope of a tag comment
            index += 1
            continue
        if not statement:
            statement_line, statement_tags = line.number, pending_tags
        statement = (statement + " " + code).strip()
        if statement.endswith(";"):
            decl = normalize(statement)
            fn = FUNC_PTR_RE.match(decl)
            field = FIELD_RE.match(decl)
            if fn:
                name = fn.group("name")
                if need_tags:
                    require_tags(statement_tags, path, statement_line, "%s::%s" % (struct_name, name))
                items.append({
                    "kind": "function",
                    "name": name,
                    "signature": "%s %s(%s)" % (normalize(fn.group("ret")), name, normalize(fn.group("args"))),
                    "pointer_type": "%s (*)(%s)" % (normalize(fn.group("ret")), normalize(fn.group("args"))),
                    "tags": statement_tags,
                    "line": statement_line,
                })
            elif field:
                items.append({"kind": "field", "name": field.group("name"), "type": normalize(field.group("type")),
                              "line": statement_line})
            else:
                raise ApiError(path, statement_line, "unrecognized declaration in %s: '%s'" % (struct_name, decl))
            statement = ""
        index += 1
    raise ApiError(path, lines[start].number, "struct %s is not closed" % struct_name)


def parse_c_header(path):
    text = open(path, encoding="utf-8").read().replace("\r\n", "\n")
    lines = split_source(text)
    # Comments removed, string literals kept (macro values such as GWP_PLUGIN_ENTRY_NAME); "\" continuations joined.
    joined = "\n".join(line.code for line in split_source(text, keep_strings=True)).replace("\\\n", " ")

    api = {}
    api["engine"] = parse_struct(lines, path, "GwpEngineApi", need_tags=True)
    api["desc"] = parse_struct(lines, path, "GwpPluginDesc", need_tags=False)

    # typedefs
    types = []
    for m in re.finditer(r"typedef\s+struct\s+(\w+)\s+(\w+)\s*;", joined):
        types.append((m.group(2), "непрозрачная структура (`struct %s`)" % m.group(1)))
    for m in re.finditer(r"typedef\s+((?:unsigned\s+)?\w+_t|int|unsigned)\s+(\w+)\s*;", joined):
        types.append((m.group(2), "`%s`" % m.group(1)))
    init_fn = None
    for m in re.finditer(r"typedef\s+(\w+)\s*\(\s*GWP_CALL\s*\*\s*(\w+)\s*\)\s*\((.*?)\)\s*;", joined, re.S):
        signature = "%s (*)(%s)" % (m.group(1), normalize(m.group(3)))
        types.append((m.group(2), "`%s`" % signature))
        if m.group(2) == "GwpPluginInitFn":
            init_fn = (m.group(1), normalize(m.group(3)))
    api["types"] = types

    # enums
    enums = []
    for m in re.finditer(r"typedef\s+enum\s+(\w+)\s*\{(.*?)\}\s*(\w+)\s*;", joined, re.S):
        values = [normalize(v) for v in m.group(2).split(",") if normalize(v)]
        enums.append((m.group(3), values))
    api["enums"] = enums

    # macros: name -> list of definitions (platform branches define some macros twice)
    macros = {}
    order = []
    for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+(GWP_\w+)(\([^)]*\))?[ \t]*(.*)$", joined, re.M):
        name = m.group(1)
        if name in IGNORED_MACROS:
            continue
        if name not in macros:
            macros[name] = []
            order.append(name)
        macros[name].append((m.group(2) or "", normalize(m.group(3))))
    api["macros"] = [(name, macros[name]) for name in order]

    # version
    version = []
    for part in ("MAJOR", "MINOR", "PATCH"):
        values = macros.get("GWP_API_VERSION_" + part)
        if not values:
            raise ApiError(path, 1, "GWP_API_VERSION_%s is not defined" % part)
        version.append(values[0][1].rstrip("uU"))
    api["version"] = ".".join(version)

    # entry point
    entry = macros.get("GWP_PLUGIN_ENTRY_NAME")
    if not entry or not init_fn:
        raise ApiError(path, 1, "GWP_PLUGIN_ENTRY_NAME or GwpPluginInitFn is not defined")
    api["entry"] = "%s %s(%s)" % (init_fn[0], entry[0][1].strip('"'), init_fn[1])
    return api


# ---------------------------------------------------------------------------------------------
# gwp.hpp (C++ helpers)
# ---------------------------------------------------------------------------------------------

def signature_of(decl, class_name):
    """Declaration text up to the end of its parameter list plus trailing qualifiers."""
    decl = normalize(decl)
    decl = re.sub(r"^template\s*<.*?>\s*", "", decl)
    open_paren = decl.find("(")
    if open_paren < 0:
        return None, None
    depth = 0
    close_paren = -1
    for i in range(open_paren, len(decl)):
        if decl[i] == "(":
            depth += 1
        elif decl[i] == ")":
            depth -= 1
            if depth == 0:
                close_paren = i
                break
    if close_paren < 0:
        return None, None
    head = decl[:open_paren].strip()
    name = head.split()[-1] if head else ""
    qualifiers = re.match(r"^\s*((?:const|noexcept|override|final)\s*)*", decl[close_paren + 1:]).group(0).strip()
    signature = (decl[:close_paren + 1] + (" " + qualifiers if qualifiers else "")).strip()
    return name, signature


def parse_cpp_header(path):
    text = open(path, encoding="utf-8").read().replace("\r\n", "\n")
    lines = split_source(text)
    classes = []
    index = 0
    while index < len(lines):
        m = re.match(r"^\s*class\s+(\w+)\s*$", lines[index].code)
        if not m:
            index += 1
            continue
        class_name = m.group(1)
        class_line = lines[index].number
        index += 1
        if index >= len(lines) or lines[index].code.strip() != "{":
            raise ApiError(path, class_line, "expected '{' on the line after 'class %s'" % class_name)
        index += 1
        methods = []
        access = "private"
        pending_tags = None
        statement, statement_line, statement_tags = "", 0, None
        depth = 0  # brace depth inside the class body (method bodies)
        while index < len(lines):
            line = lines[index]
            code = line.code.strip()
            if depth == 0 and re.match(r"^\}\s*;$", code):
                break
            if depth > 0:
                depth += code.count("{") - code.count("}")
                index += 1
                continue
            if not code:
                if line.comment:
                    tags = parse_tags(line.comment)
                    if tags:
                        pending_tags = tags
                elif not statement:
                    pending_tags = None
                index += 1
                continue
            access_match = re.match(r"^(public|private|protected)\s*:$", code)
            if access_match:
                access = access_match.group(1)
                pending_tags = None
                index += 1
                continue
            if not statement:
                statement_line, statement_tags = line.number, pending_tags
            statement = (statement + " " + code).strip()
            if statement.startswith("template") and "(" not in statement:
                index += 1  # "template <...>" line: the declaration continues below
                continue
            brace = statement.find("{")
            semicolon = statement.find(";")
            if brace < 0 and semicolon < 0:
                index += 1
                continue
            cut = min(p for p in (brace, semicolon) if p >= 0)
            decl = statement[:cut]
            if brace >= 0 and (semicolon < 0 or brace < semicolon):
                depth = statement.count("{") - statement.count("}")
            if access == "public" and "(" in decl:
                name, signature = signature_of(decl, class_name)
                if not name:
                    raise ApiError(path, statement_line, "unrecognized declaration in class %s: '%s'"
                                   % (class_name, normalize(decl)))
                what = "%s::%s" % (class_name, name)
                require_tags(statement_tags, path, statement_line, what)
                methods.append({"name": name, "signature": signature, "tags": statement_tags,
                                "line": statement_line})
            elif access == "public":
                raise ApiError(path, statement_line, "unrecognized public declaration in class %s: '%s'"
                               % (class_name, normalize(decl)))
            statement = ""
            index += 1
        classes.append({"name": class_name, "methods": methods})
        index += 1
    return classes


# ---------------------------------------------------------------------------------------------
# Page
# ---------------------------------------------------------------------------------------------

def cell(text):
    return text.replace("|", "\\|")


def code(text):
    return "`%s`" % cell(text)


def group_link(tags):
    title, page = GROUPS[tags["group"]]
    return "[%s](%s)" % (title, BASE + page)


def render(api, classes):
    engine_functions = [i for i in api["engine"] if i["kind"] == "function"]
    engine_fields = [i for i in api["engine"] if i["kind"] == "field"]
    method_count = sum(len(c["methods"]) for c in classes)
    out = []
    w = out.append
    w('# Раздел "Plugin API: полный список"')
    w("")
    w("{{TOC}}")
    w("")
    w("> Страница создаётся автоматически скриптом `xray-16/src/gw_plugins/gen_plugin_api_index.py` из заголовков `gwp_api.h` "
      "и `gwp.hpp` при каждой сборке плагинов. Не редактируйте её вручную: изменения будут перезаписаны. "
      "Подробные описания — на страницах групп по ссылкам в таблицах.")
    w("")
    w("Plugin API **%s**: %d функций движка, %d методов C++-помощников." % (api["version"], len(engine_functions),
                                                                           method_count))
    w("")
    w("Колонка «Поток»: **главный** — вызывать только с главного потока движка; **любой** — можно с любого потока.")
    w("")

    w("## Функции движка (GwpEngineApi)")
    w("")
    w("Вызываются через таблицу, которую движок передаёт в `gwp_plugin_init`: `api->имя(...)`.")
    w("")
    w("| Функция | Сигнатура | Поток | Группа |")
    w("|---|---|---|---|")
    for f in engine_functions:
        w("| %s | %s | %s | %s |" % (code(f["name"]), code(f["signature"]), THREADS[f["tags"]["thread"]],
                                     group_link(f["tags"])))
    w("")
    w("Заголовок таблицы (одинаков во всех версиях, всегда в начале):")
    w("")
    w("| Поле | Тип |")
    w("|---|---|")
    for f in engine_fields:
        w("| %s | %s |" % (code(f["name"]), code(f["type"])))
    w("")

    w("## Точка входа")
    w("")
    w("Экспортируется каждым плагином, объявляется макросом `GWP_PLUGIN_INIT`:")
    w("")
    w("```c")
    w(api["entry"] + ";")
    w("```")
    w("")

    w("## Описание плагина (GwpPluginDesc)")
    w("")
    w("Заполняется плагином в `gwp_plugin_init`.")
    w("")
    w("| Поле | Тип |")
    w("|---|---|")
    for f in api["desc"]:
        w("| %s | %s |" % (code(f["name"]), code(f["type"] if f["kind"] == "field" else f["pointer_type"])))
    w("")

    w("## C++-помощники (gwp.hpp)")
    w("")
    for c in classes:
        w("### gwp::%s" % c["name"])
        w("")
        w("| Метод | Сигнатура | Поток | Группа |")
        w("|---|---|---|---|")
        for m in c["methods"]:
            w("| %s | %s | %s | %s |" % (code(m["name"]), code(m["signature"]), THREADS[m["tags"]["thread"]],
                                         group_link(m["tags"])))
        w("")

    w("## Типы")
    w("")
    w("| Имя | Определение |")
    w("|---|---|")
    for name, definition in api["types"]:
        w("| %s | %s |" % (code(name), cell(definition)))
    w("")

    w("## Перечисления")
    w("")
    w("| Тип | Значения |")
    w("|---|---|")
    for name, values in api["enums"]:
        w("| %s | %s |" % (code(name), ", ".join(code(v) for v in values)))
    w("")

    w("## Макросы и константы")
    w("")
    w("| Имя | Значение |")
    w("|---|---|")
    for name, definitions in api["macros"]:
        params = definitions[0][0]
        if len(definitions) > 1:
            value = "несколько определений (зависит от платформы или языка C/C++)"
        elif params:
            value = "макрос-функция"
        elif definitions[0][1]:
            value = code(definitions[0][1])
        else:
            value = ""
        w("| %s | %s |" % (code(name + params), value))
    w("")
    return "\n".join(out)


def load_toc_module():
    """<GW>/tools/wiki_plugins_toc.py, or None when it is not there."""
    if not os.path.isfile(os.path.join(TOC_SCRIPT_DIR, "wiki_plugins_toc.py")):
        return None
    sys.path.insert(0, TOC_SCRIPT_DIR)
    sys.dont_write_bytecode = True  # do not leave __pycache__ in tools/
    import wiki_plugins_toc  # noqa: E402
    return wiki_plugins_toc


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--headers", default=DEFAULT_HEADERS)
    parser.add_argument("--out", default=DEFAULT_OUT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    c_header = os.path.join(args.headers, "gwp_api.h")
    cpp_header = os.path.join(args.headers, "gwp.hpp")
    out_dir = os.path.dirname(os.path.abspath(args.out))
    if not os.path.isdir(out_dir):
        print("[plugin api index] wiki folder not found (%s), skipped" % out_dir)
        return 0

    try:
        api = parse_c_header(c_header)
        classes = parse_cpp_header(cpp_header)
    except ApiError as error:
        print(error)
        return 2
    except OSError as error:
        print("%s(1): error GWPDOC: %s" % (error.filename or args.headers, error.strerror))
        return 2

    toc = load_toc_module()
    if toc is None:
        print("[plugin api index] %s not found, skipped" % os.path.join(TOC_SCRIPT_DIR, "wiki_plugins_toc.py"))
        return 0
    text = toc.apply_toc(render(api, classes), PAGE_REL)

    old = None
    if os.path.exists(args.out):
        old = open(args.out, "rb").read().decode("utf-8").replace("\r\n", "\n")
    if old == text:
        print("[plugin api index] up to date: %s" % args.out)
        return 0
    if args.check:
        print("%s(1): error GWPDOC: page is out of date, run xray-16/src/gw_plugins/gen_plugin_api_index.py" % args.out)
        return 1
    open(args.out, "wb").write(text.encode("utf-8"))
    print("[plugin api index] written: %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
