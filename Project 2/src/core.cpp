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

extern int gshare_enabled;

Core::Core(const SimContext& ctx, uint32_t core_id, ProcessorImpl* processor)
    : SimObject(ctx, "core")
    , core_id_(core_id)
    , processor_(processor)
    , reg_file_(NUM_REGS)
    , if_id_(PipelineReg<if_id_t>::Create("if_id"))
    , id_ex_(PipelineReg<id_ex_t>::Create("id_ex"))
    , ex_mem_(PipelineReg<ex_mem_t>::Create("ex_mem"))
    , mem_wb_(PipelineReg<mem_wb_t>::Create("mem_wb"))
	, bpred_(NULL)
{
  if (gshare_enabled == 1) {
    bpred_ = new GShare(BTB_SIZE, BHR_SIZE);
  } else if (gshare_enabled == 2) {
    bpred_ = new GSharePlus(BTB_SIZE, BHR_SIZE);
  }
  this->reset();
}

Core::~Core() {
  if (bpred_) {
    delete bpred_;
  }
}

void Core::reset() {
  if_id_->reset();
  id_ex_->reset();
  ex_mem_->reset();
  mem_wb_->reset();
  cout_buf_.clear();

  PC_ = STARTUP_ADDR;

  uuid_ctr_ = 0;

  fetched_instrs_ = 0;
  perf_stats_ = PerfStats();

  fetch_stalled_ = false;
  exited_ = false;
}

void Core::tick() {
  pipeline_stalled_ = false;

  this->wb_stage();
  this->mem_stage();
  this->ex_stage();
  this->id_stage();
  this->if_stage();

  ++perf_stats_.cycles;
  DPN(2, std::flush);
}

void Core::if_stage() {
  if (fetch_stalled_ || pipeline_stalled_)
    return;

  // allocate a new uuid
  uint32_t uuid = uuid_ctr_++;

  // fetch next instruction from memory at PC address
  uint32_t instr_code = 0;
  mmu_.read(&instr_code, PC_, sizeof(uint32_t), 0);

  DT(2, "IF: instr=0x" << instr_code << ", PC=0x" << std::hex << PC_ << std::dec << " (#" << uuid << ")");

  // move instruction data to next stage
  if_id_->push({instr_code, PC_, uuid});

  // advance program counter
  if (gshare_enabled) {
    PC_ = bpred_->predict(PC_);
  } else {
    PC_ += 4;
  }

  ++fetched_instrs_;
}

void Core::id_stage() {
  if (!if_id_->valid() || pipeline_stalled_)
    return;

  auto& stage_data = if_id_->data();

  // instruction decode
  auto instr = this->decode(stage_data.instr_code);

  DT(2, "ID: " << *instr << " (#" << stage_data.uuid << ")");

  // lock fetch stage if exiting program
  if (instr->getExeFlags().is_exit) {
    fetch_stalled_ = true;
  }

  // check data hazards
  if (this->check_data_hazards(*instr)) {
    pipeline_stalled_ = true;
    return;
  }

  // register file access
  uint32_t rs1_data, rs2_data;
  this->regfile_read(*instr, &rs1_data, &rs2_data);

  // move instruction data to next stage
  id_ex_->push({instr, rs1_data, rs2_data, stage_data.PC, stage_data.uuid});
  if_id_->pop();
}

void Core::ex_stage() {
  if (!id_ex_->valid() || pipeline_stalled_)
    return;

  auto& stage_data = id_ex_->data();
  auto instr = stage_data.instr;

  auto rs1_data = stage_data.rs1_data;
  auto rs2_data = stage_data.rs2_data;

  // daa forwarding
  if (instr->getExeFlags().use_rs1) {
    rs1_data = this->data_forwarding(instr->getRs1(), rs1_data);
  }
  if (instr->getExeFlags().use_rs2) {
    rs2_data = this->data_forwarding(instr->getRs2(), rs2_data);
  }

  // ALU operations
  auto result = this->alu_unit(*instr, rs1_data, rs2_data, stage_data.PC);

  // Branch operations
  result = this->branch_unit(*instr, rs1_data, rs2_data, result, stage_data.PC);

  DT(2, "EX: result=0x" << std::hex << result << std::dec << " (#" << stage_data.uuid << ")");

  // move instruction data to next stage
  ex_mem_->push({instr, rs1_data, rs2_data, result, stage_data.PC, stage_data.uuid});
  id_ex_->pop();
}

void Core::mem_stage() {
  if (!ex_mem_->valid() || pipeline_stalled_)
    return;

  auto& stage_data = ex_mem_->data();
  auto instr = stage_data.instr;

  auto result = this->mem_access(*instr, stage_data.result, stage_data.rs2_data);

  DT(3, "MEM: result=0x" << std::hex << result << std::dec << " (#" << stage_data.uuid << ")");

  // move instruction data to next stage
  mem_wb_->push({instr, result, stage_data.PC, stage_data.uuid});
  ex_mem_->pop();
}

void Core::wb_stage() {
  if (!mem_wb_->valid() || pipeline_stalled_)
    return;

  auto& stage_data = mem_wb_->data();
  auto instr = stage_data.instr;

  // update register file
  this->regfile_write(*instr, stage_data.result);

  DT(3, "WB:" << std::dec << " (#" << stage_data.uuid << ")");

  assert(perf_stats_.instrs <= fetched_instrs_);
  ++perf_stats_.instrs;

  // handle program termination
  if (instr->getExeFlags().is_exit) {
    exited_ = true;
  }

  mem_wb_->pop();
}

bool Core::check_data_hazards(const Instr &instr) {
  auto exe_flags = instr.getExeFlags();

  if (id_ex_->valid()) {
    auto& ex_data = id_ex_->data();
    auto& ex_instr = *ex_data.instr;
    if (exe_flags.use_rs1 && ex_instr.getExeFlags().is_load && ex_instr.getRd() == instr.getRs1()) {
      DT(2, "*** ID Stall: data hazard on rs1 (#" << if_id_->data().uuid << ")");
      return true;
    }
    if (exe_flags.use_rs2 && ex_instr.getExeFlags().is_load && ex_instr.getRd() == instr.getRs2()) {
      DT(2, "*** ID Stall: data hazard on rs2 (#" << if_id_->data().uuid << ")");
      return true;
    }
    if (exe_flags.is_csr && ex_instr.getExeFlags().is_csr && ex_instr.getImm() == instr.getImm()) {
      DT(2, "*** ID Stall: CSR write at addr=0x" << std::hex << instr.getImm() << std::dec << " (#" << if_id_->data().uuid << ")");
      return true;
    }
  }

  return false;
}

uint32_t Core::data_forwarding(uint32_t reg, uint32_t data) {
  // x0 is hardwired to 0
  if (reg == 0)
    return data;

  if (ex_mem_->valid()) {
    auto& mem_data = ex_mem_->data();
    auto& mem_instr = *mem_data.instr;
    if (mem_instr.getExeFlags().use_rd && mem_instr.getRd() == reg) {
      DT(2, "Forwarding: x" << reg << ", data=0x" << std::hex << mem_data.result << std::dec << " from EX/MEM (#" << id_ex_->data().uuid << ")");
      return mem_data.result;
    }
  }

  if (mem_wb_->valid()) {
    auto& wb_data = mem_wb_->data();
    auto& wb_instr = *wb_data.instr;
    if (wb_instr.getExeFlags().use_rd && wb_instr.getRd() == reg) {
      DT(2, "Forwarding: x" << reg << ", data=0x" << std::hex << wb_data.result << std::dec << " from MEM/WB (#" << id_ex_->data().uuid << ")");
      return wb_data.result;
    }
  }

  return data;
}

void Core::regfile_read(const Instr &instr, uint32_t* rs1_data, uint32_t* rs2_data) {
  auto exe_flags = instr.getExeFlags();

  uint32_t _rs1_data(0), _rs2_data(0);

  if (exe_flags.use_rs1 && instr.getRs1() != 0) {
    _rs1_data = reg_file_.at(instr.getRs1());
    DT(2, "Regfile: addr=" << instr.getRs1() << ", data=0x" << std::hex << _rs1_data << std::dec << " (#" << if_id_->data().uuid << ")");
  }

  if (exe_flags.use_rs2 && instr.getRs2() != 0) {
    _rs2_data = reg_file_.at(instr.getRs2());
    DT(2, "Regfile: addr=" << instr.getRs2() << ", data=0x" << std::hex << _rs2_data << std::dec << " (#" << if_id_->data().uuid << ")");
  }

  if (exe_flags.is_csr) {
    _rs2_data = this->get_csr(instr.getImm());
     DT(2, "CSR: addr=0x" << std::hex << instr.getImm() << ", data=0x" << _rs2_data << std::dec << " (#" << if_id_->data().uuid << ")");
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
  std::cout << std::dec << "PERF: instrs=" << perf_stats_.instrs << ", cycles=" << perf_stats_.cycles
            << ", bpred=" << (perf_stats_.branches - perf_stats_.bpred_miss) << "/"
            << perf_stats_.branches << std::endl;
}
