#ifndef M8_CARD_FOLDER_H
#define M8_CARD_FOLDER_H
#include <stdint.h>
#include "m8rt.h"
#include "card.h"
/* Browsable folder mirror of the card (Windows). See card_folder.c. */
char *m8_card_folder_default(void);                /* Documents\M8 Card; free() it */
/* Before the runtime loads the card: apply folder edits and dropped folders
 * (destination per folder, "" for the card root), rebuilding the FAT32 card
 * when needed, then size the card to what is on it. */
int m8_card_folder_prepare(m8_card *card, const char *folder,
                           const char *const *drops, const char *const *destinations, unsigned drop_count);
/* After the card is saved, with the runtime still loaded. */
void m8_card_folder_finish(m8_card *card, m8rt *rt, const char *folder);
/* After the M8 crashed: copy the session's card (if it changed) to a
 * separate "rescued" folder; the main card and folder are not touched. */
/* Returns 1 with the folder in where when work was rescued. */
int m8_card_folder_rescue(m8_card *card, m8rt *rt, const char *folder, char *where, size_t capacity);
#endif
