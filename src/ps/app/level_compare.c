#include "level_compare.h"
#include <math.h>
#include <string.h>

static bool finite_nonnegative(double v){return isfinite(v)&&v>=0.0;}
static bool set_valid(const LfnLevelSet *s){
  if(!s||s->band_count>LFN_LEVEL_BANDS_MAX||!finite_nonnegative(s->floor_power))return false;
  for(unsigned i=0;i<s->band_count;i++){
    const LfnBandLevel *b=&s->bands[i];
    if(!isfinite(b->low_hz)||!isfinite(b->high_hz)||b->high_hz<b->low_hz||
       !finite_nonnegative(b->power_linear))return false;
  }
  return true;
}
static bool same_band(const LfnBandLevel *a,const LfnBandLevel *b){
  double low=fabs(a->low_hz)*1e-9,high=fabs(a->high_hz)*1e-9;
  if(low<1e-12)low=1e-12;
  if(high<1e-12)high=1e-12;
  return fabs(a->low_hz-b->low_hz)<=low&&fabs(a->high_hz-b->high_hz)<=high;
}
static double db(double power){return 10.0*log10(power);}
bool lfn_level_compare(const LfnLevelSet *before,const LfnLevelSet *after,LfnCompareResult *out){
  if(!out)return false;
  memset(out,0,sizeof(*out));
  if(!set_valid(before)||!set_valid(after)){out->status=LFN_COMPARE_INVALID;return false;}
  const LfnLevelMatchKey *a=&before->key,*b=&after->key;
  unsigned mask=0;
  if(a->measurement_point_id!=b->measurement_point_id)mask|=LFN_COMPARE_MISMATCH_POINT;
  if(a->pose_id!=b->pose_id)mask|=LFN_COMPARE_MISMATCH_POSE;
  if(a->gain_setting_id!=b->gain_setting_id)mask|=LFN_COMPARE_MISMATCH_GAIN;
  if(a->config_hash!=b->config_hash)mask|=LFN_COMPARE_MISMATCH_CONFIG;
  if(a->algorithm_hash!=b->algorithm_hash)mask|=LFN_COMPARE_MISMATCH_ALGORITHM;
  if(a->calibrated!=b->calibrated||(a->calibrated&&a->calibration_id!=b->calibration_id))
    mask|=LFN_COMPARE_MISMATCH_CALIBRATION;
  if(before->band_count!=after->band_count){mask|=LFN_COMPARE_MISMATCH_BANDS;}
  else for(unsigned i=0;i<before->band_count;i++)
    if(!same_band(&before->bands[i],&after->bands[i])){mask|=LFN_COMPARE_MISMATCH_BANDS;break;}
  if(mask){out->status=LFN_COMPARE_INCOMPATIBLE;out->mismatch_mask=mask;return true;}
  out->status=LFN_COMPARE_OK;
  out->absolute_allowed=a->calibrated&&b->calibrated&&a->calibration_id!=0;
  out->band_count=before->band_count;
  for(unsigned i=0;i<before->band_count;i++){
    const LfnBandLevel *x=&before->bands[i],*y=&after->bands[i];
    LfnBandComparison *c=&out->bands[i];
    c->below_floor=x->below_floor||y->below_floor||
      (before->floor_power>0&&x->power_linear<before->floor_power)||
      (after->floor_power>0&&y->power_linear<after->floor_power)||
      x->power_linear<=0.0||y->power_linear<=0.0;
    if(x->power_linear>0.0&&!c->below_floor)c->before_db=db(x->power_linear);else c->before_null=true;
    if(y->power_linear>0.0&&!c->below_floor)c->after_db=db(y->power_linear);else c->after_null=true;
    c->delta_valid=!c->below_floor&&!c->before_null&&!c->after_null;
    if(c->delta_valid)c->delta_db=c->after_db-c->before_db;
    if(c->below_floor){c->before_null=true;c->after_null=true;}
  }
  return true;
}
