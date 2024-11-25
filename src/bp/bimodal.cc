/* Copyright 2020 HPS/SAFARI Research Groups
*
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
*/

//
// CSE 220 Fall 2024 - lab3
// This is actually not a bimodal predictor, but a copy of the gshare predictor
// from scarab's implementation within gshare.cc and gshare.h.
// It was made to figure out how to add a new predictor and connect
// it to the rest of scarab. This is not part of our implementation.
// It may be expanded to an actual bi-modal predictor to compare against
// our algorithm by the final submission.
//

#include "bimodal.h"

#include <vector>

extern "C" {
#include "bp/bp.param.h"
#include "core.param.h"
#include "globals/assert.h"
#include "statistics.h"
}

#define PHT_INIT_VALUE (0x1 << (PHT_CTR_BITS - 1)) /* weakly taken */
#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_BP_DIR, ##args)

namespace {

struct Bimodal_State {
  std::vector<uns8> pht;
};

std::vector<Bimodal_State> bimodal_state_all_cores;

uns32 get_pht_index(const Addr addr, const uns32 hist) {
  const uns32 cooked_hist = hist >> (32 - HIST_LENGTH);
  const uns32 cooked_addr = (addr >> 2) & N_BIT_MASK(HIST_LENGTH);
  return cooked_hist ^ cooked_addr;
}
}  // namespace

// The only speculative state of gshare is the global history which is managed
// by bp.c. Thus, no internal timestamping and recovery mechanism is needed.
void bp_bimodal_timestamp(Op* op) {}
void bp_bimodal_recover(Recovery_Info* info) {}

// Speculative update is basically the same as update, but it is also called
// for branches that are being executed speculatively. I.e., if you predict
// taken and the branch is not resolved yet, then another branch appears and
// is resolved before the first one, the speculative update is called for the
// second branch (since it was resolved while being executed speculatively).
// This is not necessarily needed, but it is good for performance.
void bp_bimodal_spec_update(Op* op) {}
void bp_bimodal_retire(Op* op) {}
uns8 bp_bimodal_full(uns proc_id) { return 0; }


void bp_bimodal_init() {
  bimodal_state_all_cores.resize(NUM_CORES);
  for(auto& bimodal_state : bimodal_state_all_cores) {
    bimodal_state.pht.resize(1 << HIST_LENGTH, PHT_INIT_VALUE);
  }
}

uns8 bp_bimodal_pred(Op* op) {
  const uns   proc_id      = op->proc_id;
  const auto& bimodal_state = bimodal_state_all_cores.at(proc_id);

  const Addr  addr      = op->oracle_info.pred_addr;
  const uns32 hist      = op->oracle_info.pred_global_hist;
  const uns32 pht_index = get_pht_index(addr, hist);
  const uns8  pht_entry = bimodal_state.pht[pht_index];
  const uns8  pred      = pht_entry >> (PHT_CTR_BITS - 1) & 0x1;

  DEBUG(proc_id, "Predicting with bimodal for  op_num:%s  index:%d\n",
        unsstr64(op->op_num), pht_index);
  DEBUG(proc_id, "Predicting  addr:%s  pht:%u  pred:%d  dir:%d\n",
        hexstr64s(addr), pht_index, pred, op->oracle_info.dir);

  return pred;
}

void bp_bimodal_update(Op* op) {
  if(op->table_info->cf_type != CF_CBR) {
    // If op is not a conditional branch, we do not interact with gshare.
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
