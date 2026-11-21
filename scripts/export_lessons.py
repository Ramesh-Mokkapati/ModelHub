"""Copies the lessons applications taught this PC's ModelHub into the repository (docs/lessons), so a clone starts with them.

    python scripts/export_lessons.py                      # port-fix and refactor-fix
    python scripts/export_lessons.py reveng-fix --dry-run

For each domain, <home>/<domain>.jsonl and <home>/<domain>.md (home: %LOCALAPPDATA%\\ModelHub\\lessons, or --home) are
merged into docs/lessons/<domain>.jsonl and .md: only the records (same key, model, removed, added) and the playbook
sections ("## heading" or text) the repository lacks are added; nothing already there changes. On the way, a program's own
file names and paths are hidden, as the applications do before they send a lesson: 'VDL_String.h' -> '<file1>.h' (the same
number for the same file within one record), C:\\src\\... -> <path>. System, CRT, ATL/MFC and SDK headers and libraries keep
their names. Everything that was masked is listed, so the change can be reviewed before it is committed.

The hub brings shipped lessons in when it starts (LessonStore::Seed): a new install gets them all, an existing one the ones
it lacks.
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

SYSTEM_HEADER = re.compile(
    r"^(afx\w*|atl\w*|std\w*|tchar|windows|win\w+|wchar|string|strsafe|shlwapi|shlobj\w*|shellapi|ole2|oleauto|olectl|oledb\w*|"
    r"objbase|objidl|comdef|comutil|commctrl|commdlg|combaseapi|unknwn|wtypes|basetsd|sal|specstrings\w*|process|io|direct|"
    r"time|math|malloc|memory|mbstring|mbctype|ctype|wctype|errno|limits|locale|signal|stdarg|stddef|assert|crtdbg|intrin|"
    r"tlhelp32|psapi|sql|sqlext|sqltypes|sqlucode|odbcinst|mmsystem|richedit|rpc\w*|lmcons|lmerr|iphlpapi|dbghelp|setupapi|"
    r"cfgmgr32|devguid|winternl|ntstatus|unistd|fcntl|dirent|pthread|dlfcn|termios|netdb|pch|resource|targetver)\.(h|hpp)$")
SYSTEM_LIBRARY = re.compile(
    r"^(kernel32|user32|gdi32|advapi32|ws2_32|wsock32|ole32|oleaut32|shell32|comctl32|comdlg32|msvcrtd?|ucrtbased?|"
    r"vcruntime\w*|msvcp\w*|libcmtd?|libucrtd?|libvcruntimed?|ntdll|wininet|winhttp|crypt32|bcrypt|odbc32|odbccp32|version|"
    r"shlwapi|psapi|iphlpapi|netapi32|secur32|setupapi|dbghelp|winmm|uuid|rpcrt4|mscoree|mscorlib|mfc\w*|atls?\w*)\.(lib|dll)$")
# a drive letter only after something that is not a letter or digit: "https://..." is not drive s:
WIN_PATH = re.compile(r"""(?<![A-Za-z0-9_])(?:[A-Za-z]:|\\\\[^\\/\s'"<>|]+)[\\/][^\s'"<>|*?()\[\]]*""")
UNIX_PATH = re.compile(r"""(^|[\s'"(\[=])(/(?:[\w.\-]+/)+[\w.\-]+)""")
FILE_NAME = re.compile(
    r"\b([A-Za-z0-9_\-]+(?:\.[A-Za-z0-9_\-]+)*)\.(cpp|cxx|cc|c|h|hpp|hxx|hh|inl|ipp|rc|rc2|idl|odl|def|vcxproj|vcproj|sln|"
    r"csproj|props|targets|lib|dll|exe|sys|ocx|obj|pdb|pch|java|class|jar|war|kt|cs|vb|config|resx|properties|ini|cfg|conf|"
    r"xml|json|txt|log|dat|db|csv|ico|bmp|png|res|manifest)\b", re.IGNORECASE)
LITERAL = re.compile(r'"(?:[^"\\\n]|\\.)*"')
ANGLE_INCLUDE = re.compile(r"#\s*include\s*<([^>]+)>")


class Mask:
    """Hides file names and paths; the same name gets the same placeholder within one mask."""

    def __init__(self, keep=()):
        self.keep = {k.lower() for k in keep}
        self.names = {}
        self.masked = set()  # what was hidden, for the review list

    def _masked(self, text, unix_paths):
        def path(m):
            self.masked.add(m.group(0))
            return "<path>"

        s = WIN_PATH.sub(path, text)
        if unix_paths:
            def upath(m):
                self.masked.add(m.group(2))
                return m.group(1) + "<path>"
            s = UNIX_PATH.sub(upath, s)

        def name(m):
            whole, lower = m.group(0), m.group(0).lower()
            at = m.start()
            if (at > 0 and s[at - 1] == "<") or SYSTEM_HEADER.match(lower) or SYSTEM_LIBRARY.match(lower) or lower in self.keep:
                return whole
            if lower not in self.names:
                self.names[lower] = "<file%d>" % (len(self.names) + 1)
            self.masked.add(whole)
            return self.names[lower] + "." + m.group(2)

        return FILE_NAME.sub(name, s)

    def message(self, text):
        return self._masked(text, True)

    def code(self, text):
        return LITERAL.sub(lambda m: self._masked(m.group(0), False), text)


def masked_record(r, review):
    r = dict(r)
    keep = set()
    for field in ("removed", "added"):
        lines = r.get(field)
        for line in (lines if isinstance(lines, list) else [lines or ""]):
            for inc in ANGLE_INCLUDE.findall(line):
                keep.add(re.split(r"[\\/]", inc)[-1])
    mask = Mask(keep)
    for field in ("removed", "added"):
        v = r.get(field)
        if isinstance(v, list):
            r[field] = [mask.code(x) if isinstance(x, str) else x for x in v]
        elif isinstance(v, str):
            r[field] = mask.code(v)
    for field in ("error", "note"):
        if isinstance(r.get(field), str):
            r[field] = mask.message(r[field])
    # the keys are matched against the applications' own masked keys: masked alone, the same way
    for field in ("detail", "key"):
        if isinstance(r.get(field), str):
            alone = Mask()
            r[field] = alone.message(r[field])
            review |= alone.masked
    review |= mask.masked
    return r


def identity(r):
    dump = lambda v: json.dumps(v if v is not None else [], sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return "\x1f".join([r.get("key", ""), r.get("model", ""), dump(r.get("removed")), dump(r.get("added"))])


def read_records(path):
    out = []
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(r, dict) and r.get("key"):
                out.append(r)
    return out


def sections(text):
    """The "## " sections of a playbook, each with its heading line (the title before the first is left out)."""
    parts = re.split(r"(?m)^(?=## )", text)
    return [p.rstrip("\n") for p in parts if p.startswith("## ")]


def has_section(current, section):
    heading = section.split("\n", 1)[0]
    return re.search(r"(?m)^" + re.escape(heading) + r"$", current) is not None or section.strip() in current


def export(domain, home, out_dir, dry_run):
    review = set()
    added_records = added_sections = 0
    src_records = home / (domain + ".jsonl")
    dst_records = out_dir / (domain + ".jsonl")
    if src_records.exists():
        existing = read_records(dst_records)
        seen = {identity(r) for r in existing}
        new = []
        for r in read_records(src_records):
            m = masked_record(r, review)
            if identity(m) not in seen:
                seen.add(identity(m))
                new.append(m)
        added_records = len(new)
        if new and not dry_run:
            with dst_records.open("a", encoding="utf-8", newline="\n") as f:
                for r in new:
                    f.write(json.dumps(r, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n")
    src_book = home / (domain + ".md")
    dst_book = out_dir / (domain + ".md")
    if src_book.exists():
        mask = Mask()
        shipped = mask.message(src_book.read_text(encoding="utf-8"))
        review |= mask.masked
        current = dst_book.read_text(encoding="utf-8") if dst_book.exists() else ""
        if not current:
            current = shipped if shipped.endswith("\n") else shipped + "\n"
            added_sections = len(sections(shipped))
        else:
            for s in sections(shipped):
                if has_section(current, s):
                    continue
                current += ("" if current.endswith("\n") else "\n") + "\n" + s.strip() + "\n"
                added_sections += 1
        if added_sections and not dry_run:
            dst_book.write_text(current, encoding="utf-8", newline="\n")
    print("%s: %d record(s) and %d playbook section(s) %s" % (domain, added_records, added_sections, "would be added" if dry_run else "added"))
    for item in sorted(review):
        print("  masked: " + item)


def main():
    repo = Path(__file__).resolve().parent.parent
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("domains", nargs="*", default=["port-fix", "refactor-fix"])
    p.add_argument("--home", type=Path, default=Path(os.environ.get("LOCALAPPDATA", "")) / "ModelHub" / "lessons")
    p.add_argument("--out", type=Path, default=repo / "docs" / "lessons")
    p.add_argument("--dry-run", action="store_true", help="say what would be added, change nothing")
    a = p.parse_args()
    if not a.home.is_dir():
        sys.exit("no lessons folder at %s" % a.home)
    a.out.mkdir(parents=True, exist_ok=True)
    for d in a.domains:
        if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.\-]{0,63}", d):
            sys.exit("not a domain: " + d)
        export(d, a.home, a.out, a.dry_run)


if __name__ == "__main__":
    main()
