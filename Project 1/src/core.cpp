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

Core::Core(const SimContext& ctx, uint32_t core_id, ProcessorImpl* processor)
    : SimObject(ctx, "core")
    , core_id_(core_id)
    , processor_(processor)
    , reg_file_(NUM_REGS)
{
  this->reset();
}

Core::~Core() {}

void Core::reset() {
  if_id_.reset();
  id_ex_.reset();
  ex_mem_.reset();
  mem_wb_.reset();
  cout_buf_.clear();

  PC_ = STARTUP_ADDR;

  uuid_ctr_ = 0;

  fetched_instrs_ = 0;
  perf_stats_ = PerfStats();

  fetch_stalled_ = false;
  exited_ = false;
}

void Core::tick() {
  //REMOVE
  // std::cout << "TICK: Cycle" << perf_stats_.cycles 
  //             << " | Instructions executed: " << perf_stats_.instrs 
  //             << " | Stalled: " << fetch_stalled_ << std::endl;
  this->wb_stage();
  this->mem_stage();
  this->ex_stage();
  this->id_stage();
  this->if_stage();

  ++perf_stats_.cycles;
  DPN(2, std::flush);
}

void Core::if_stage() {
  if (fetch_stalled_ || !if_id_.empty())
    return;

  // allocate a new uuid
  uint32_t uuid = uuid_ctr_++;

  // fetch next instruction from memory at PC address
  uint32_t instr_code = 0;
  mmu_.read(&instr_code, PC_, sizeof(uint32_t), 0);

  DT(2, "IF: instr=0x" << instr_code << ", PC=0x" << std::hex << PC_ << std::dec << " (#" << uuid << ")");

  // move instruction data to next stage
  if_id_.push({instr_code, PC_, uuid});

  // advance program counter
  PC_ += 4;

  // lock this stage until branch resoluion
  fetch_stalled_ = true;

  ++fetched_instrs_;
}

void Core::id_stage() {
  if (if_id_.empty() || !id_ex_.empty())
    return;

  auto& stage_data = if_id_.data();

  // instruction decode
  auto instr = this->decode(stage_data.instr_code);

  DT(2, "ID: " << *instr << " (#" << stage_data.uuid << ")");

  // lock fetch stage if exiting program
  if (!instr->getExeFlags().is_exit) {
    fetch_stalled_ = false;
  }

  // branch target bypass
  if (instr->getBrOp() == BrOp::JAL
   && !instr->getExeFlags().is_exit) {
    auto br_target = stage_data.PC + instr->getImm();
    PC_ = br_target;
  }

  // check data hazards
  if (this->check_data_hazards(*instr))
    return;

  // register file access
  uint32_t rs1_data, rs2_data;
  this->regfile_read(*instr, &rs1_data, &rs2_data);

  // move instruction data to next stage
  id_ex_.push({instr, rs1_data, rs2_data, stage_data.PC, stage_data.uuid});
  if_id_.pop();
}

void Core::ex_stage() {
  if (id_ex_.empty() || !ex_mem_.empty())
    return;

  auto& stage_data = id_ex_.data();

  // ALU operations
  auto result = this->alu_unit(*stage_data.instr, stage_data.rs1_data, stage_data.rs2_data, stage_data.PC);

  // Branch operations
  result = this->branch_unit(*stage_data.instr, stage_data.rs1_data, stage_data.rs2_data, result, stage_data.PC);

  DT(2, "EX: result=0x" << std::hex << result << std::dec << " (#" << stage_data.uuid << ")");

  // move instruction data to next stage
  ex_mem_.push({stage_data.instr, stage_data.rs1_data, stage_data.rs2_data, result, stage_data.PC, stage_data.uuid});
  id_ex_.pop();
}

void Core::mem_stage() {
  if (ex_mem_.empty() || !mem_wb_.empty())
    return;

  auto& stage_data = ex_mem_.data();

  auto result = this->mem_access(*stage_data.instr, stage_data.result, stage_data.rs2_data);

  DT(3, "MEM: result=0x" << std::hex << result << std::dec << " (#" << stage_data.uuid << ")");

  // move instruction data to next stage
  mem_wb_.push({stage_data.instr, result, stage_data.PC, stage_data.uuid});
  ex_mem_.pop();
}

void Core::wb_stage() {
  if (mem_wb_.empty())
    return;

  auto& stage_data = mem_wb_.data();

  // update register file
  this->regfile_write(*stage_data.instr, stage_data.result);

  DT(3, "WB:" << std::dec << " (#" << stage_data.uuid << ")");

  assert(perf_stats_.instrs <= fetched_instrs_);
  ++perf_stats_.instrs;

  // handle program termination
  if (stage_data.instr->getExeFlags().is_exit) {
    exited_ = true;
  }

  mem_wb_.pop();
}

/*
 

 *bool Core::check_data_hazards(const Instr &instr) {
  auto exe_flags = instr.getExeFlags();

  if (!ex_mem_.empty()) {
    auto& ex_data = ex_mem_.data();
    // TODO: check LDAD instruction data hazards in EX/MEM
  }

  return false;
}

bool Core::data_forwarding(uint32_t reg, uint32_t* data) {
  bool forwarded = false;

  if (!ex_mem_.empty()) {
    auto& ex_data = ex_mem_.data();
    auto& ex_instr = *ex_data.instr;
    // TODO: check data forwarding from EX/MEM
  }

  if (!forwarded && !mem_wb_.empty()) {
    auto& mem_data = mem_wb_.data();
    auto& mem_instr = *mem_data.instr;
    // TODO: check data forwarding from MEM/WB
  }

  return forwarded;
}
 
 * */

/*
ECALL(115)
ADDI(19)
AUIPC(23)
BEQ(99)
FENCE(15)
LUI(55)
JAL(111)



*/




bool Core::check_data_hazards(const Instr &instr) {
  auto id_flag = instr.getExeFlags();
  //debugging, so function is getting called correctly
  // std::cout << "DEBUG: if_stage() setting fetch_stalled_ = true | if_id_.empty() = " 
  //         << if_id_.empty() << " | id_ex_.empty() = " << id_ex_.empty() << std::endl;

  //If EX/MEM has an instruction that is a load, and the new instr uses that Rd...
  if (!ex_mem_.empty()) {
    auto& ex_data = ex_mem_.data();
    auto& ex_instr = *ex_data.instr;
    auto ex_flags = ex_instr.getExeFlags();

    //TODO: check LD-use hazard from EX/MEM DECODING IS NOT CORRECT
    if (ex_flags.is_load && ex_flags.use_rd && ex_instr.getRd() != 0) {
      uint32_t id_rs1 = instr.getRs1();
      uint32_t id_rs2 = instr.getRs2();


    // std::cout << "DEBUG: Instruction Type: " << static_cast<int>(instr.getOpcode())
    //       << " | use_rd: " << id_flag.use_rd
    //       << " | Rd: " << instr.getRd() 
    //       << " | use_rs1: " << id_flag.use_rs1
    //       << " | use_rs2: " << id_flag.use_rs2
    //       << " | is_load: " << ex_flags.is_load
    //       << std::endl;

      // If the current's instruction's Rs1 or Rs2 = the load's Rd, that’s a hazard
      if ((id_flag.use_rs1 && id_rs1 == ex_instr.getRd()) || (id_flag.use_rs2 && id_rs2 == ex_instr.getRd())) {
        //debugging
        // std::cout << "STALL: Load-use harzard detected! stalling pipeline " << ex_instr.getRd() << " | ID/EX Rs1=" << id_rs1 
        //               << " Rs2=" << id_rs2 << std::endl;
        //return true;
        //std::cout << "TESTING check_data_hazards" << std::endl;
        return true;  
      }
    }
  }
  return false;
}
/*




*/
                          //register file access  register data
bool Core::data_forwarding(uint32_t reg, uint32_t* data) {
  bool forwarded = false;

  if (!ex_mem_.empty()) {
    auto& ex_data = ex_mem_.data(); //Get a reference to the EX/MEM pipeline register
    auto& ex_instr = *ex_data.instr; //DE-reference the instruction pointer to get the instruction itself
  //   // TODO: check data forwarding from EX/MEM
  //   /*
  //   1a. EX/MEM Rd = ID/EX.Rs1
  //   1b. EX/MEM Rd = ID/EX.Rs2 
  //   AND EX/MEM.RegWrite = 1 
  //   */
   auto ex_flags = ex_instr.getExeFlags();
   uint32_t exRd = ex_instr.getRd();

    if(ex_flags.use_rd && exRd == reg){
        *data = ex_data.result; // Forward from the EX/MEM pipeline reg
        forwarded = true;
        // std::cout << "FORWARD: EX/MEM forwarding Rd=" << exRd 
        //               << " Value=" << ex_data.result << std::endl;
    }

  }

  if (!forwarded && !mem_wb_.empty()) {
    auto& mem_data = mem_wb_.data(); //Get a reference to the MEM/WB pipeline register
    auto& mem_instr = *mem_data.instr; // De-reference the instruction pointer to get the instruction itself
  //   // TODO: check data forwarding from MEM/WB
  //   /*
  //   2a. MEM/WB.Rd = ID/EX.Rs1
  //   2b. MEM/WB.Rd = ID/EX.Rs2 
  //   AND MEM/WB.RegWrite = 1
  //   */
    auto mem_flags = mem_instr.getExeFlags();
    uint32_t memRd = mem_instr.getRd();
  if (mem_flags.use_rd && memRd == reg) {
        *data = mem_data.result; // Forward from the MEM/WB pipeline reg
        forwarded = true;
        // std::cout << "FORWARD: MEM/WB forwarding Rd=" << memRd 
        //               << " Value=" << mem_data.result << std::endl;
    }
  }
  return forwarded;
}


void Core::regfile_read(const Instr &instr, uint32_t* rs1_data, uint32_t* rs2_data) {
  auto exe_flags = instr.getExeFlags();

  uint32_t _rs1_data(0), _rs2_data(0);

  if (exe_flags.use_rs1 && instr.getRs1() != 0) {
    if (!this->data_forwarding(instr.getRs1(), &_rs1_data)) {
      _rs1_data = reg_file_.at(instr.getRs1());
      DT(2, "Regfile: addr=" << instr.getRs1() << ", data=0x" << std::hex << _rs1_data << std::dec << " (#" << if_id_.data().uuid << ")");
    }
  }

  if (exe_flags.use_rs2 && instr.getRs2() != 0) {
    if (!this->data_forwarding(instr.getRs2(), &_rs2_data)) {
      _rs2_data = reg_file_.at(instr.getRs2());
      DT(2, "Regfile: addr=" << instr.getRs2() << ", data=0x" << std::hex << _rs2_data << std::dec << " (#" << if_id_.data().uuid << ")");
    }
  }

  if (exe_flags.is_csr) {
    _rs2_data = this->get_csr(instr.getImm());
     DT(2, "CSR: addr=0x" << std::hex << instr.getImm() << ", data=0x" << _rs2_data << std::dec << " (#" << if_id_.data().uuid << ")");
  }

  *rs1_data = _rs1_data;
  *rs2_data = _rs2_data;
}

void Core::regfile_write(const Instr &instr, uint32_t alu_result) {
  auto exe_flags = instr.getExeFlags();
  if (exe_flags.use_rd && instr.getRd() != 0) {
    reg_file_.at(instr.getRd()) = alu_result;
  }
}

void Core::writeToStdOut(const void* data) {
  char c = *(char*)data;
  cout_buf_ << c;
  if (c == '\n') {
    std::cout << cout_buf_.str() << std::flush;
    cout_buf_.str("");
  }
}

void Core::cout_flush() {
  auto str = cout_buf_.str();
  if (!str.empty()) {
    std::cout << str << std::endl;
  }
}

bool Core::check_exit(Word* exitcode, bool riscv_test) const {
  if (exited_) {
    Word ec = reg_file_.at(3);
    if (riscv_test) {
      *exitcode = (1 - ec);
    } else {
      *exitcode = ec;
    }
    return true;
  }
  return false;
}

bool Core::running() const {
  return (perf_stats_.instrs != fetched_instrs_) || (fetched_instrs_ == 0);
}

void Core::attach_ram(RAM* ram) {
  mmu_.attach(*ram, 0, 0xFFFFFFFF);
}

void Core::showStats() {
  std::cout << std::dec << "PERF: instrs=" << perf_stats_.instrs << ", cycles=" << perf_stats_.cycles << std::endl;
}
