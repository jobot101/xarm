#!/usr/bin/env python3
"""Write web/src/lessons.json from school_lessons.py: the Learning page (the app's main page) shows the course.
Lesson text only, no quiz answers. CI runs this before building the web app."""
import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import school_lessons as SL

names = [L["name"] for L in SL.SCHOOL]
starts = {names.index(first): title for title, first in SL.PARTS}
out, part = [], None
for i, L in enumerate(SL.SCHOOL):
    if i in starts:
        part = {"title": starts[i], "lessons": []}
        out.append(part)
    texts = [s if isinstance(s, str) else s[0] for s in L["steps"]]
    first = texts[0]
    title = first.split(". ", 1)[1] if ". " in first else first
    part["lessons"].append({"n": i + 1, "title": title.rstrip(".?!"), "teaser": L["teaser"], "story": texts[1:]})
assert sum(len(p["lessons"]) for p in out) == len(SL.SCHOOL)
with open(os.path.join(HERE, "..", "web", "src", "lessons.json"), "w") as f:
    quiz = [q for L in SL.SCHOOL for (q, _, _) in L["quiz"]]   # questions only, in firmware order (q index)
    json.dump({"quiz": quiz, "parts": out, "ranks": SL.RANKS, "rank_every": 5}, f, indent=1)
print(f"lessons.json: {len(SL.SCHOOL)} lessons in {len(out)} parts")
