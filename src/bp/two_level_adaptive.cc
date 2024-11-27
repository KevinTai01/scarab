// two_level_adaptive.cc
// not added into bp.h, bp.cc, bp_table.def, ect. pending a working
// implementation

#include "two_level_adaptive.h"
#include <cmath>
#include <vector>

extern "C" {
#include "bp/bp.param.h"
#include "core.param.h"
#include "globals/assert.h"
#include "statistics.h"
}

#define DEBUG(proc_id, args...) \
  _DEBUG(proc_id, DEBUG_BP_DIR, ##args)  // for debugging purposes

enum Automata {
  LAST_TIME,  // Predict the same as the last outcome
  A1,  // Predict not taken iff both last outcomes were not taken
  A2,  // ?
  A3,  // ?
  A4,  // ?
};

// Adjustable parameters
// The size of each entry in the history register table
#define HRT_entry_size 12

// The maximum number of entries in the history register table
// TODO: This does nothing. We will need to write some way for the entries over
//  the size limit to be evicted. I think that the paper uses LRU, which would
//  be relatively simple to implement (using a queue representing the age
//  of different instructions).
#define HRT_size 512

// The number of buckets in the history register table. This is not to be
// confused with the number of entries in the pattern table.
#define HRT_buckets 4096

// The automata to use for the pattern table
#define PT_automata LAST_TIME

// Derived parameters
// A bit mask for n bits should be 2^n - 1
// TODO: Double check if this is correct
#define HRT_entry_mask ((1 << HRT_entry_size) - 1)
// We have 2^HR_entry_size entries in the pattern table
#define PT_entries (1 << HRT_entry_size)

namespace {
struct TLA_State {
  Hash_Table hr_table;
  std::vector<uns8> pattern_table;
};

std::vector<TLA_State> tla_state_all_cores;

// ---------- History Register Table ----------
// Note: The current implementation is based on a hash table. We can probably
// do a similar thing to the automata and add a wrapper to allow for a cache
// implementation.

// Initialize the underlying hash table for the history register table.
// In regard to setting all entries to 0, this can be done at the time of
// first update. Before that, the hash table accesses return NULL if the entry
// is not found, which the wrapper function returns as 0.
void hrt_init(TLA_State* tla_state) {
  init_hash_table(&tla_state->hr_table,
    "history_register_table", HRT_buckets, sizeof(uns64));
}

// Get the history register content for the given address
// how to handle collisions? Do we need to? --> Ignored for now. We'll see if
// it's necessary
uns64 hrt_get(const TLA_State* tla_state, const Addr addr) {
  void* data = hash_table_access(&tla_state->hr_table, addr);

  if (!data) {
    return 0;  // Return 0 if the entry is not found
  }

  const uns64* history = static_cast<uns64*>(data);

  // Return the last HRT_entry_size bits of the history register content
  // This removes any history older than HRT_entry_size, effectively working
  // as though the underlying data structure has HRT_entry_size bits.
  return *history & HRT_entry_mask;
}

// Update the history register content for the given address with the branch
// outcome. The other implementation used the hash_table_access_replace function
// which might be a better choice here as well, though I think that this
// also works.
void hrt_update(TLA_State* tla_state, const Addr addr, const uns8 outcome) {
  Flag found;
  auto* history = static_cast<uns64*>(
    hash_table_access_create(&tla_state->hr_table, addr, &found));

  if (!found) {
    *history = 0;  // Initialize new entries to 0
  }

  // Left shift and insert the outcome bit
  *history = (*history << 1) | (outcome & 0x1);
}

// ---------- Pattern Table ----------

// Initialize the pattern table
void pt_init(TLA_State* tla_state) {
  tla_state->pattern_table.resize(PT_entries, 0);
}

// Get the n-th entry in the pattern table where n is hr_content
uns8 pt_get(const TLA_State* tla_state, const uns64 hr_content) {
  return tla_state->pattern_table.at(hr_content);
}

// Update the n-th entry in the pattern table where n is hr_content
void pt_update(TLA_State* tla_state, const uns64 hr_content, const uns8 new_state) {
  tla_state->pattern_table.at(hr_content) = new_state;
}

// ---------- Automata ----------

// Last time automata - predict the same as the last outcome
// The update function for the automata gets the current state and the latest
// branch result and returns the new state after taking the appropriate
// transition. Do not call directly, use automata_update instead.
uns8 automata_last_time_update(const uns8 state, const uns8 latest_result) {
  return latest_result;
}

// The get function for the automata gets the current state and returns the
// prediction based on the state, without changing the state. Do not call
// directly, use automata_get instead.
uns8 automata_last_time_get(const uns8 state) {
  return state & 0x1;
}

// TODO: Check that this makes sense and actually works
// TODO: Later: Implement the other automata

// Call the currently selected automata with the current state and the latest
// branch result to get the new state after taking the appropriate transition.
// This is basically just a wrapper around the automata-specific functions.
// The state should be taken from the appropriate PT entry and the resulting
// state should be stored back into the same PT entry.
// Note: This is basically the change_SC function from the earlier version of
// this code.
uns8 automata_update(const uns8 state, const uns8 latest_result) {
  switch (PT_automata) {
    case LAST_TIME:
      return automata_last_time_update(state, latest_result);
    // TODO: Fill in the other automata
    default:
      return 0;
  }
}

// Call the currently selected automata with the current state to get the
// prediction based on the state, without changing the state.
// Again, this just calls the automata-specific functions. This is used to
// get the prediction for the branch.
uns8 automata_get(const uns8 state) {
  switch (PT_automata) {
    case LAST_TIME:
      return automata_last_time_get(state);
    // TODO: Fill in the other automata
    default:
      return 0;
  }
}

} // namespace

void bp_two_level_adaptive_init() {
  tla_state_all_cores.resize(NUM_CORES);

  // might not need this --> However, it doesn't really hurt to have it.
  for(auto& tla_state : tla_state_all_cores) {
    hrt_init(&tla_state);
    pt_init(&tla_state);
  }
}

uns8 bp_two_level_adaptive_pred(Op* op) {
  const uns   proc_id       = op->proc_id;  // process ID
  const auto& tla_state     = tla_state_all_cores.at(proc_id);
  const Addr  addr          = op->oracle_info.pred_addr;  // instruction address
  // Looking here, we have what appears to be the prediction history for the
  // branch. We actually might not even need to have a history register table,
  // since we could just index into the pattern table with the history (well,
  // at least the last few bits of it). That said, as mentioned above we will
  // need to write some sort of eviction mechanism for HRT entries over the
  // size limit.
  const uns32 hist          = op->oracle_info.pred_global_hist; // prediction history

  const uns64 history = hrt_get(&tla_state, addr);
  const uns8 automata_state = pt_get(&tla_state, history);
  const uns8 prediction_bit = automata_get(automata_state);

  return prediction_bit;

  // const uns32 pht_index = get_pht_index(addr, hist);
  // const uns HR_bits = HRT.get_HR_content(addr);
  // const uns8  pht_entry = bimodal_state.pht[pht_index];
  // const uns8 prediction_bit = BHPT.get_pattern_history_bits(HR_bits);

  // DEBUG(proc_id, "Predicting with bimodal for  op_num:%s  index:%d\n",
  // unsstr64(op->op_num), pht_index); DEBUG(proc_id, "Predicting  addr:%s
  // pht:%u  pred:%d  dir:%d\n", hexstr64s(addr), pht_index, pred,
  // op->oracle_info.dir);

  // TODO: We should add debugging functions like they have fore gshare.
}

// Note: All the functions must be present in the .cc file, even if they are
// doing nothing.
// Timestamping and recovery will be implemented ... later
void bp_two_level_adaptive_timestamp(Op* op) {}
void bp_two_level_adaptive_recover(Recovery_Info* info) {}
// Speculative update is basically the same as update, but it is also called
// for branches that are being executed speculatively. I.e., if you predict
// taken and the branch is not resolved yet, then another branch appears and
// is resolved before the first one, the speculative update is called for the
// second branch (since it was resolved while being executed speculatively).
// Note: This is NOT called to predict the branch, but to update the predictor
void bp_two_level_adaptive_spec_update(Op* op) {}
// Retire and full will be implemented ... later
void bp_two_level_adaptive_retire(Op* op) {}
uns8 bp_two_level_adaptive_full(uns proc_id) { return 0; }

void bp_two_level_adaptive_update(Op* op) {
  // Don't interact with instructions which are not conditional branches.
  if(op->table_info->cf_type != CF_CBR) {
    return;
  }

  const uns   proc_id       = op->proc_id;  // process ID
  auto&       tla_state     = tla_state_all_cores.at(proc_id);
  const Addr  addr          = op->oracle_info.pred_addr;  // instruction address
  const uns32 hist          = op->oracle_info.pred_global_hist;  // prediction history
  const uns8  outcome       = op->oracle_info.dir;  // branch outcome

  // TODO: Check if this is correct. I didn't really check against the paper.
  const uns64 history = hrt_get(&tla_state, addr);
  const uns8 automata_state = pt_get(&tla_state, history);
  const uns8 new_automata_state = automata_update(automata_state, outcome);
  pt_update(&tla_state, history, new_automata_state);
  hrt_update(&tla_state, addr, outcome);

  // const uns32 pht_index    = get_pht_index(addr, hist);
  // const uns8  pht_entry    = bimodal_state.pht[pht_index];
  //
  // DEBUG(proc_id, "Writing bimodal PHT for  op_num:%s  index:%d  dir:%d\n",
  //       unsstr64(op->op_num), pht_index, op->oracle_info.dir);

  // HRT.update_HR_content(addr, hist);  // FIGURE OUT HOW TO GET LATEST PREDICTION
  // const uns HR_bits = HRT.get_HR_content(addr);
  // BHPT.change_SC(history_register_table.get_HR_content(addr),
  //                get_pattern_history_bits(HR_bits), 0);

  /*  if(op->oracle_info.dir) {
    bimodal_state.pht[pht_index] = SAT_INC(pht_entry, N_BIT_MASK(PHT_CTR_BITS));
  } else {
    bimodal_state.pht[pht_index] = SAT_DEC(pht_entry, 0);
    } */

  // DEBUG(proc_id, "Updating addr:%s  pht:%u  ent:%u  dir:%d\n", hexstr64s(addr),
  //       pht_index, bimodal_state.pht[pht_index], op->oracle_info.dir);
}
