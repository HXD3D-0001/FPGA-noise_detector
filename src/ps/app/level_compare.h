#ifndef LFN_LEVEL_COMPARE_H
#define LFN_LEVEL_COMPARE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../time/time_map.h"

enum { LFN_LEVEL_BANDS_MAX=32 };
typedef struct {
  double low_hz,high_hz;
  double power_linear; /* linear mean power, never dB */
  bool below_floor;    /* caller resolved BELOW_FLOOR/UNRESOLVED */
} LfnBandLevel;

/* Direct level comparison is allowed only when point, pose, gain, mode and
 * algorithm match; calibration must be the same valid configuration. */
typedef struct {
  uint64_t measurement_point_id,pose_id;
  uint32_t gain_setting_id,config_hash,algorithm_hash;
  bool calibrated;
  uint64_t calibration_id;
} LfnLevelMatchKey;

typedef struct {
  LfnLevelMatchKey key;
  double floor_power;
  uint64_t duration_ns,valid_ns,expected_ns;
  uint64_t dropped_frames,clipped_samples;
  unsigned band_count;
  LfnBandLevel bands[LFN_LEVEL_BANDS_MAX];
} LfnLevelSet;

typedef enum { LFN_COMPARE_OK,LFN_COMPARE_INCOMPATIBLE,LFN_COMPARE_INVALID } LfnCompareStatus;
enum {
  LFN_COMPARE_MISMATCH_POINT=1u<<0,LFN_COMPARE_MISMATCH_POSE=1u<<1,
  LFN_COMPARE_MISMATCH_GAIN=1u<<2,LFN_COMPARE_MISMATCH_CONFIG=1u<<3,
  LFN_COMPARE_MISMATCH_ALGORITHM=1u<<4,LFN_COMPARE_MISMATCH_CALIBRATION=1u<<5,
  LFN_COMPARE_MISMATCH_BANDS=1u<<6
};
typedef struct {
  bool before_null,after_null,delta_valid,below_floor;
  double before_db,after_db,delta_db;
} LfnBandComparison;

typedef struct {
  LfnCompareStatus status;
  unsigned mismatch_mask;
  bool absolute_allowed;
  unsigned band_count;
  LfnBandComparison bands[LFN_LEVEL_BANDS_MAX];
} LfnCompareResult;

bool lfn_level_compare(const LfnLevelSet *before,const LfnLevelSet *after,LfnCompareResult *);
#endif
