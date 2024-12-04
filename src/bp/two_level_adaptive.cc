// two_level_adaptive.cc in scarab repo
// now added into bp.h, bp.cc, bp_table.def, ect.

#include "two_level_adaptive.h"
#include <cmath>
#include <map>
#include <vector>
#include <list>

extern "C" {
#include "bp/bp.param.h"
#include "core.param.h"
#include "bp.param.h"
#include "globals/assert.h"
#include "statistics.h"
}

#define DEBUG(proc_id, args...) \
  _DEBUG(proc_id, DEBUG_BP_DIR, ##args)  // for debugging purposes

// Enumeration of different automata used in paper's testing configurations
enum Automata {
  LAST_TIME = 0,  // Stores last outcome
  A1,  // Records results of last 2 times that history pattern appeared
  A2,  // Saturated up-down counter
  A3,  // Records results of last 2 branch executions
  A4,  // TODO: Haven't figured out reasoning for this one yet
};

enum Automata_state {S0 = 0, S1, S2, S3};   // Enumeration of automata states

// ___Adjustable parameters___ (for experimentation)
// Size of each entry in history register table
#define HRT_entry_size TLA_HRT_ENTRY_SIZE

// The maximum number of entries in the history register table
// TODO: This does nothing. We will need to write some way for the entries over
//  the size limit to be evicted. I think that the paper uses LRU, which would
//  be relatively simple to implement (using a queue representing the age
//  of different instructions). -> This will be done with the AHRT (assosiative 
// history register table), which is essentially an LRU cache. 
// The HHRT is essentially a direct mapped cache (possibly without address 
// checking), so this size can be used during its initialization.
#define HRT_size TLA_HRT_TOTAL_ENTRIES

// The number of buckets in the IHRT (ideal history register table). This is NOT
// to be confused with the number of entries in the pattern table. -> This will
// become ireelevant once the IHRT is implemented using a map 
#define IHRT_buckets 65536

// The automata to use for the pattern table
#define PT_automata TLA_AUTOMATA

// AHRT (Associative History Register) associativity (AHRT uses set assoc cache)
#define AHRT_set_assoc AHRT_SET_ASSOC

// Derived parameters
// A bit mask for n bits should be 2^n - 1 (TODO: Double check this)
#define HRT_entry_mask ((1 << HRT_entry_size) - 1)

// We have 2^HR_entry_size entries in the pattern table
#define PT_entries (1 << HRT_entry_size)

namespace {
  // ---------- Set Associative Cache ----------

  // uns_log2(): implementing log2 using shifts
  uns uns_log2(uns n) {
    uns result = 0;
    if (n == 0) return 0;     // log2(0) is undefined, but here, we'll return 0
    while(n >>= 1) result++;  // Shift n right until it is 0

    return result;            // The number of shifts is the log2 of n
  }

  struct CacheEntry {
    Addr tag;
    uns64 value;
  };

  struct AHRT_Cache {
    uns num_sets;
    uns associativity;
    // Number of bits used for the index into the cache. The rest of the bits
    // are used for the tag.
    uns index_len;
    // The outer vector represents the sets, and the inner list represents the
    // entries in each set. Each entry is a struct containing the tag and value.
    std::vector<std::list<CacheEntry>> cache;
  };

  // Initialize the cache with the given number of sets. Unlike the previous
  // version, we don't initialize the sets at the start. This means that we
  // don't need a valid bit, but we do need to check at each update if the
  // set is full and evict the least recently used entry if it is.
  void ahrt_cache_init(AHRT_Cache* cache, uns num_sets, uns associativity) {
    cache->num_sets = num_sets;
    cache->associativity = associativity;
    cache->index_len = uns_log2(num_sets);
    cache->cache.resize(num_sets);
  }

  // Get the value associated with the given address. If the key is found, the
  // value is updated and the entry is moved to the front of the set (most
  // recently used position). If the key is not found, the function returns
  // false. This is a bit more generic than the previous version and the
  // AHRT-specific logic is moved to the calling code.
  bool ahrt_cache_get(AHRT_Cache* cache, const Addr addr, uns64& value) {
    const uns set_index = addr % cache->num_sets;
    auto& set = cache->cache[set_index];
    const Addr tag = addr >> cache->index_len;

    // Iterate through the set from start to end
    for (auto it = set.begin(); it != set.end(); ++it) {
      if (it->tag == tag) {
        value = it->value;
        // Move the accessed entry to the front (most recently used position)
        set.splice(set.begin(), set, it);
        return true;
      }
    }
    return false;
  }

  // Insert or update the value associated with the given address. If the
  // address is not found in the cache, a new entry is inserted at the front
  // of the set. If the set is full, the least recently used entry is evicted.
  void ahrt_cache_put(AHRT_Cache* cache, const Addr addr, const uns64 value) {
    const uns set_index = addr % cache->num_sets;
    auto& set = cache->cache[set_index];
    const Addr tag = addr >> cache->index_len;

    for (auto it = set.begin(); it != set.end(); ++it) {
      if (it->tag == tag) {
        // Update the value and move the entry to the front
        it->value = value;
        set.splice(set.begin(), set, it);
        return;
      }
    }

    // Remove least recently used entry if the set is full
    if (set.size() >= cache->associativity) {
      set.pop_back();
    }

    // Insert the new entry at the front
    set.emplace_front(CacheEntry{tag, value});
  }


  // ---------- TLA_State struct ----------
  struct TLA_State {
    // CacheState ahr_table;   // AHRT (Associative History Register Table)
    AHRT_Cache ahr_table;
    std::vector<uns64> hash_hr_table;     // HHRT (Hash History Register   
    std::map<uns64, uns64> ihr_table;   // IHRT (Ideal History Register Table)

    std::vector<uns8> pattern_table;    // PT (Pattern Table)
  };
  std::vector<TLA_State> tla_state_all_cores;


  // ahrt_init: Initialize the AHRT with its proper size and all contents to 0.
  void ahrt_init(TLA_State* tla_state) {
    ahrt_cache_init(&tla_state->ahr_table, HRT_size/AHRT_set_assoc, AHRT_set_assoc);
  }

  // ahrt_get: Get the history register content for the given address
  uns64 ahrt_get(TLA_State* tla_state, const Addr addr) {
    uns64 history;
    if(ahrt_cache_get(&tla_state->ahr_table, addr, history)) {
      return history & HRT_entry_mask;
    } else {
      return 0;
    }
  }

  // ahrt_update: Update the history register content for the given address with
  // the branch outcome
  void ahrt_update(TLA_State* tla_state, const Addr addr, const uns8 outcome) {
    auto history = ahrt_get(tla_state, addr);
    history = (history << 1) | (outcome & 0x1);
    ahrt_cache_put(&tla_state->ahr_table, addr, history);
  }

  // ---------- Hash History Register Table ----------
  // Note: The current implementation is based on a hash table. We can probably
  // do a similar thing to the automata and add a wrapper to allow for a cache
  // implementation. -> In reality, the hash table implementation didn't account
  // for limited HRT size and resembled the IHRT (ideal history register table),
  // so the previous functionality was repurposed for the IHRT.

  // ihrt_init: Initialize the HHRT with its proper size and all contents to 0.
  void hhrt_init(TLA_State* tla_state) {
    tla_state->hash_hr_table.resize(HRT_size, 0);
  }

  // Collisions: The paper mentions "interference" in the execution history
  // when using the HHRT. It may be possible that this is referring to the
  // fact that the HRRT have no address checking, so if 2 branch addresses
  // share the same hash, they may end up using the same HR entry. I find this
  // somewhat unlikely, but I'll assume that for now. (The AHRT has address
  // checking, so I would expect if they wanted to simulate a direct-mapped
  // HRT with address checking, they would have at least 1 setup with a 1-way
  // set associative AHRT.)

  // hhrt_get: Get the ideal history register content for the given address
  uns64 hhrt_get(const TLA_State* tla_state, const Addr addr) {
    // TODO: Make sure this is right!!
    int HHRT_index = static_cast<int>(addr) % HRT_size;
    // void* data = &tla_state->hash_hr_table[HHRT_index];
    // no collision checking for now (so expect performance to be... bad)
    const uns64 history = tla_state->hash_hr_table.at(HHRT_index);

    // Return the last HRT_entry_size bits of the history register content
    // This removes any history older than HRT_entry_size, effectively working
    // as though the underlying data structure has HRT_entry_size bits.
    return history & HRT_entry_mask;
  }

  // ihrt_update: Update the hash history register content for the given address
  // with  the branch outcome.
  void hhrt_update(TLA_State* tla_state, const Addr addr, const uns8 outcome) {
    // TODO: Make sure this is right!!
    int   HHRT_index = static_cast<int>(addr) % HRT_size;
    auto* history    = &tla_state->hash_hr_table[HHRT_index];
    // no collision checking (so expect performance to be... bad)

    // Left shift and insert the latest outcome bit
    *history = (*history << 1) | (outcome & 0x1);
  }

  // ---------- Ideal History Register Table ----------

  // ihrt_init: Initialize the underlying hash table for the history register
  // table. We can setting all entries to 0 at the time of the first update.
  // Before that, the hash table accesses return NULL if the entry is not found,
  // which the wrapper function returns as 0.
  // NOTE: This has been changed to use a map, since the scarab hashtable may not
  // allow for theorietical unlimited capacity

  // ihrt_init: Since we're using a map, we don't actually need this, so it's blank
  void ihrt_init(TLA_State* tla_state) {
    //init_hash_table(&tla_state->ihr_table, "history_register_table", IHRT_buckets,
    //                sizeof(uns64));
  }

  // ihrt_get: Get the ideal history register content for the given address
  // Don't need to handle collisions (since the IHRT is used to simulate the
  // theoretical scenario of every branch being able to have its own HR)
  uns64 ihrt_get(TLA_State* tla_state, const Addr addr) {
    uns64 history;

    if (tla_state->ihr_table.count(addr)) {       // Return 0 if the entry is not found
      history = tla_state->ihr_table[addr];
    }  else {
      return 0;
    }

    // Return the last HRT_entry_size bits of the history register content
    // This removes any history older than HRT_entry_size, effectively working
    // as though the underlying data structure has HRT_entry_size bits.
    return history & HRT_entry_mask;
  }

  // ihrt_update: Update the history register content for the given address with
  // the branch outcome. The other implementation used the
  // hash_table_access_replace function which might be a better choice here as
  // well, though I think that this also works.
  void ihrt_update(TLA_State* tla_state, const Addr addr, const uns8 outcome) {
   
    // If addr not initally found in ihrt, insert entry int ihrt using initial outcome
    if(! tla_state->ihr_table.count(addr)) {      
      tla_state->ihr_table.insert(std::pair<uint64_t, uint64_t>(addr, outcome));
    } else {                                
      tla_state->ihr_table[addr] = (tla_state->ihr_table[addr] << 1) | (outcome & 0x1) ;
    }
    
  }

  // ---------- History Register Selection Mechanism ----------

  // Select the appropriate function to call depending on which underlying
  // mechanism is being used for the history register table.
  void hrt_init(TLA_State* tla_state) {
    if(TLA_HRT_MECHANISM == 0) {
      return ahrt_init(tla_state);
    } else if (TLA_HRT_MECHANISM == 1) {
      return hhrt_init(tla_state);
    } else if (TLA_HRT_MECHANISM == 2) {
      return ihrt_init(tla_state);
    } else {
      ASSERT(0, "Invalid HRT mechanism");
    }
  }

  uns64 hrt_get(TLA_State* tla_state, const Addr addr) {
    if(TLA_HRT_MECHANISM == 0) {
      return ahrt_get(tla_state, addr);
    } else if (TLA_HRT_MECHANISM == 1) {
      return hhrt_get(tla_state, addr);
    } else if (TLA_HRT_MECHANISM == 2) {
      return ihrt_get(tla_state, addr);
    } else {
      ASSERT(0, "Invalid HRT mechanism");
      return 0;
    }
  }

  void hrt_update(TLA_State* tla_state, const Addr addr, const uns8 outcome) {
    if(TLA_HRT_MECHANISM == 0) {
      return ahrt_update(tla_state, addr, outcome);
    } else if (TLA_HRT_MECHANISM == 1) {
      return hhrt_update(tla_state, addr, outcome);
    } else if (TLA_HRT_MECHANISM == 2) {
      return ihrt_update(tla_state, addr, outcome);
    } else {
      ASSERT(0, "Invalid HRT mechanism");
    }
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
  // TODO: Check that all the automata make sense and actually work
  // Each automata has 2 functions:
  // Update function: Gets the current state and the latest branch result and returns
  // the new state after taking the appropriate transition. Not called directly by bp 
  // functions, use automata_update instead.

  // Get_pred function: gets the current state and returns the prediction based on the 
  // state, without changing the state. Not called directly, use automata_get instead.

  // Last time automata - predict the same as the last outcome
  // Last time update function: Update to same status as latest result
  Automata_state automata_last_time_update(Automata_state state, const uns8 latest_result) {
    if (latest_result == 1) {
      return S1;
    } else {
      return S0;
    }
  }

  // Last time get_pred: return current state
  uns8 automata_last_time_get_pred(Automata_state state) {
    if (state == S0) {
      return 0;
    } else {
      return 1;
    }
  }

  // Automata A1: Records results of last 2 times that history pattern appeared
  // A1 Update function: Record new history pattern as leftmost bit
  Automata_state automata_A1_update(Automata_state state, const uns8 latest_result) {
    switch(state) {
      case S0: if (latest_result == 0) {
                return S0;
              } else {
                return S1;
              }
      case S1: if (latest_result == 0) {
                return S2;
              } else {
                return S3;
              }
      case S2: if (latest_result == 0) {
                return S0;
              } else {
                return S1;
              }
      case S3: if (latest_result == 0) {
                return S2;
              } else {
                return S3;
              }
      default: return S0;
    }
  }

  // A1 Get_pred: Predict not taken if both last outcomes were not taken
  uns8 automata_A1_get_pred(Automata_state state) {
    switch (state) {
      case S0: return 0;
      case S1: return 1;
      case S2: return 1;
      case S3: return 1;
      default: return 0;
    }
  }

  // Automata A2: Saturated up-down counter
  // Update function: Increments (if taken) or decrements (if not taken) counter
  Automata_state automata_A2_update(Automata_state state, const uns8 latest_result) {
    switch(state) {
      case S0: if (latest_result == 0) {
                return S0;
              } else {
                return S1;
              }
      case S1: if (latest_result == 0) {
                return S0;
              } else {
                return S2;
              }
      case S2: if (latest_result == 0) {
                return S1;
              } else {
                return S3;
              }
      case S3: if (latest_result == 0) {
                return S2;
              } else {
                return S3;
              }
      default: return S0;
    }
  }

  // A2 Get function: predicts not taken if counter < 2
  uns8 automata_A2_get_pred(Automata_state state) {
    switch (state) {
      case S0: return 0;
      case S1: return 0;
      case S2: return 1;
      case S3: return 1;
      default: return 0;
    }
  }

  // Automata A3: Records results of last 2 branches
  // A3 Update function: Update with latest result as leftmost bit
  Automata_state automata_A3_update(Automata_state state, const uns8 latest_result) {
    switch(state) {
      case S0: if (latest_result == 0) {
                return S0;
              } else {
                return S1;
              }
      case S1: if (latest_result == 0) {
                return S0;
              } else {
                return S3;
              }
      case S2: if (latest_result == 0) {
                return S3;
              } else {
                return S0;
              }
      case S3: if (latest_result == 0) {
                return S2;
              } else {
                return S3;
              }
      default: return S0;
    }
  }

  // A3 Get_pred function: Not taken if branch before last branch untaken, else: taken
  uns8 automata_A3_get_pred(Automata_state state) {
    switch (state) {
      case S0: return 0;
      case S1: return 0;
      case S2: return 1;
      case S3: return 1;
      default: return 0;
    }
  }

  // Automata A4: TODO: Haven't figured out reasoning for this one yet
  // A4 Update function (updates state)
  Automata_state automata_A4_update(Automata_state state, const uns8 latest_result) {
    switch(state) {
      case S0: if (latest_result == 0) {
                return S0;
              } else {
                return S1;
              }
      case S1: if (latest_result == 0) {
                return S0;
              } else {
                return S3;
              }
      case S2: if (latest_result == 0) {
                return S1;
              } else {
                return S3;
              }
      case S3: if (latest_result == 0) {
                return S2;
              } else {
                return S3;
              }
      default: return S0;
    }
  }

  // A4 Get function: Not taken if branch before last branch untaken, else: taken 
  uns8 automata_A4_get_pred(Automata_state state) {
    switch (state) {
      case S0: return 0;
      case S1: return 0;
      case S2: return 1;
      case S3: return 1;
      default: return 0;
    }
  }

  // Get the new state by calling the currently selected automata with the current 
  // state and the latest branch result to take the appropriate transition.
  // This is basically just a wrapper around the automata-specific functions.
  // The state should be taken from the appropriate PT entry and the resulting
  // state should be stored back into the same PT entry.
  // Note: This replaced the change_SC function from the earlier versions.

  uns8 automata_update(const uns8 state, const uns8 latest_result) {
    Automata_state AT_state = static_cast<Automata_state>(state);
    switch (PT_automata) {
      case LAST_TIME: return automata_last_time_update(AT_state, latest_result);
      case A1: return automata_A1_update(AT_state, latest_result);
      case A2: return automata_A2_update(AT_state, latest_result);
      case A3: return automata_A3_update(AT_state, latest_result);
      case A4: return automata_A4_update(AT_state, latest_result);
      default: return 0;
    }
  }

  // Get the prediction for the branch by calling the currently selected automata 
  // with the current state to get the prediction based on the state, (without 
  // changing the state).

  uns8 automata_get_pred(const uns8 state) {
    Automata_state AT_state = static_cast<Automata_state>(state);

    switch (PT_automata) {     // get_pred automatically returns uns8
      case LAST_TIME: return automata_last_time_get_pred(AT_state);
      case A1: return automata_A1_get_pred(AT_state);
      case A2: return automata_A2_get_pred(AT_state);
      case A3: return automata_A3_get_pred(AT_state);
      case A4: return automata_A4_get_pred(AT_state);
      default: return 0;
    }
  }

} //  end of namespace

//-------------- BRANCH PREDICTOR FUNCTIONS --------------

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
  auto&       tla_state     = tla_state_all_cores.at(proc_id);
  const Addr  addr          = op->oracle_info.pred_addr;  // instruction address

  // Looking here, we have what appears to be the prediction history for the
  // branch. We actually might not even need to have a history register table,
  // since we could just index into the pattern table with the history (well,
  // at least the last few bits of it). That said, as mentioned above we will
  // need to write some sort of eviction mechanism for HRT entries over the
  // size limit.
  // const uns32 hist          = op->oracle_info.pred_global_hist; // prediction history

  const uns64 history = hrt_get(&tla_state, addr);
  const uns8 automata_state = pt_get(&tla_state, history);
  const uns8 prediction_bit = automata_get_pred(automata_state);

  return prediction_bit;

  // const uns HR_bits = HRT.get_HR_content(addr);
  // const uns8 prediction_bit = BHPT.get_pattern_history_bits(HR_bits);

  // DEBUG(proc_id, "Predicting with bimodal for  op_num:%s  index:%d\n",
  // unsstr64(op->op_num), pht_index); DEBUG(proc_id, "Predicting  addr:%s
  // pht:%u  pred:%d  dir:%d\n", hexstr64s(addr), pht_index, pred,
  // op->oracle_info.dir);

  // TODO: We should add debugging functions like they have for gshare.
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

// Retire and full will be implemented ... later (TODO: determine if we need this)
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
  // const uns32 hist          = op->oracle_info.pred_global_hist;  // prediction history
  const uns8  outcome       = op->oracle_info.dir;  // branch outcome

  // TODO: Check if this is correct. I didn't really check against the paper.
  const uns64 history = hrt_get(&tla_state, addr);
  const uns8 automata_state = pt_get(&tla_state, history);
  const uns8 new_automata_state = automata_update(automata_state, outcome);
  pt_update(&tla_state, history, new_automata_state);
  hrt_update(&tla_state, addr, outcome);
}
