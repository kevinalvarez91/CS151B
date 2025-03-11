// Copyright 2025 Blaise Tine
//
// Licensed under the Apache License;
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iostream>
#include <iomanip>
#include <string.h>
#include <assert.h>
#include <util.h>
#include "types.h"
#include "core.h"
#include "debug.h"
#include "processor_impl.h"

using namespace tinyrv;

void Core::issue() {
  if (issue_queue_->empty())
    return;

  auto& is_data = issue_queue_->data();
  auto instr = is_data.instr;
  auto exe_flags = instr->getExeFlags();

  /*
  Check for structural hazards
  From LECTURE, check RS and ROB avalibility
  */

  // check for structial hazards
  // TODO:
  if(RS_.full() || ROB_.full()){
    return; // Stall for the next cycle
  }


  uint32_t rs1_data = 0;  // rs1 data obtained from register file or ROB
  uint32_t rs2_data = 0;  // rs2 data obtained from register file or ROB
  int rs1_rsid = -1;      // reservation station id for rs1 (-1 indicates data in already available)
  int rs2_rsid = -1;      // reservation station id for rs2 (-1 indicates data is already available)

  auto rs1 = instr->getRs1();
  auto rs2 = instr->getRs2();

  // get rs1 data
  // check the RAT if value is in the registe file
  // if not in the register file, check data is in the ROB
  // else set rs1_rsid to the reservation station id producing the data
  // remember to first check if the instruction actually uses rs1
  // HINT: should use RAT, ROB, RST, and reg_file_
  // TODO:
  if(exe_flags.use_rs1) {
    if(!RAT_.exists(rs1)){
      rs1_data = reg_file_.at(rs1);
      rs1_rsid = -1;
    }
    else{
      //rat has a entry, meaning rs1 is produced by the ROB
      int rob_index = RAT_.get(rs1);
      const auto& rob_entry = ROB_.get_entry(rob_index);
      if(rob_entry.ready){
        rs1_data = rob_entry.result;
        rs1_rsid = -1;
      }
      else{ // Make sure to check the RST_ 
        rs1_rsid = RST_[rob_index];
      }
    }
  }

  // get rs2 data
  // check the RAT if value is in the registe file
  // if not in the register file, check data is in the ROB
  // else set rs1_rsid to the reservation station id producing the data
  // remember to first check if the instruction actually uses rs2
  // HINT: should use RAT, ROB, RST, and reg_file_
  // TODO:
  if(exe_flags.use_rs2){
    if(!RAT_.exists(rs2)){
      rs2_data = reg_file_.at(rs2);
      rs2_rsid = -1;
    }
    else{
      int rob_index = RAT_.get(rs2);
      const auto& rob_entry = ROB_.get_entry(rob_index);
      if(rob_entry.ready){
        rs2_data = rob_entry.result;
        rs2_rsid = -1;
      }
      else{
        rs2_rsid = RST_[rob_index];
      }
    }
  }

  // allocat new ROB entry and obtain its index
  // TODO:
 int rob_Allocation = ROB_.allocate(instr);
 // Just anthoer step of error checking, should never happen
 if(rob_Allocation < 0){ 
    return;
 }

  // update the RAT mapping if this instruction write to the register file
  // TODO:
  if(exe_flags.use_rd){
    // The value is in rob_Allocation, because the ROB acts like the PRF
    int rd = instr->getRd();
    RAT_.set(rd, rob_Allocation);
  }

  // issue the instruction to free reservation station
  // TODO:
  // We release the reservation station later after the execute stage
  int rs_index = RS_.issue(rob_Allocation, rs1_rsid, rs2_rsid, rs1_data, rs2_data, instr);
  if(rs_index < 0){
    return; // If RS_.issue returns a negative index, we know that something must have gone wrong
  }

  // update RST mapping
  // TODO:
  // Set the RST_ to now point to the reservation station that will execute the instruction
  RST_[rob_Allocation] = rs_index;

  DT(2, "Issue: " << *instr);

  // pop issue queue
  issue_queue_->pop();
}

void Core::execute() {
  // execute functional units
  for (auto fu : FUs_) {
    fu->execute();
  }

  // find the next functional units that is done executing
  // and push its output result to the common data bus
  // then clear the functional unit.
  // The CDB can only serve one functional unit per cycle
  // HINT: should use CDB_ and FUs_
  for (auto fu : FUs_) {
    // TODO:
    if(fu->done()){
      auto cdb_data = fu->get_output();
      CDB_.push(cdb_data.result, cdb_data.rob_index, cdb_data.rs_index);
      fu->clear();
      break;
    }
  }

  // schedule ready instructions to corresponding functional units
  // iterate through all reservation stations, check if the entry is valid, but not running yet,
  // and its operands are ready, and also make sure that is not locked (LSU case).
  // once a candidate is found, issue the instruction to its corresponding functional unit.
  // HINT: should use RS_ and FUs_
  for (int rs_index = 0; rs_index < (int)RS_.size(); ++rs_index) {
    auto& entry = RS_.get_entry(rs_index);
    // TODO:
    if(entry.valid && !entry.running && entry.operands_ready() && !RS_.locked(rs_index)){
      //Determine which FU is necessary
      FUType fu_type = entry.instr->getFUType();
      auto fu = FUs_.at(static_cast<int>(fu_type));
      if(!fu->busy()){
        fu->issue(entry.instr, entry.rob_index, rs_index, entry.rs1_data, entry.rs2_data);
        entry.running = true;
        // Only one instruction per cycle(not superscalar)
        //break;
      }
    }
  }
}

void Core::writeback() {
  // CDB broadcast
  if (CDB_.empty())
    return;

  auto& cdb_data = CDB_.data();

  // update all reservation stations waiting for operands
  // HINT: use RS::entry_t::update_operands()
  for (int rs_index = 0; rs_index < (int)RS_.size(); ++rs_index) {
    // TODO:
    auto& entry = RS_.get_entry(rs_index);
    if(entry.valid){ // I don't think we need to check running status
      entry.update_operands(cdb_data);
    }
  }

  // free the RS entry associated with this CDB response
  // so that it can be used by other instructions
  // TODO:
  RS_.release(cdb_data.rs_index);

  // update ROB
  // TODO:
  ROB_.update(cdb_data);

  // clear CDB
  // TODO:
  CDB_.pop(); // Remove the current data from the CDB

  RS_.dump();
}

void Core::commit() {
  // commit ROB head entry
  if (ROB_.empty())
    return;

  int head_index = ROB_.head_index();
  auto& rob_head = ROB_.get_entry(head_index);

  // check if the head entry is ready to commit
  if (rob_head.ready) {
    auto instr = rob_head.instr;
    auto exe_flags = instr->getExeFlags();

    // If this instruction writes to the register file,
    // (1) update the register file
    // (2) clear the RAT if still pointing to this ROB head
    // HINT: Need to update the RAT to point back to the reg file(don't just clear it) (Set it to -1, actually)
    // TODO:
    if(exe_flags.use_rd){
      int rd = instr->getRd();
      reg_file_.at(rd) = rob_head.result;

      if(RAT_.exists(rd) && RAT_.get(rd) == head_index){
        // Instead of clearing, update RAT to point back to reg_file
        //RAT_.set(rd, -1); // Set it to -1
        RAT_.clear(rd); // Clear the entry
      }
    }


    // pop ROB entry
    // TODO:
    ROB_.pop(); // Get rid of the head entry(and commit)

    DT(2, "Commit: " << *instr);

    assert(perf_stats_.instrs <= fetched_instrs_);
    ++perf_stats_.instrs;

    // handle program termination
    if (exe_flags.is_exit) {
      exited_ = true;
    }
  }

  ROB_.dump();
}