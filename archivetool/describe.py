"""Interactive metadata help from a local LLM: suggest, ask the owner, refine, review.

Used by `archive describe` (a folder before burning, or an existing disc) and
by `archive make --llm`. Results are a "draft": plain JSON that `archive make
--draft` reads, so suggestions can be prepared, edited by hand, and reused.
"""

import json
import os
import sys

from . import bag, catalog, llm, web


def log(msg=""):
    print(msg, file=sys.stderr)


def ask(prompt):
    try:
        return input(prompt).strip()
    except EOFError:
        return ""


def show(suggestion):
    log("")
    log("  Title:       %s" % suggestion["title"])
    log("  Description: %s" % suggestion["description"])
    log("  Subjects:    %s" % ", ".join(suggestion["subjects"]))
    if suggestion.get("folder_tags"):
        log("  Folder tags:")
        for folder, tags in sorted(suggestion["folder_tags"].items()):
            log("    %-40s %s" % (folder + "/", ", ".join(tags)))
    log("")


def conversation(client, inventory_text, folders, rounds=2, max_questions=5):
    """Suggest -> ask the owner each question -> refine. Returns (suggestion, answers)."""
    log("Asking %s at %s ..." % (client.resolve_model(), client.url))
    suggestion = llm.suggest(client, inventory_text, max_questions=max_questions, folders=folders)
    answers = []
    for round_no in range(rounds):
        show(suggestion)
        questions = suggestion["questions"]
        if not questions:
            break
        log("The model has %d question%s. Answer what you can; press Enter to skip one, or type '.' "
            "to stop answering." % (len(questions), "" if len(questions) == 1 else "s"))
        new = []
        for q in questions:
            log("")
            log("Q: " + q)
            a = ask("A: ")
            if a == ".":
                break
            if a:
                new.append((q, a))
        if not new:
            break
        answers += new
        log("")
        log("Refining with your answers ...")
        suggestion = llm.suggest(client, inventory_text, answers=answers, previous=suggestion,
                                 max_questions=max_questions if round_no + 1 < rounds else 0, folders=folders)
    return suggestion, answers


def review(suggestion, answers, current=None):
    """Let the owner accept or edit each field. Returns the draft dict."""
    current = current or {}
    show(suggestion)

    def pick(label, value, old):
        hint = " (current: %s)" % old if old and old != value else ""
        answer = ask("%s: [Enter] accept, '-' keep current, or type a replacement%s\n> " % (label, hint))
        if answer == "-":
            return old
        return answer or value

    title = pick("Title", suggestion["title"], current.get("title"))
    description = pick("Description", suggestion["description"], current.get("description"))
    subjects = pick("Subjects", ", ".join(suggestion["subjects"]), ", ".join(current.get("subjects") or []))
    tags = suggestion.get("folder_tags") or {}
    if tags and ask("Keep the %d folder tags? [Y/n] " % len(tags)).lower().startswith("n"):
        tags = {}
    return {
        "title": title,
        "description": description,
        "subjects": [s.strip().lower() for s in (subjects or "").split(",") if s.strip()],
        "notes": qa_notes(answers),
        "folder_tags": tags,
    }


def qa_notes(answers):
    return ["Q: %s\nA: %s" % qa for qa in answers]


def save_draft(path, draft, agent):
    """Draft JSON: title, description, subjects, notes, folder_tags (+ unanswered questions)."""
    data = dict(draft, agent=agent)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write("\n")


def load_draft(path):
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    return {
        "title": data.get("title") or None,
        "description": data.get("description") or None,
        "subjects": list(data.get("subjects") or []),
        "notes": list(data.get("notes") or []),
        "folder_tags": {k: list(v) for k, v in (data.get("folder_tags") or {}).items()},
        "agent": data.get("agent") or "draft",
    }


# ---------------------------------------------------------------- targets

def folder_entries(src):
    """Sizes and dates without hashing (fast): enough for an inventory."""
    entries = []
    for root, dirs, names in os.walk(src):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for name in sorted(names):
            full = os.path.join(root, name)
            if os.path.isfile(full) and not os.path.islink(full):
                st = os.stat(full)
                entries.append(bag.Entry(os.path.relpath(full, src).replace(os.sep, "/"), st.st_size, st.st_mtime))
    return entries


def disc_entries(home, disc_id):
    """Entries for a disc already in the catalogue, from its listing (paths relative to data/)."""
    import calendar
    import time
    path = home.listing_path(disc_id)
    if not os.path.exists(path):
        raise SystemExit("Error: no file listing for %s at %s" % (disc_id, path))
    entries = []
    for size, mtime, rel in web.read_listing(path):
        try:
            ts = calendar.timegm(time.strptime(mtime, "%Y-%m-%dT%H:%M:%SZ"))
        except ValueError:
            ts = 0
        entries.append(bag.Entry(rel, int(size), ts))
    return entries


def existing_metadata(disc):
    return {"Title": disc.get("Title"), "Description": disc.get("Description"),
            "Subject": disc.get_all("Subject"), "Note": disc.get_all("Note"), "Coverage": disc.get("Coverage")}


def apply_to_disc(home, cat, disc, draft, agent):
    """Write an accepted draft into the home catalogue, with provenance."""
    changed = []
    for field, key in (("Title", "title"), ("Description", "description")):
        if draft.get(key) and draft[key] != disc.get(field):
            disc.set(field, draft[key])
            changed.append(field)
    if draft.get("subjects") and draft["subjects"] != disc.get_all("Subject"):
        disc.fields = [(k, v) for k, v in disc.fields if k != "Subject"]
        insert_at = next((i for i, (k, _) in enumerate(disc.fields) if k in ("Note", "Location", "Rights", "Media")), len(disc.fields))
        disc.fields[insert_at:insert_at] = [("Subject", s) for s in draft["subjects"]]
        changed.append("Subject")
    for note in draft.get("notes") or []:
        disc.add("Note", note)
    if draft.get("notes"):
        changed.append("Note")
    if draft.get("folder_tags"):
        path = home.disc_file("tags", disc.get("Id"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        catalog.write_tags(path, draft["folder_tags"])
        changed.append("folder tags")
    if changed:
        cat.events.append(catalog.new_event(
            disc.get("Id"), "metadata modification", "success", "%s + owner review" % agent,
            "updated %s (suggested by a local LLM, reviewed by the owner)" % ", ".join(changed)))
        home.save(cat)
    return changed


def run(args):
    """`archive describe`."""
    home = catalog.Home(args.home)
    cat = home.load()
    disc = cat.disc(args.target)
    if args.apply:
        # No LLM involved: apply a saved (possibly hand-edited) draft to a disc
        if not disc:
            raise SystemExit("Error: --apply needs a disc id from the catalogue, not %r" % args.target)
        draft = load_draft(args.apply)
        changed = apply_to_disc(home, cat, disc, draft, draft["agent"])
        print("%s: updated %s" % (disc.get("Id"), ", ".join(changed) or "nothing"))
        return 0
    client = llm.Client(args.llm_url, args.llm_model, args.llm_allow_remote)
    if disc:
        entries = disc_entries(home, disc.get("Id"))
        text_root = os.path.join(args.disc_root, "data") if args.disc_root else None
        inv = llm.inventory(entries, disc.get("Id"), existing_metadata(disc), text_root=text_root)
        current = {"title": disc.get("Title"), "description": disc.get("Description"), "subjects": disc.get_all("Subject")}
    elif os.path.isdir(args.target):
        src = os.path.abspath(args.target)
        entries = folder_entries(src)
        inv = llm.inventory(entries, os.path.basename(src), text_root=src)
        current = {}
    else:
        raise SystemExit("Error: %s is neither a disc id in the catalogue nor a folder" % args.target)

    if args.show_inventory:
        print(inv)
        return 0
    interactive = sys.stdin.isatty()
    try:
        if interactive:
            suggestion, answers = conversation(client, inv, llm.folders_of(entries), args.rounds, args.questions)
        else:
            # No terminal: one suggestion; the questions are kept in the draft for later
            suggestion = llm.suggest(client, inv, max_questions=args.questions, folders=llm.folders_of(entries))
            answers = []
    except llm.LLMError as err:
        raise SystemExit("Error: %s" % err)

    if interactive:
        draft = review(suggestion, answers, current)
    else:
        draft = dict(suggestion, notes=qa_notes(answers))
        if not args.save:
            print(json.dumps(draft, ensure_ascii=False, indent=2))
            return 0
    if args.save:
        save_draft(args.save, draft, client.agent)
        log("Draft saved to %s (use: archive make --draft %s ...)" % (args.save, args.save))
    if disc and sys.stdin.isatty():
        if ask("Apply to %s in the home catalogue? [y/N] " % disc.get("Id")).lower().startswith("y"):
            changed = apply_to_disc(home, cat, disc, draft, client.agent)
            log("Updated: %s" % (", ".join(changed) or "nothing"))
        else:
            log("Not applied.")
    return 0


def make_draft(args, src, entries, interactive):
    """For `archive make --llm`: run the conversation and review before the usual prompts."""
    if not interactive:
        raise SystemExit("Error: --llm needs an interactive terminal (or prepare a draft with "
                         "'archive describe <folder> --save draft.json' and pass --draft draft.json)")
    client = llm.Client(args.llm_url, args.llm_model, args.llm_allow_remote)
    inv = llm.inventory(entries, os.path.basename(src), text_root=src)
    try:
        suggestion, answers = conversation(client, inv, llm.folders_of(entries), args.llm_rounds, 5)
    except llm.LLMError as err:
        raise SystemExit("Error: %s" % err)
    draft = review(suggestion, answers)
    draft["agent"] = client.agent
    return draft

