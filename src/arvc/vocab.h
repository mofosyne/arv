/* The set vocabulary (src/arv/sets.py; tests/fixtures/vocab-*.tsv and match-rules.tsv are the
 * contract): codes in a directed acyclic graph of Parents, with Aliases and Match rules. */
#ifndef ARV_VOCAB_H
#define ARV_VOCAB_H

#include <stddef.h>

typedef struct {
    char **v;
    size_t n;
} strlist;

typedef struct {
    char *code, *name, *scope_note;
    int order;              /* Order, or 50: sorts siblings for display */
    strlist parents, aliases, matches;
} vset;

typedef struct {
    vset *e;                /* in file order */
    size_t n;
    strlist alias_keys;     /* word(alias), first seen first */
    strlist alias_codes;    /* the code each alias key names */
    char *path;
} vocab;

void strlist_add(strlist *l, const char *s);
void strlist_free(strlist *l);
int strlist_has(const strlist *l, const char *s);

/* Loads a vocabulary from `path`, or from `text` when path is NULL. Returns 0, or -1 with a
 * message in err. */
int vocab_load(vocab *v, const char *path, const char *text, char *err, size_t errlen);
void vocab_free(vocab *v);

const vset *vocab_get(const vocab *v, const char *code);
/* a code for a typed code or alias ("trips" -> TRIP), or NULL; codes win over aliases */
const char *vocab_resolve(const vocab *v, const char *text);
/* the best code for a folder-derived word ("Photos" -> PHOTO), or NULL; aliases win */
const char *vocab_guess(const vocab *v, const char *text);
/* every path from a top-level entry down to code, sorted ("MEMORIES/PHOTO/SCAN", ...) */
void vocab_paths(const vocab *v, const char *code, strlist *out);
/* does the code lie above `code` on any path? */
int vocab_is_ancestor(const vocab *v, const char *above, const char *code);
/* codes like the given one, for "did you mean" */
void vocab_near(const vocab *v, const char *code, strlist *out);
/* does a Match glob apply to a relative path? */
int vocab_path_matches(const char *pattern, const char *path);
/* codes whose Match rules claim at least a tenth of the paths, most first, most specific only */
void vocab_rule_suggestions(const vocab *v, char *const *paths, size_t n, strlist *out);
/* comparison form of a word: ASCII capitals and digits only (out has room for strlen(text)+1) */
void vocab_word(const char *text, char *out);

#endif
