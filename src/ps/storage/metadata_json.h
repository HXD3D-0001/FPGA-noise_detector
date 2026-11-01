#ifndef LFN_METADATA_JSON_H
#define LFN_METADATA_JSON_H
#include "../../shared/record_metadata.h"
#include <stddef.h>
enum { MJ_OBJECT,MJ_ARRAY,MJ_STRING,MJ_NUMBER,MJ_LITERAL };
typedef struct {uint16_t start,end,next,count;uint8_t type;} MetadataJsonToken;
typedef struct {const uint8_t *bytes;size_t length,position;unsigned count;MetadataJsonToken token[STORAGE_METADATA_TOKENS];} MetadataJson;
bool metadata_json_parse(MetadataJson *,const uint8_t *,size_t);
int metadata_json_field(const MetadataJson *,int,const char *);
int metadata_json_index(const MetadataJson *,int,unsigned);
bool metadata_json_string(const MetadataJson *,int,char *,size_t);
bool metadata_json_is(const MetadataJson *,int,const char *);
bool metadata_json_u64(const MetadataJson *,int,uint64_t *);
bool metadata_json_i64(const MetadataJson *,int,int64_t *);
bool metadata_json_u32(const MetadataJson *,int,uint32_t *);
bool metadata_json_double(const MetadataJson *,int,double *);
bool metadata_json_bool(const MetadataJson *,int,bool *);
#endif
