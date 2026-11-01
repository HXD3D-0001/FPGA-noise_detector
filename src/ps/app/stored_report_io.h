#ifndef LFN_STORED_REPORT_IO_H
#define LFN_STORED_REPORT_IO_H
#include "saved_report.h"
/* Serialized read-only caller, with the recording volume already unmounted.
 * Scratch is static; never call from capture or concurrently with recording. */
bool lfn_stored_report_read(LfnSavedReport *,const char *,bool (*owner)(void *),void *);
#endif
