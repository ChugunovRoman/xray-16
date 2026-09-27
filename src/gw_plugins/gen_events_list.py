"""Generates wiki/doc/plugins/api/events_list.md: the list of event bus events, built from the engine and script sources.

Usage (from the GW folder):
    python xray-16/src/gw_plugins/gen_events_list.py [--xrgame DIR] [--axr-main FILE] [--meta FILE] [--manual FILE]
                                                     [--out FILE] [--check]

    --xrgame    folder with addon_event_bus.cpp and addon_object_events.cpp (default: ../xrGame next to this script)
    --axr-main  axr_main.script with the intercepts table (default: <GW>/GlobalWar/gamedata/scripts/axr_main.script)
    --meta      descriptions of the events (default: events_meta.json next to this script)
    --manual    hand-written text blocks of the page (default: events_list_manual.md next to this script)
    --out       page to write (default: <GW>/wiki/doc/plugins/api/events_list.md)
    --check     do not write; exit 1 when the page is out of date (for CI)

Runs automatically from build_plugins.cmd (same folder) before every plugins build.
The wiki navigation block is built by <GW>/tools/wiki_plugins_toc.py; without the wiki, that script or axr_main.script
the page is skipped (engine-only checkouts such as the engine CI).

Where the data comes from:
    addon_event_bus.cpp      built-in events and their schemas (kBuiltinNames, kBuiltinSchemas), events with a result
                             (kLuaResultBindings), valid schema codes (IsValidSchema)
    addon_object_events.cpp  events whose source moves to the engine (kEngineEvents: name -> group), groups
                             (kGroupNames), groups the engine emits in this build (kImplementedGroups)
    axr_main.script          script events and their schemas (local intercepts = { name = "schema", ... })
    events_meta.json         hand-written: page sections and, per event, when it comes, its arguments, notes
    events_list_manual.md    hand-written text blocks (<!-- @block name --> ... ): intro, notation, event sequence, ...

Every event (built-in or from intercepts) must have an entry in events_meta.json, and every entry must name an existing
event. The number of described arguments must match the schema. Errors are printed as "file(line): error GWPDOC: ..."
so Visual Studio shows them in the Error List; exit code 2. An argument type in the description that the schema does
not allow is printed as a warning (the page is still written).

Adding a script event: add it to intercepts in axr_main.script with its schema, add its entry to events_meta.json,
run the script.
"""

import argparse
import json
import os
import re
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))  # xray-16/src/gw_plugins
ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "..", ".."))  # GW: xray-16, wiki, tools, GlobalWar side by side
DEFAULT_XRGAME = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "xrGame"))
DEFAULT_AXR_MAIN = os.path.join(ROOT, "GlobalWar", "gamedata", "scripts", "axr_main.script")
DEFAULT_META = os.path.join(SCRIPT_DIR, "events_meta.json")
DEFAULT_MANUAL = os.path.join(SCRIPT_DIR, "events_list_manual.md")
DEFAULT_OUT = os.path.join(ROOT, "wiki", "doc", "plugins", "api", "events_list.md")
TOC_SCRIPT_DIR = os.path.join(ROOT, "tools")  # wiki_plugins_toc.py: navigation block of wiki pages
PAGE_REL = "api/events_list.md"  # path of the page inside doc/plugins, for the TOC generator
BASE = "https://gitlab.com/great-war/wiki/-/tree/master/doc/plugins/"

# Schema codes (must be the same set as in IsValidSchema of addon_event_bus.cpp) -> description on the page.
CODES = [
    ("b", "`BOOL`"),
    ("I", "целое: `INT` или `NUMBER` без дробной части (числа из Lua всегда `NUMBER`)"),
    ("N", "число: `NUMBER` или `INT`"),
    ("s", "`STRING`"),
    ("v", "`VEC3`"),
    ("o", "`OBJECT` — онлайн-объект"),
    ("O", "`SERVER_OBJECT` — серверный объект ALife"),
    ("t", "`LUA_REF` — Lua-таблица или userdata без нативной формы"),
    ("*", "любой тип, в том числе `NIL` и отсутствующий аргумент"),
]

# GwpValue type written at the start of an argument description -> schema codes that allow it.
TYPE_CODES = {
    "OBJECT": "o",
    "SERVER_OBJECT": "O",
    "NUMBER": "IN",
    "INT": "IN",
    "STRING": "s",
    "BOOL": "b",
    "VEC3": "v",
    "LUA_REF": "t",
}

# Blocks of events_list_manual.md, all required.
MANUAL_BLOCKS = ["intro", "notation", "builtin_intro", "builtin_outro", "sequence", "script_intro", "result_outro"]

BUILTIN_SECTION = "builtin"


class DocError(Exception):
    def __init__(self, path, line, message):
        super().__init__(message)
        self.path = path
        self.line = line
        self.message = message

    def __str__(self):
        # Visual Studio parses "file(line): error CODE: text" into the Error List (double click opens the line).
        return "%s(%d): error GWPDOC: %s" % (os.path.normpath(self.path), self.line, self.message)


def warning(path, line, message):
    print("%s(%d): warning GWPDOC: %s" % (os.path.normpath(path), line, message))


# ---------------------------------------------------------------------------------------------
# Source reading
# ---------------------------------------------------------------------------------------------

def read_text(path, encoding):
    try:
        return open(path, "rb").read().decode(encoding).replace("\r\n", "\n")
    except UnicodeDecodeError as error:
        raise DocError(path, 1, "the file is not %s: %s" % (encoding, error))


def strip_cpp_comment(line):
    """Code part of one C++ line: '//' comments removed (the parsed tables have no '//' inside strings)."""
    in_string = False
    i = 0
    while i < len(line):
        ch = line[i]
        if in_string:
            if ch == "\\":
                i += 1
            elif ch == '"':
                in_string = False
        elif ch == '"':
            in_string = True
        elif line.startswith("//", i):
            return line[:i]
        i += 1
    return line


def cpp_block(lines, path, start_pattern, what):
    """(first line number, code of the lines) of an initializer `<start_pattern> ... };`, comments removed."""
    for index, line in enumerate(lines):
        if re.search(start_pattern, line):
            body = []
            for j in range(index, len(lines)):
                code = strip_cpp_comment(lines[j])
                body.append((j + 1, code))
                if re.search(r"\}\s*;", code):
                    return body
            raise DocError(path, index + 1, "%s is not closed with '};'" % what)
    raise DocError(path, 1, "cannot find %s" % what)


def string_items(body):
    """String literals of a block with their line numbers."""
    items = []
    for number, code in body:
        for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', code):
            items.append((number, m.group(1)))
    return items


def schema_positions(schema):
    """Schema -> list of (code, optional). Invalid schemas are rejected before."""
    positions = []
    i = 0
    while i < len(schema):
        code = schema[i]
        optional = code == "*"
        if i + 1 < len(schema) and schema[i + 1] == "?":
            optional = True
            i += 1
        positions.append((code, optional))
        i += 1
    return positions


def valid_schema(schema, codes):
    i = 0
    while i < len(schema):
        if schema[i] not in codes:
            return False
        if i + 1 < len(schema) and schema[i + 1] == "?":
            i += 1
        i += 1
    return True


def parse_event_bus(path):
    lines = read_text(path, "utf-8").split("\n")
    bus = {}

    names = string_items(cpp_block(lines, path, r"constexpr\s+pcstr\s+kBuiltinNames\s*\[\s*\]\s*=", "kBuiltinNames"))
    schemas = string_items(cpp_block(lines, path, r"constexpr\s+pcstr\s+kBuiltinSchemas\s*\[\s*\]\s*=",
                                     "kBuiltinSchemas"))
    if len(names) != len(schemas):
        raise DocError(path, schemas[0][0] if schemas else 1, "kBuiltinSchemas has %d entries, kBuiltinNames has %d"
                       % (len(schemas), len(names)))
    bus["builtin"] = [(name, schema, line) for (line, name), (_, schema) in zip(names, schemas)]

    results = {}
    for number, code in cpp_block(lines, path, r"constexpr\s+LuaResultBinding\s+kLuaResultBindings\s*\[\s*\]\s*=",
                                  "kLuaResultBindings"):
        for m in re.finditer(r'\{\s*"(\w+)"\s*,\s*(\d+)\s*,\s*"(\w+)"\s*\}', code):
            results[m.group(1)] = {"arg": int(m.group(2)), "field": m.group(3), "line": number}
    if not results:
        raise DocError(path, 1, "kLuaResultBindings: no entries found")
    bus["results"] = results

    codes = None
    for index, line in enumerate(lines):
        if re.search(r"\bbool\s+IsValidSchema\s*\(", line):
            for j in range(index, min(index + 15, len(lines))):
                m = re.search(r'strchr\s*\(\s*"([^"]*)"', lines[j])
                if m:
                    codes = (j + 1, m.group(1))
                    break
            break
    if not codes:
        raise DocError(path, 1, "cannot find the schema codes (strchr(\"...\") in IsValidSchema)")
    known = "".join(code for code, _ in CODES)
    if sorted(codes[1]) != sorted(known):
        raise DocError(path, codes[0], "schema codes '%s' differ from CODES '%s' in %s; update the table"
                       % (codes[1], known, os.path.basename(__file__)))
    bus["codes"] = codes[1]
    return bus


def parse_object_events(path):
    lines = read_text(path, "utf-8").split("\n")
    groups = [name for _, name in string_items(cpp_block(lines, path, r"constexpr\s+pcstr\s+kGroupNames\s*\[\s*\]\s*=",
                                                         "kGroupNames"))]
    if not groups:
        raise DocError(path, 1, "kGroupNames: no entries found")

    def group_of(enum_name, line):
        name = enum_name.lower()
        if name not in groups:
            raise DocError(path, line, "EGroup::%s has no name in kGroupNames (%s)" % (enum_name, ", ".join(groups)))
        return name

    engine = {}
    for number, code in cpp_block(lines, path, r"constexpr\s+EngineEvent\s+kEngineEvents\s*\[\s*\]\s*=",
                                  "kEngineEvents"):
        for m in re.finditer(r'\{\s*"(\w+)"\s*,\s*EGroup::(\w+)\s*\}', code):
            if m.group(1) in engine:
                raise DocError(path, number, "kEngineEvents: '%s' is listed twice" % m.group(1))
            engine[m.group(1)] = {"group": group_of(m.group(2), number), "line": number}
    if not engine:
        raise DocError(path, 1, "kEngineEvents: no entries found")

    implemented = None
    for number, line in enumerate(lines, start=1):
        if not re.search(r"constexpr\s+u32\s+kImplementedGroups\s*=", strip_cpp_comment(line)):
            continue
        # The expression may continue on the next lines, up to the ';'.
        text = strip_cpp_comment(line)
        extra = number
        while ";" not in text and extra < len(lines):
            text += " " + strip_cpp_comment(lines[extra])
            extra += 1
        m = re.search(r"kImplementedGroups\s*=\s*(.*?);", text)
        if not m:
            raise DocError(path, number, "kImplementedGroups: no ';' found")
        expr = " ".join(m.group(1).split())
        if expr == "0":
            implemented = []
        elif expr == "kAllGroups":
            implemented = list(groups)
        else:
            found = re.findall(r"EGroup::(\w+)", expr)
            rest = re.sub(r"(Bit\s*\(\s*)?EGroup::\w+(\s*\))?|\||\s", "", expr)
            if not found or rest:
                raise DocError(path, number, "kImplementedGroups: cannot parse '%s' (expected 0, kAllGroups or "
                               "Bit(EGroup::X) | Bit(EGroup::Y) ...)" % expr)
            implemented = [group_of(name, number) for name in found]
        break
    if implemented is None:
        raise DocError(path, 1, "cannot find kImplementedGroups")
    return {"groups": groups, "engine": engine, "implemented": implemented}


def parse_intercepts(path, codes):
    lines = read_text(path, "cp1251").split("\n")
    start = None
    for index, line in enumerate(lines):
        if re.match(r"^\s*local\s+intercepts\s*=\s*\{\s*$", line):
            start = index
            break
    if start is None:
        raise DocError(path, 1, "cannot find 'local intercepts = {'")
    events = {}
    order = []
    for index in range(start + 1, len(lines)):
        number = index + 1
        code = lines[index]
        comment = code.find("--")
        if comment >= 0:
            code = code[:comment]
        code = code.strip()
        if not code:
            continue
        if code.startswith("}"):
            return events, order
        m = re.match(r'^([A-Za-z_]\w*)\s*=\s*"([^"]*)"\s*,?$', code)
        if not m:
            raise DocError(path, number, "intercepts: unrecognized line '%s' (expected: name = \"schema\",)" % code)
        name, schema = m.group(1), m.group(2)
        if name in events:
            raise DocError(path, number, "intercepts: '%s' is listed twice" % name)
        if not valid_schema(schema, codes):
            raise DocError(path, number, "intercepts: invalid schema '%s' of '%s' (codes %s, '?' = may be nil)"
                           % (schema, name, " ".join(codes)))
        events[name] = {"schema": schema, "line": number}
        order.append(name)
    raise DocError(path, start + 1, "intercepts table is not closed")


def parse_manual(path):
    lines = read_text(path, "utf-8").split("\n")
    blocks = {}
    current = None
    for number, line in enumerate(lines, start=1):
        m = re.match(r"^<!--\s*@block\s+(\w+)\s*-->\s*$", line)
        if m:
            current = m.group(1)
            if current not in MANUAL_BLOCKS:
                raise DocError(path, number, "unknown block '%s' (known: %s)" % (current, ", ".join(MANUAL_BLOCKS)))
            if current in blocks:
                raise DocError(path, number, "block '%s' is defined twice" % current)
            blocks[current] = []
            continue
        if current is None:
            continue  # text before the first block: a comment for editors, not on the page
        blocks[current].append(line)
    for name in MANUAL_BLOCKS:
        if name not in blocks:
            raise DocError(path, 1, "block '%s' is missing (add '<!-- @block %s -->')" % (name, name))
    return {name: "\n".join(text).strip("\n") for name, text in blocks.items()}


def parse_meta(path):
    raw = read_text(path, "utf-8")
    raw_lines = raw.split("\n")

    def line_of(key, after=1):
        pattern = re.compile(r'^\s*"%s"\s*:' % re.escape(key))
        for number in range(after, len(raw_lines) + 1):
            if pattern.search(raw_lines[number - 1]):
                return number
        return 1

    def no_duplicates(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise DocError(path, line_of(key), "key '%s' is defined twice" % key)
            result[key] = value
        return result

    try:
        meta = json.loads(raw, object_pairs_hook=no_duplicates)
    except json.JSONDecodeError as error:
        raise DocError(path, error.lineno, "invalid JSON: %s" % error.msg)
    if not isinstance(meta, dict) or not isinstance(meta.get("sections"), list) or \
            not isinstance(meta.get("events"), dict):
        raise DocError(path, 1, "expected an object with 'sections' (array) and 'events' (object)")

    sections = []
    ids = set()
    for section in meta["sections"]:
        sid = section.get("id") if isinstance(section, dict) else None
        if not isinstance(sid, str) or not sid or sid in ids:
            raise DocError(path, 1, "sections: every section needs a unique string 'id'")
        ids.add(sid)
        if not isinstance(section.get("title"), str) or not section["title"]:
            raise DocError(path, 1, "section '%s' has no 'title'" % sid)
        scripts = section.get("scripts", [])
        if not isinstance(scripts, list) or not all(isinstance(s, str) for s in scripts):
            raise DocError(path, 1, "section '%s': 'scripts' must be an array of strings" % sid)
        if "intro" in section and not isinstance(section["intro"], str):
            raise DocError(path, 1, "section '%s': 'intro' must be a string" % sid)
        sections.append(section)
    if BUILTIN_SECTION not in ids:
        raise DocError(path, 1, "sections: section '%s' (built-in engine events) is missing" % BUILTIN_SECTION)

    events = {}
    events_line = line_of("events")
    for name, entry in meta["events"].items():
        line = line_of(name, events_line)
        if not isinstance(entry, dict):
            raise DocError(path, line, "event '%s': expected an object" % name)
        unknown = set(entry) - {"section", "args", "when", "notes", "emitter"}
        if unknown:
            raise DocError(path, line, "event '%s': unknown field(s) %s" % (name, ", ".join(sorted(unknown))))
        if entry.get("section") not in ids:
            raise DocError(path, line, "event '%s': 'section' must be one of %s" % (name, ", ".join(sorted(ids))))
        if not isinstance(entry.get("when"), str) or not entry["when"]:
            raise DocError(path, line, "event '%s' has no 'when'" % name)
        args = entry.get("args")
        if not isinstance(args, list) or not all(isinstance(a, str) and a for a in args):
            raise DocError(path, line, "event '%s': 'args' must be an array of non-empty strings ([] = none)" % name)
        for field in ("notes", "emitter"):
            if field in entry and not isinstance(entry[field], str):
                raise DocError(path, line, "event '%s': '%s' must be a string" % (name, field))
        events[name] = dict(entry, line=line)
    return {"sections": sections, "events": events}


# ---------------------------------------------------------------------------------------------
# Model
# ---------------------------------------------------------------------------------------------

def arg_types(text):
    """GwpValue types listed at the start of an argument description: "`OBJECT` или `NIL` кто" -> [OBJECT, NIL]."""
    m = re.match(r"^`([A-Z_]+)`((?:\s*(?:или|,)\s*`[A-Z_]+`)*)", text)
    if not m:
        return []
    return [m.group(1)] + re.findall(r"`([A-Z_]+)`", m.group(2))


def build(paths, bus, objects, intercepts, meta):
    """Checks the sources against each other and returns the list of events for the page."""
    events = {}
    for name, schema, line in bus["builtin"]:
        events[name] = {"name": name, "schema": schema, "builtin": True}
    intercept_events, _ = intercepts
    for name, info in intercept_events.items():
        if name in events:
            raise DocError(paths["axr_main"], info["line"], "'%s' is a built-in engine event, remove it from intercepts"
                           % name)
        events[name] = {"name": name, "schema": info["schema"], "builtin": False}

    for name, info in objects["engine"].items():
        if name not in events or events[name]["builtin"]:
            raise DocError(paths["objects"], info["line"], "engine-sourced event '%s' is not in intercepts of "
                           "axr_main.script (its schema lives there)" % name)
        events[name]["group"] = info["group"]
    for name, info in bus["results"].items():
        if name not in events:
            raise DocError(paths["bus"], info["line"], "kLuaResultBindings: '%s' is not a known event" % name)
        events[name]["result"] = info

    for name, entry in meta["events"].items():
        if name not in events:
            raise DocError(paths["meta"], entry["line"], "'%s' is not a built-in event and not in intercepts of "
                           "axr_main.script; remove the entry or declare the event" % name)
    for name, event in events.items():
        if name not in meta["events"]:
            where = (paths["bus"], 1) if event["builtin"] else (paths["axr_main"], intercept_events[name]["line"])
            raise DocError(where[0], where[1], "event '%s' has no entry in %s" % (name, os.path.basename(paths["meta"])))
        entry = meta["events"][name]
        is_builtin_section = entry["section"] == BUILTIN_SECTION
        if event["builtin"] != is_builtin_section:
            raise DocError(paths["meta"], entry["line"], "event '%s': section must %sbe '%s'"
                           % (name, "" if event["builtin"] else "not ", BUILTIN_SECTION))
        positions = schema_positions(event["schema"])
        if len(entry["args"]) != len(positions):
            raise DocError(paths["meta"], entry["line"], "event '%s': %d argument(s) described, the schema '%s' has %d"
                           % (name, len(entry["args"]), event["schema"], len(positions)))
        for index, (text, (code, optional)) in enumerate(zip(entry["args"], positions)):
            for gwp_type in arg_types(text):
                allowed = code == "*" or (gwp_type == "NIL" and optional) or code in TYPE_CODES.get(gwp_type, "")
                if not allowed:
                    warning(paths["meta"], entry["line"], "event '%s', argument %d: type %s does not match the schema "
                            "code '%s%s' (schema '%s')" % (name, index, gwp_type, code, "?" if optional else "",
                                                          event["schema"]))
        if "result" in event and "emitter" not in entry:
            raise DocError(paths["meta"], entry["line"], "event '%s' has a result: add 'emitter' (who sends it)" % name)
        event.update(entry)
    return events


# ---------------------------------------------------------------------------------------------
# Page
# ---------------------------------------------------------------------------------------------

def cell(text):
    return text.replace("|", "\\|").replace("\n", " ")


def code(text):
    return "`%s`" % cell(text)


def schema_cell(schema):
    return code(schema) if schema else "`()`"


def group_list(groups):
    return ", ".join(code(g) for g in groups) if groups else "нет"


def source_cell(event, implemented):
    if event["builtin"]:
        return "движок"
    group = event.get("group")
    if not group:
        return "Lua"
    if group in implemented:
        return "движок (группа %s)" % code(group)
    return "Lua; переедет в движок: группа %s" % code(group)


def args_cell(args):
    if not args:
        return "—"
    return cell(", ".join("%d: %s" % (i, a) for i, a in enumerate(args)))


def event_cell(event):
    text = code(event["name"])
    if "result" in event:
        text += " (**результат**)"
    return text


def notes_cell(event):
    parts = []
    result = event.get("result")
    if result:
        parts.append("**результат**: поле `%s` таблицы флагов (у Lua-подписчиков — %d-й аргумент, плагину не "
                     "передаётся), у плагина — `event->result`" % (result["field"], result["arg"]))
    if event.get("notes"):
        parts.append(event["notes"])
    return cell("; ".join(parts))


def render(events, objects, meta, manual):
    implemented = objects["implemented"]
    out = []
    w = out.append
    w('# Раздел "Plugin API: список событий"')
    w("")
    w("{{TOC}}")
    w("")
    w("> Страница создаётся автоматически скриптом `xray-16/src/gw_plugins/gen_events_list.py` при каждой сборке "
      "плагинов: имена и схемы — из движка (`addon_event_bus.cpp`, `addon_object_events.cpp`) и из таблицы "
      "`intercepts` в `axr_main.script`, описания — из `xray-16/src/gw_plugins/events_meta.json` и "
      "`events_list_manual.md`. Не редактируйте её вручную: изменения будут перезаписаны.")
    w("")
    w(manual["intro"])
    w("")
    w("## Содержание")
    w("")
    w("1. [Обозначения](#обозначения)")
    w("2. [Встроенные события движка](#встроенные-события-движка)")
    w("3. [Последовательность событий](#последовательность-событий)")
    w("4. [Скриптовые события](#скриптовые-события)")
    w("5. [События с результатом](#события-с-результатом)")
    w("")

    w("## Обозначения")
    w("")
    w("Колонка «Схема» — схема аргументов события: по одному коду на аргумент, в порядке аргументов. Подробно — "
      "\"[Схема аргументов](%sapi/events.md#схема-аргументов)\"." % BASE)
    w("")
    w("| Код | Что приходит плагину |")
    w("|---|---|")
    for letter, description in CODES:
        w("| %s | %s |" % (code(letter), description))
    w("| `?` после кода | аргумент может быть `NIL` (или отсутствовать) |")
    w("| `()` | аргументов нет |")
    w("")
    w(manual["notation"])
    w("")
    w("Колонка «Источник» — кто отправляет событие: **движок** (из C++) или **Lua** (скрипт через "
      "`SendScriptCallback`). Пометка «переедет в движок: группа X» — событие этапа B: движок начнёт отправлять его "
      "сам, когда группа X будет реализована, а отправка того же события из Lua будет отбрасываться (см. "
      "\"[Источник события](%sapi/events.md#источник-события-lua-или-движок)\"). Группы, которые движок отправляет "
      "в этой сборке: %s." % (BASE, group_list(implemented)))
    w("")

    header = "| Событие | Схема | Источник | Аргументы | Когда | Примечания |"
    divider = "|---|---|---|---|---|---|"

    def table(names):
        w(header)
        w(divider)
        for name in names:
            e = events[name]
            w("| %s | %s | %s | %s | %s | %s |" % (event_cell(e), schema_cell(e["schema"]), source_cell(e, implemented),
                                                   args_cell(e["args"]), cell(e["when"]), notes_cell(e)))
        w("")

    by_section = {}
    for name in meta["events"]:
        by_section.setdefault(meta["events"][name]["section"], []).append(name)

    w("## Встроенные события движка")
    w("")
    w(manual["builtin_intro"])
    w("")
    table(by_section.get(BUILTIN_SECTION, []))
    w(manual["builtin_outro"])
    w("")

    w("## Последовательность событий")
    w("")
    w(manual["sequence"])
    w("")

    w("## Скриптовые события")
    w("")
    w(manual["script_intro"])
    w("")
    for section in meta["sections"]:
        if section["id"] == BUILTIN_SECTION:
            continue
        names = by_section.get(section["id"], [])
        if not names:
            continue
        title = section["title"]
        if section.get("scripts"):
            title += " (%s)" % ", ".join("`%s`" % s for s in section["scripts"])
        w("### " + title)
        w("")
        if section.get("intro"):
            w(section["intro"])
            w("")
        table(names)

    w("## События с результатом")
    w("")
    w("| Событие | Отправляет | Поле таблицы флагов | Позиция таблицы у Lua-подписчиков |")
    w("|---|---|---|---|")
    for name in meta["events"]:
        e = events[name]
        if "result" in e:
            w("| %s | %s | %s | %d-й аргумент |" % (code(name), cell(e["emitter"]), code(e["result"]["field"]),
                                                  e["result"]["arg"]))
    w("")
    w(manual["result_outro"])
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
    parser.add_argument("--xrgame", default=DEFAULT_XRGAME)
    parser.add_argument("--axr-main", default=DEFAULT_AXR_MAIN)
    parser.add_argument("--meta", default=DEFAULT_META)
    parser.add_argument("--manual", default=DEFAULT_MANUAL)
    parser.add_argument("--out", default=DEFAULT_OUT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    out_dir = os.path.dirname(os.path.abspath(args.out))
    if not os.path.isdir(out_dir):
        print("[events list] wiki folder not found (%s), skipped" % out_dir)
        return 0
    if not os.path.isfile(args.axr_main):
        print("[events list] %s not found, skipped" % args.axr_main)
        return 0

    paths = {
        "bus": os.path.join(args.xrgame, "addon_event_bus.cpp"),
        "objects": os.path.join(args.xrgame, "addon_object_events.cpp"),
        "axr_main": args.axr_main,
        "meta": args.meta,
        "manual": args.manual,
    }
    try:
        bus = parse_event_bus(paths["bus"])
        objects = parse_object_events(paths["objects"])
        intercepts = parse_intercepts(paths["axr_main"], bus["codes"])
        meta = parse_meta(paths["meta"])
        manual = parse_manual(paths["manual"])
        events = build(paths, bus, objects, intercepts, meta)
    except DocError as error:
        print(error)
        return 2
    except OSError as error:
        print("%s(1): error GWPDOC: %s" % (error.filename or args.xrgame, error.strerror))
        return 2

    toc = load_toc_module()
    if toc is None:
        print("[events list] %s not found, skipped" % os.path.join(TOC_SCRIPT_DIR, "wiki_plugins_toc.py"))
        return 0
    text = toc.apply_toc(render(events, objects, meta, manual), PAGE_REL)

    old = None
    if os.path.exists(args.out):
        old = open(args.out, "rb").read().decode("utf-8").replace("\r\n", "\n")
    if old == text:
        print("[events list] up to date: %s" % args.out)
        return 0
    if args.check:
        print("%s(1): error GWPDOC: page is out of date, run xray-16/src/gw_plugins/gen_events_list.py" % args.out)
        return 1
    open(args.out, "wb").write(text.encode("utf-8"))
    print("[events list] written: %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
