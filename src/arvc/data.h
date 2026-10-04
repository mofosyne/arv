/* Files shared with the Python arv (src/arv/), compiled in: see dev-tools/embed.c and data.c. */
#ifndef ARV_DATA_H
#define ARV_DATA_H

extern const char DATA_DESCRIPTORS[];   /* descriptors.rec: the catalogue's record descriptors */
extern const char DATA_README[];        /* readme.txt: README.txt on every disc (str.format style) */
extern const char DATA_DEFAULT_SETS[];  /* default_sets.rec: the starting set vocabulary */
extern const char DATA_INDEX_CSS[];     /* index.css: the style of index.html on every disc */
extern const char DATA_DEFAULT_TAGS[];  /* default_tags.rec: the starting tag vocabulary */

#endif
