"""Regenerate the UiText.Texts block in Launcher.cs from ui_texts_source.py.

Usage: python -I launcher/ui_texts_build.py [--check]
"""
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
SOURCE = HERE / "ui_texts_source.py"
TARGET = HERE / "Launcher.cs"
START = "    static readonly Dictionary<string,Dictionary<string,string>> Texts=new Dictionary<string,Dictionary<string,string>>{\n"


def load():
    namespace = {}
    exec(compile(SOURCE.read_text(encoding="utf-8"), str(SOURCE), "exec"), namespace)
    langs, texts = namespace["LANGS"], namespace["T"]
    keys = list(texts["en"])
    for code, _ in langs:
        if list(texts[code]) != keys:
            raise SystemExit(f"{code}: keys differ from English")
        for key in keys:
            if re.findall(r"\{\d\}", texts[code][key]) != re.findall(r"\{\d\}", texts["en"][key]):
                raise SystemExit(f"{code}.{key}: placeholders differ from English")
    return langs, texts


def block(langs, texts):
    quote = lambda value: json.dumps(value, ensure_ascii=False)
    lines = []
    for code, _ in langs:
        pairs = ",".join("{" + quote(key) + "," + quote(value) + "}" for key, value in texts[code].items())
        lines.append("        {" + quote(code) + ",new Dictionary<string,string>{" + pairs + "}}")
    return ",\n".join(lines) + "\n"


def main():
    langs, texts = load()
    source = TARGET.read_text(encoding="utf-8")
    begin = source.index(START) + len(START)
    end = source.index("    };\n", begin)
    updated = source[:begin] + block(langs, texts) + source[end:]
    if "--check" in sys.argv:
        print("UI_TEXTS_UP_TO_DATE" if updated == source else "UI_TEXTS_DIFFER")
        return 0 if updated == source else 1
    TARGET.write_text(updated, encoding="utf-8", newline="")
    print("UI_TEXTS_WRITTEN languages=%d keys=%d" % (len(langs), len(texts["en"])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
