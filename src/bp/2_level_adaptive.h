// 2_LEVEL_ADAPTIVE_H: prototypes for 2_level adaptive training bp

#ifndef __2_LEVEL_ADAPTIVE_H__
#define __2_LEVEL_ADAPTIVE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bp/bp.h"

  /*************Interface to Scarab***************/
  void bp_2_level_adaptive_init(void);
  // void bp_2_level_adaptive_timestamp(Op*);
  uns8 bp_2_level_adaptive_pred(Op*);
  uns8 bp_2_level_adaptive_spec_update(Op*);
  void bp_2_level_adaptive_update(Op*);
  // void bp_2_level_adaptive_retire(Op*);
  void bp_2_level_adaptive_recover(Recovery_Info*);
  // uns8 bp_2_level_adaptive_full(uns);

#ifdef __cplusplus
}
#endif


#endif  // __2_LEVEL_ADAPTIVE_H__
