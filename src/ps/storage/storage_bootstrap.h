#ifndef LFN_STORAGE_BOOTSTRAP_H
#define LFN_STORAGE_BOOTSTRAP_H
#include "storage_family.h"
#include "storage_service.h"
enum { STORAGE_BOOTSTRAP_DEFINITIONS=16 };
typedef struct {RecordIdentity identity;size_t bytes;uint8_t payload[STORAGE_METADATA_BYTES];} StorageDefinitionCopy;
typedef struct {unsigned count;bool valid;uint64_t session_id;StorageDefinitionCopy records[STORAGE_BOOTSTRAP_DEFINITIONS];} StorageBootstrapPlan;
typedef enum { BOOTSTRAP_SUBMIT,BOOTSTRAP_WAIT,BOOTSTRAP_DONE,BOOTSTRAP_FAILED } StorageBootstrapState;
typedef struct {const StorageBootstrapPlan *plan;StorageService *service;unsigned cursor;StorageTicket ticket;StorageBootstrapState state;} StorageBootstrapWriter;
/* Build deep copies ONLY from this file's exact durable definition entries.
 * Caller first seals/drains owner. Dependency closure includes earlier capture
 * revision/calibration map's own config/capture, sorted by prior record order.
 * No pointer to old registry survives; cap is explicit, never silent omission. */
StorageResult storage_bootstrap_plan(StorageBootstrapPlan *,StorageService *,const StorageFamily *,uint64_t file_id,uint32_t config_id,uint64_t capture_id);
bool storage_bootstrap_init(StorageBootstrapWriter *,const StorageBootstrapPlan *,StorageService *fresh_empty_service);
/* No backend IO; sole owner separately services queue/IO fairly. Each definition
 * must be SYNCED before the next. Producers stay quiescent until DONE. */
void storage_bootstrap_step(StorageBootstrapWriter *);
#endif
