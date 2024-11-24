#include "bimodal.h"

#include <vector>

extern "C" {
#include "bp/bp.param.h"
#include "core.param.h"
#include "globals/assert.h"
#include "statistics.h"
}

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_BP_DIR, ##args)    // for debugging purposes
Hash_Table hash_history_register_table;

namespace {

  struct 2_level_adaptive_State {
    std::vector<uns8> pht;
  };

  std::vector<2_level_adaptive_State> bimodal_state_all_cores;

  uns32 get_pht_index(const Addr addr, const uns32 hist) {
    const uns32 cooked_hist = hist >> (32 - HIST_LENGTH);
    const uns32 cooked_addr = (addr >> 2) & N_BIT_MASK(HIST_LENGTH);
    return cooked_hist ^ cooked_addr;
  }
}  // end of namespace

void bp_2_level_adaptive_init() {
  2_level_adaptive_state_all_cores.resize(NUM_CORES);
  
  Flag init_hash_history_register_table;                 // NEW: must initialize all entries of set_entries_occupied_table to 0
  int* set_table_entry;                                  // (This is to avoid junk values)
  HHRT_size = 512
  
  for(auto& 2_level_adaptive__state : 2_level_adaptive_state_all_cores) {
    init_hash_table(&hash_history_register_table, "HHRT", HHRT_size, sizeof(int));     // NEW: Initialize address table
    //2_level_adaptive_state.pht.resize(1 << HIST_LENGTH, PHT_INIT_VALUE);

    for(int i = 0; i < num_of_sets; i++) {   
      set_table_entry = hash_table_access_create(&hash_history_register_table, i, &init_hash_history_register_table);
      *set_table_entry = 0;
    }
    
  }
}

// Timestamping and recovery will be implemented ... later 
//void bp_2_level_adaptive_timestamp(Op* op) {}
//void bp_2_level_adaptive_recover(Recovery_Info* info) {}

uns8 bp_2_level_adaptive_pred(Op* op) {
  const uns   proc_id      = op->proc_id;
  const auto& bimodal_state = bimodal_state_all_cores.at(proc_id);
  const Addr  addr      = op->oracle_info.pred_addr;
  const uns32 hist      = op->oracle_info.pred_global_hist;
  const uns32 pht_index = get_pht_index(addr, hist);
  const uns8  pht_entry = bimodal_state.pht[pht_index];
  // uns8  prediction_bit_zc      = 0
  prediction_bit_zc = pht_entry >> (PHT_CTR_BITS - 1) & 0x1;

  // DEBUG(proc_id, "Predicting with bimodal for  op_num:%s  index:%d\n", unsstr64(op->op_num), pht_index);
  // DEBUG(proc_id, "Predicting  addr:%s  pht:%u  pred:%d  dir:%d\n", hexstr64s(addr), pht_index, pred, op->oracle_info.dir);

  return prediction_bit_zc;
}

// Speculative update is basically the same as update, but it is also called
// for branches that are being executed speculatively. I.e., if you predict
// taken and the branch is not resolved yet, then another branch appears and
// is resolved before the first one, the speculative update is called for the
// second branch (since it was resolved while being executed speculatively).

// The paper calls for speculative branches to be predicted as taken, so that
// will be done here.

uns8 bp_2_level_adaptive_spec_update(Op* op) {
  const uns8 predict_taken = 1;
  return predict_taken;
}

void bp_2_level_adaptive_update(Op* op) {
  if(op->table_info->cf_type != CF_CBR) {    // If op isn't conditional branch, don't interact with gshare.
    return;
  }

  const uns   proc_id      = op->proc_id;
  auto&       bimodal_state = bimodal_state_all_cores.at(proc_id);
  const Addr  addr         = op->oracle_info.pred_addr;
  const uns32 hist         = op->oracle_info.pred_global_hist;
  const uns32 pht_index    = get_pht_index(addr, hist);
  const uns8  pht_entry    = bimodal_state.pht[pht_index];

  DEBUG(proc_id, "Writing bimodal PHT for  op_num:%s  index:%d  dir:%d\n",
        unsstr64(op->op_num), pht_index, op->oracle_info.dir);

  if(op->oracle_info.dir) {
    bimodal_state.pht[pht_index] = SAT_INC(pht_entry, N_BIT_MASK(PHT_CTR_BITS));
  } else {
    bimodal_state.pht[pht_index] = SAT_DEC(pht_entry, 0);
  }

  DEBUG(proc_id, "Updating addr:%s  pht:%u  ent:%u  dir:%d\n", hexstr64s(addr),
        pht_index, bimodal_state.pht[pht_index], op->oracle_info.dir);
}

// Retire and full will be implemented ... later 
//void bp_2_level_adaptive_retire(Op* op) {}
//uns8 bp_2_level_adaptive_full(uns proc_id) { return 0; }


