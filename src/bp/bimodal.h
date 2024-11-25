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

#ifndef __BIMODAL_H__
#define __BIMODAL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bp/bp.h"

  /*************Interface to Scarab***************/
  void bp_bimodal_init(void);
  void bp_bimodal_timestamp(Op*);
  uns8 bp_bimodal_pred(Op*);
  void bp_bimodal_spec_update(Op*);
  void bp_bimodal_update(Op*);
  void bp_bimodal_retire(Op*);
  void bp_bimodal_recover(Recovery_Info*);
  uns8 bp_bimodal_full(uns);

#ifdef __cplusplus
}
#endif


#endif  // __BIMODAL_H__
