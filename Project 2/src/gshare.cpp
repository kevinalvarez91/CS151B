// Copyright 2024 blaise
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iostream>
#include <assert.h>
#include <util.h>
#include "types.h"
#include "core.h"
#include "debug.h"

using namespace tinyrv;

///////////////////////////////////////////////////////////////////////////////

GShare::GShare(uint32_t BTB_size, uint32_t BHR_size)
  : BTB_(BTB_size, BTB_entry_t{false, 0x0, 0x0})
  , PHT_((1 << BHR_size), 0x0)
  , BHR_(0x0)
  , BTB_shift_(log2ceil(BTB_size))
  , BTB_mask_(BTB_size-1)
  , BHR_mask_((1 << BHR_size)-1) {
  //--
}

GShare::~GShare() {
  //--
}

uint32_t GShare::predict(uint32_t PC) {
  uint32_t next_PC = PC + 4;
  uint8_t pht_index = ((PC>>2) ^ BHR_) & BHR_mask_;
  bool predict_taken = PHT_[pht_index] >= 2;
  
  // TODO:
  uint32_t tag = (PC >> 2) >> BTB_shift_;
  
  if(predict_taken){
    uint32_t btb_index = (PC >> 2) & BTB_mask_;
    auto& btb_entry = BTB_[btb_index];
    if(btb_entry.valid && btb_entry.tag == tag)
      next_PC = btb_entry.target;
  }


  DT(3, "*** GShare: predict PC=0x" << std::hex << PC << std::dec
        << ", next_PC=0x" << std::hex << next_PC << std::dec
        << ", predict_taken=" << predict_taken);
  return next_PC;
}

void GShare::update(uint32_t PC, uint32_t next_PC, bool taken) {
  DT(3, "*** GShare: update PC=0x" << std::hex << PC << std::dec
        << ", next_PC=0x" << std::hex << next_PC << std::dec
        << ", taken=" << taken);

  // TODO:
  //update PHT
  uint8_t pht_index = ((PC>>2) ^ BHR_) & BHR_mask_;
  if(taken){
    if(PHT_[pht_index] < 3){
      PHT_[pht_index]++;
    }
  }
  else{
    if(PHT_[pht_index]>0){
      PHT_[pht_index]--;
    }
  }


  //update BHR
  BHR_ = ((BHR_ << 1) | (taken ? 1 : 0)) & BHR_mask_;

  //update BTB
  if (taken) {
    uint32_t btb_index = (PC >> 2) & BTB_mask_;
    auto& btb_entry = BTB_[btb_index];

    // Set the tag for this PC
    uint32_t tag = (PC >> 2) >> BTB_shift_;
    

    // Update BTB entry with new data
    btb_entry.valid = true;
    btb_entry.tag = tag;
    btb_entry.target = next_PC;
  }
}

///////////////////////////////////////////////////////////////////////////////

GSharePlus::GSharePlus(uint32_t BTB_size, uint32_t BHR_size)
  : BTB_(BTB_size, BTB_entry_t{false, 0x0, 0x0})
  , PHT_((1 << BHR_size), 0x2)
  , BHR_(0x0)
  , BTB_shift_(log2ceil(BTB_size))
  , BTB_mask_(BTB_size-1)
  , BHR_mask_((1 << BHR_size)-1) {
  //--
}

GSharePlus::~GSharePlus() {
  //--
}

uint32_t GSharePlus::predict(uint32_t PC) {
  uint32_t next_PC = PC + 4;
  uint16_t pht_index = ((PC>>2) ^ BHR_) & BHR_mask_;
  bool predict_taken = PHT_[pht_index] >= 2;
  
  // TODO:
  uint32_t tag = (PC >> 2) >> BTB_shift_;
  
  if(predict_taken){
    uint32_t btb_index = (PC >> 2) & BTB_mask_;
    auto& btb_entry = BTB_[btb_index];
    if(btb_entry.valid && btb_entry.tag == tag)
      next_PC = btb_entry.target;
  }


  DT(3, "*** GShare: predict PC=0x" << std::hex << PC << std::dec
        << ", next_PC=0x" << std::hex << next_PC << std::dec
        << ", predict_taken=" << predict_taken);
  return next_PC;
}

void GSharePlus::update(uint32_t PC, uint32_t next_PC, bool taken) {
  DT(3, "*** GShare: update PC=0x" << std::hex << PC << std::dec
    << ", next_PC=0x" << std::hex << next_PC << std::dec
    << ", taken=" << taken);

  // TODO:
  //update PHT
  uint16_t pht_index = ((PC>>2) ^ BHR_) & BHR_mask_;
  if(taken){
  if(PHT_[pht_index] < 3){
    PHT_[pht_index]++;
  }
  }
  else{
  if(PHT_[pht_index]>0){
    PHT_[pht_index]--;
  }
  }


  //update BHR
  BHR_ = ((BHR_ << 1) | (taken ? 1 : 0)) & BHR_mask_;

  //update BTB
  if (taken) {
  uint32_t btb_index = (PC >> 2) & BTB_mask_;
  auto& btb_entry = BTB_[btb_index];

  // Set the tag for this PC
  uint32_t tag = (PC >> 2) >> BTB_shift_;


  // Update BTB entry with new data
  btb_entry.valid = true;
  btb_entry.tag = tag;
  btb_entry.target = next_PC;
  }
}


