// two_level_adaptive.cc
// not added into bp.h, bp.cc, bp_table.def, ect. pending a working implementation

#include "two_level_adaptive.h"
#include <vector>
#include <cmath>
using math::pow;

extern "C" {
  #include "bp/bp.param.h"
  #include "core.param.h"
  #include "globals/assert.h"
  #include "statistics.h"
}

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_BP_DIR, ##args)    // for debugging purposes

namespace {
  
  struct history_register_table {      // use HHRT for now (though performance might be ... suboptimal)
    Hash_Table HHRT;                // HHRT: hash history register table (hashtable-based HRT) 

    // init_hashtable(): Initialize address table
    void init_hashtable (uns hhrt_size, uns hrt_entry_size) {
      init_hash_table(&HHRT, "hash_history_register_table", hhrt_size, hrt_entry_size);     
      Flag init_hash_history_register_table;                 // NEW: must initialize all entries of set_entries_occupied_table to 0
      int* set_table_entry;                                  // (This is to avoid junk values)

      for(int i = 0; i < hhrt; i++) {   
	set_table_entry = hash_table_access_create(&HHRT, i, &init_hash_history_register_table);
	*set_table_entry = 0u;
      }
    }

    int get_HR_content (Addr branch_addr) {    
      Flag HR_content_flag;                              // also handles new entry initialization
      uns history_bits* = hash_table_access_create(&HHRT, branch_addr, &HR_content_flag);   // how to handle collisions? Do we need to?
      return *history_bits;
    }

    void update_HR_content (Addr branch_addr, uns branch_outcome) {
      Flag HR_content_flag;
      uns current_HR_content = hash_table_access(&HHRT, branch_addr, &HR_content_flag);  
      uns new_HR_content = (current_HR_content >> 1u) + branch_outcome;
      hash_table_access_replace(&HHRT, branch_addr, new_HR_content);
    }
  };     // end struct history_register_table

  struct branch_history_pattern_table {
    uns8 PT = uns8[pow(2, HRT_entry_size) ]

    uns8 get_prediction (int hr_content) {
      return PT[hr_content];
    }

    void change_SC (uns hr_content, uns current_Sc, int automata_to_use) {
      uns8 last_outcome = hr_content[0];    // yes, this is horribly incorrect, change to use proper bit operations
      uns8 SC_plus_one = 0u;
      
      switch (automata_to_use) {
      case 0: SC_plus_one = LT_Last_Time(last_outcome);
	break;
      default: SC_plus_one = LT_Last_Time(last_outcome);
	break;
      }
      PT[hr_content] = SC_plus_one;
    }
    
  };     // end struct branch_history_pattern_table 

  // Automata (choose 1 only)
  uns8 LT_Last_Time (uns8 actual_last_branch_outcome) {
    return actual_last_branch_outcome;
  }

  history_register_table HRT;
  branch_history_pattern_table BHPT;
}  // end of namespace

void bp_two_level_adaptive_init() {
  2_level_adaptive_state_all_cores.resize(NUM_CORES);
  uns HHRT_size = 512;                                   // We'll start with the most common sizes used in the paper's tests
  uns HRT_entry_size = 12;
  
  //for(auto& two_level_adaptive_state : two_level_adaptive_state_all_cores) {      // might not need this 
  HRT.init_hashtable(HHRT_size, HRT_entry_size);              // this might not work, change accordingly
  //}
  
}

// Timestamping and recovery will be implemented ... later 
//void bp_two_level_adaptive_timestamp(Op* op) {}
//void bp_two_level_adaptive_recover(Recovery_Info* info) {}

uns8 bp_two_level_adaptive_pred(Op* op) {
  const uns   proc_id      = op->proc_id;                           // process ID      
  const auto& bimodal_state = bimodal_state_all_cores.at(proc_id);
  const Addr  addr      = op->oracle_info.pred_addr;                // instruction address
  const uns32 hist      = op->oracle_info.pred_global_hist;         // prediction history
  // const uns32 pht_index = get_pht_index(addr, hist);
  const uns HR_bits = HRT.get_HR_content(addr);                         
  //const uns8  pht_entry = bimodal_state.pht[pht_index];
  const uns8 prediction_bit = BHPT.get_pattern_history_bits(HR_bits);

  // DEBUG(proc_id, "Predicting with bimodal for  op_num:%s  index:%d\n", unsstr64(op->op_num), pht_index);
  // DEBUG(proc_id, "Predicting  addr:%s  pht:%u  pred:%d  dir:%d\n", hexstr64s(addr), pht_index, pred, op->oracle_info.dir);

  return prediction_bit;
}

// Speculative update is basically the same as update, but it is also called
// for branches that are being executed speculatively. I.e., if you predict
// taken and the branch is not resolved yet, then another branch appears and
// is resolved before the first one, the speculative update is called for the
// second branch (since it was resolved while being executed speculatively).

// The paper calls for speculative branches to be predicted as taken, so that
// will be done here.

uns8 bp_two_level_adaptive_spec_update(Op* op) {
  const uns8 predict_taken = 1u;
  return predict_taken;
}

void bp_two_level_adaptive_update(Op* op) {
  if(op->table_info->cf_type != CF_CBR) {    // If op isn't conditional branch, don't interact with gshare.
    return;
  }

  const uns   proc_id      = op->proc_id;                           // process ID
  auto&       bimodal_state = bimodal_state_all_cores.at(proc_id);
  const Addr  addr         = op->oracle_info.pred_addr;             // instruction address
  const uns32 hist         = op->oracle_info.pred_global_hist;      // prediction history
  //const uns32 pht_index    = get_pht_index(addr, hist);
  //const uns8  pht_entry    = bimodal_state.pht[pht_index];

  DEBUG(proc_id, "Writing bimodal PHT for  op_num:%s  index:%d  dir:%d\n",
        unsstr64(op->op_num), pht_index, op->oracle_info.dir);

  HRT.update_HR_content(addr, hist);      // FIGURE OUT HOW TO GET LATEST PREDICTION
  const uns HR_bits = HRT.get_HR_content(addr);  
  BHPT.change_SC(history_register_table.get_HR_content(addr), get_pattern_history_bits(HR_bits), 0);
  
  /*  if(op->oracle_info.dir) {
    bimodal_state.pht[pht_index] = SAT_INC(pht_entry, N_BIT_MASK(PHT_CTR_BITS));
  } else {
    bimodal_state.pht[pht_index] = SAT_DEC(pht_entry, 0);
    } */

  DEBUG(proc_id, "Updating addr:%s  pht:%u  ent:%u  dir:%d\n", hexstr64s(addr),
        pht_index, bimodal_state.pht[pht_index], op->oracle_info.dir);
}

// Retire and full will be implemented ... later 
//void bp_two_level_adaptive_retire(Op* op) {}
//uns8 bp_two_level_adaptive_full(uns proc_id) { return 0; }


