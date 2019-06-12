// Copyright 2019 UCWeb Co., Ltd.

#include "src/llvm/tf/v8-codegen.h"

#include "src/assembler-inl.h"
#include "src/llvm/compiler-state.h"
#include "src/llvm/exception-table-arm.h"
#include "src/llvm/stack-maps.h"

#include <unordered_set>
#include "src/callable.h"
#include "src/factory.h"
#include "src/handles-inl.h"
#include "src/macro-assembler.h"
#include "src/safepoint-table.h"

namespace v8 {
namespace internal {
namespace tf_llvm {

namespace {
class CodeGeneratorLLVM {
 public:
  CodeGeneratorLLVM(Isolate*);
  ~CodeGeneratorLLVM() = default;
  Handle<Code> Generate(const CompilerState& state);

 private:
  int HandleCall(const CallInfo*, const StackMaps::Record&);
  int HandleStoreBarrier(const StackMaps::Record&);
  int HandleReturn(const ReturnInfo*, const StackMaps::Record&);
  int HandleStackMapInfo(const StackMapInfo* stack_map_info,
                         const StackMaps::Record* record);
  void ProcessForConstantLoad(const uint32_t* code_start,
                              const uint32_t* code_end,
                              const LoadConstantRecorder&);
  void ProcessRecordMap(const StackMaps::RecordMap& rm,
                        const StackMapInfoMap& info_map);

  struct RecordReference {
    const StackMaps::Record* record;
    const StackMapInfo* info;
    RecordReference(const StackMaps::Record*, const StackMapInfo*);
    ~RecordReference() = default;
  };

  typedef std::unordered_map<uint32_t, RecordReference> RecordReferenceMap;
  Isolate* isolate_;
  Zone zone_;
  MacroAssembler masm_;
  SafepointTableBuilder safepoint_table_builder_;
  RecordReferenceMap record_reference_map_;
  uint32_t reference_instruction_;
  int slot_count_ = 0;
  bool needs_frame_ = false;
};

// Adjust for LLVM's callseq_end, which will emit a SDNode
// Copy r0. And it will becomes a defined instruction if register allocator
// allocates result other than r0.
inline void AdjustCallSite(int* callsite, const byte* instruction_start) {
  int callsite_adj = *callsite;
  const Instr* where =
      reinterpret_cast<const Instr*>(instruction_start + callsite_adj);
  while (!Assembler::IsBlxReg(*(where - 1))) {
    where -= 1;
    callsite_adj -= sizeof(Instr);
  }
  *callsite = callsite_adj;
}

// Adjust for LLVM's unmergeable block, which results in a branch.
inline void AdjustHandler(int* handler, const byte* instruction_start) {
  int handler_adj = *handler;
  const Instr* where =
      reinterpret_cast<const Instr*>(instruction_start + handler_adj);
  if (Assembler::IsBranch(*where)) {
    handler_adj = handler_adj + Assembler::GetBranchOffset(*where) + 8;
  }
  *handler = handler_adj;
}

void EmitHandlerTable(const CompilerState& state, Isolate* isolate,
                      Handle<Code> code) {
  if (!state.exception_table_) return;
  ExceptionTableARM exception_table(state.exception_table_->data(),
                                    state.exception_table_->size());
  const std::vector<std::tuple<int, int>>& callsite_handler_pairs =
      exception_table.CallSiteHandlerPairs();
  Handle<HandlerTable> table =
      Handle<HandlerTable>::cast(isolate->factory()->NewFixedArray(
          HandlerTable::LengthForReturn(
              static_cast<int>(callsite_handler_pairs.size())),
          TENURED));
  for (size_t i = 0; i < callsite_handler_pairs.size(); ++i) {
    int callsite, handler;
    std::tie(callsite, handler) = callsite_handler_pairs[i];
    AdjustCallSite(&callsite, code->instruction_start());
    AdjustHandler(&handler, code->instruction_start());
    table->SetReturnOffset(static_cast<int>(i), callsite);
    table->SetReturnHandler(static_cast<int>(i), handler);
  }
  code->set_handler_table(*table);
}

CodeGeneratorLLVM::CodeGeneratorLLVM(Isolate* isolate)
    : isolate_(isolate),
      zone_(isolate->allocator(), "llvm"),
      masm_(isolate, nullptr, 0, CodeObjectRequired::kYes),
      safepoint_table_builder_(&zone_) {}

int CodeGeneratorLLVM::HandleCall(const CallInfo* call_info,
                                  const StackMaps::Record& record) {
  auto call_paramters_iterator = call_info->locations().begin();
  int call_target_reg = *(call_paramters_iterator++);
  int pc_offset = masm_.pc_offset();
  RegList reg_list = 0;
  for (; call_paramters_iterator != call_info->locations().end();
       ++call_paramters_iterator) {
    int reg = *call_paramters_iterator;
    reg_list |= 1 << reg;
  }
  if (call_info->is_tailcall() && call_info->tailcall_return_count()) {
    masm_.add(sp, sp, Operand(call_info->tailcall_return_count() * 4));
  }
  if (reg_list != 0) masm_.stm(db_w, sp, reg_list);

  if (!call_info->is_tailcall())
    masm_.blx(Register::from_code(call_target_reg));
  else
    masm_.bx(Register::from_code(call_target_reg));
  if (!call_info->is_tailcall()) {
    // record safepoint
    // FIXME: (UC_linzj) kLazyDeopt is abusing, pass frame-state flags to
    // determine.
    Safepoint safepoint = safepoint_table_builder_.DefineSafepoint(
        &masm_, Safepoint::kSimple, 0, Safepoint::kLazyDeopt);
    for (auto& location : record.locations) {
      if (location.kind != StackMaps::Location::Indirect) continue;
      // only understand stack slot
      if (location.dwarfReg == 13) {
        // Remove the effect from safepoint-table.cc
        safepoint.DefinePointerSlot(
            slot_count_ - 1 - location.offset / kPointerSize, &zone_);
      } else {
        CHECK(location.dwarfReg == 11);
        safepoint.DefinePointerSlot(-location.offset / kPointerSize + 1,
                                    &zone_);
      }
    }
  }
  CHECK(0 == ((masm_.pc_offset() - pc_offset) % sizeof(uint32_t)));
  return (masm_.pc_offset() - pc_offset) / sizeof(uint32_t);
}

int CodeGeneratorLLVM::HandleStoreBarrier(const StackMaps::Record& r) {
  int pc_offset = masm_.pc_offset();
  masm_.blx(ip);
  CHECK(0 == ((masm_.pc_offset() - pc_offset) % sizeof(uint32_t)));
  return (masm_.pc_offset() - pc_offset) / sizeof(uint32_t);
}

int CodeGeneratorLLVM::HandleReturn(const ReturnInfo* info,
                                    const StackMaps::Record&) {
  int instruction_count = 2;
  if (info->pop_count_is_constant()) {
    if (info->constant() != 0)
      masm_.add(sp, sp, Operand(info->constant() * 4));
    else
      instruction_count = 1;
  } else {
    masm_.add(sp, sp, Operand(r1, LSL, 2));
  }
  masm_.bx(lr);
  return instruction_count;
}

int CodeGeneratorLLVM::HandleStackMapInfo(const StackMapInfo* stack_map_info,
                                          const StackMaps::Record* record) {
  switch (stack_map_info->GetType()) {
    case StackMapInfoType::kCallInfo:
      return HandleCall(static_cast<const CallInfo*>(stack_map_info), *record);
    case StackMapInfoType::kStoreBarrier:
      return HandleStoreBarrier(*record);
    case StackMapInfoType::kReturn:
      return HandleReturn(static_cast<const ReturnInfo*>(stack_map_info),
                          *record);
  }
  UNREACHABLE();
}

Handle<Code> CodeGeneratorLLVM::Generate(const CompilerState& state) {
  StackMaps sm;
  if (state.stackMapsSection_) {
    DataView dv(state.stackMapsSection_->data());
    sm.parse(&dv);
  }
  auto rm = sm.computeRecordMap();
  const ByteBuffer& code = state.codeSectionList_.front();
  needs_frame_ = state.needs_frame_;

  const uint32_t* instruction_pointer =
      reinterpret_cast<const uint32_t*>(code.data());
  unsigned num_bytes = code.size();
  const uint32_t* instruction_end =
      reinterpret_cast<const uint32_t*>(code.data() + num_bytes);
  ProcessRecordMap(rm, state.stack_map_info_map_);

  slot_count_ = sm.stackSize() / kPointerSize;
  CHECK(slot_count_ < 0x1000);
  int incremental = 0;
  int base_offset = masm_.pc_offset();
  {
    Assembler::BlockConstPoolScope block_const_pool(&masm_);

    for (; num_bytes; num_bytes -= incremental * sizeof(uint32_t),
                      instruction_pointer += incremental) {
      int pc_offset = masm_.pc_offset() - base_offset;
      CHECK((pc_offset + num_bytes) == code.size());
      auto found = record_reference_map_.find(pc_offset);
      if (found != record_reference_map_.end()) {
        reference_instruction_ = *instruction_pointer;
        auto& record_reference = found->second;
        incremental =
            HandleStackMapInfo(record_reference.info, record_reference.record);
        continue;
      }
      incremental = 1;
      uint32_t instruction = *instruction_pointer;
      masm_.dd(instruction);
    }
  }
  instruction_pointer = reinterpret_cast<const uint32_t*>(code.data());
  ProcessForConstantLoad(instruction_pointer, instruction_end,
                         state.load_constant_recorder_);
  record_reference_map_.clear();
  safepoint_table_builder_.Emit(&masm_, slot_count_);
  CodeDesc desc;
  masm_.GetCode(isolate_, &desc);
  Handle<Code> new_object = isolate_->factory()->NewCode(
      desc, static_cast<Code::Kind>(state.code_kind_), masm_.CodeObject());
  new_object->set_stack_slots(slot_count_);
  new_object->set_safepoint_table_offset(
      safepoint_table_builder_.GetCodeOffset());
  new_object->set_is_turbofanned(true);
  EmitHandlerTable(state, isolate_, new_object);
  return new_object;
}

void CodeGeneratorLLVM::ProcessForConstantLoad(
    const uint32_t* code_start, const uint32_t* code_end,
    const LoadConstantRecorder& load_constant_recorder) {
  // constant_pc_offset, pc_offset, type.
  using WorkListEntry = std::tuple<int, int, LoadConstantRecorder::Type>;
  std::vector<WorkListEntry> work_list;
  typedef std::unordered_set<int> ConstantLocationSet;
  ConstantLocationSet constant_location_set;
  int pc_offset = masm_.pc_offset();
  masm_.LLVMGrowBuffer();
  for (auto instruction_pointer = code_start; instruction_pointer != code_end;
       instruction_pointer += 1) {
    uint32_t instruction = *instruction_pointer;
    if (Assembler::IsLdrPcImmediateOffset(instruction)) {
      Address& address =
          Memory::Address_at(Assembler::constant_pool_entry_address(
              reinterpret_cast<Address>(
                  const_cast<uint32_t*>(instruction_pointer)),
              nullptr));
      std::unique_ptr<StackMapInfo> to_push;
      int constant_pc_offset =
          std::distance(reinterpret_cast<const uint8_t*>(code_start),
                        reinterpret_cast<const uint8_t*>(&address));
      if (constant_location_set.find(constant_pc_offset) !=
          constant_location_set.end())
        continue;
      constant_location_set.insert(constant_pc_offset);
      int pc_offset =
          std::distance(reinterpret_cast<const uint8_t*>(code_start),
                        reinterpret_cast<const uint8_t*>(instruction_pointer));

      LoadConstantRecorder::Type type =
          load_constant_recorder.Query(reinterpret_cast<int64_t>(address));
      work_list.emplace_back(constant_pc_offset, pc_offset, type);
    }
  }
  std::stable_sort(work_list.begin(), work_list.end(),
                   [](const WorkListEntry& lhs, const WorkListEntry& rhs) {
                     return std::get<0>(lhs) < std::get<0>(rhs);
                   });
  for (auto& entry : work_list) {
    switch (std::get<2>(entry)) {
      case LoadConstantRecorder::kHeapConstant:
        masm_.reset_pc(std::get<1>(entry));
        masm_.RecordRelocInfo(RelocInfo::EMBEDDED_OBJECT);
        break;
      case LoadConstantRecorder::kCodeConstant:
        masm_.reset_pc(std::get<1>(entry));
        masm_.RecordRelocInfo(RelocInfo::CODE_TARGET);
        break;
      case LoadConstantRecorder::kExternalReference:
        masm_.reset_pc(std::get<1>(entry));
        masm_.RecordRelocInfo(RelocInfo::EXTERNAL_REFERENCE);
        break;
      case LoadConstantRecorder::kIsolateExternalReference: {
        masm_.reset_pc(std::get<1>(entry));
        masm_.RecordRelocInfo(RelocInfo::EXTERNAL_REFERENCE);
      } break;
      case LoadConstantRecorder::kRecordStubCodeConstant: {
        masm_.reset_pc(std::get<1>(entry));
        masm_.RecordRelocInfo(RelocInfo::CODE_TARGET);
      } break;
      case LoadConstantRecorder::kModuloExternalReference: {
        masm_.reset_pc(std::get<1>(entry));
        masm_.RecordRelocInfo(RelocInfo::EXTERNAL_REFERENCE);
      } break;
      default:
        UNREACHABLE();
    }
  }
  for (auto& entry : work_list) {
    switch (std::get<2>(entry)) {
      case LoadConstantRecorder::kHeapConstant:
      case LoadConstantRecorder::kCodeConstant:
      case LoadConstantRecorder::kExternalReference:
        break;
      case LoadConstantRecorder::kIsolateExternalReference: {
        ExternalReference isolate_external_reference =
            ExternalReference::isolate_address(isolate_);
        masm_.instr_at_put(
            std::get<0>(entry),
            reinterpret_cast<Instr>(isolate_external_reference.address()));
      } break;
      case LoadConstantRecorder::kRecordStubCodeConstant: {
        Callable const callable =
            Builtins::CallableFor(isolate_, Builtins::kRecordWrite);
        masm_.instr_at_put(std::get<0>(entry),
                           reinterpret_cast<Instr>(callable.code().location()));
      } break;
      case LoadConstantRecorder::kModuloExternalReference: {
        ExternalReference modulo_reference =
            ExternalReference::mod_two_doubles_operation(isolate_);
        masm_.instr_at_put(std::get<0>(entry),
                           reinterpret_cast<Instr>(modulo_reference.address()));
      } break;
      default:
        UNREACHABLE();
    }
  }
  masm_.reset_pc(pc_offset);
}

void CodeGeneratorLLVM::ProcessRecordMap(const StackMaps::RecordMap& rm,
                                         const StackMapInfoMap& info_map) {
  for (auto& item : rm) {
    CHECK(item.second.size() == 1);
    auto& record = item.second.front();
    uint32_t instruction_offset = item.first;
    auto stack_map_info_found = info_map.find(record.patchpointID);
    CHECK(stack_map_info_found != info_map.end());
    const StackMapInfo* stack_map_info = stack_map_info_found->second.get();
    switch (stack_map_info->GetType()) {
      case StackMapInfoType::kCallInfo:
      case StackMapInfoType::kStoreBarrier:
      case StackMapInfoType::kReturn:
        break;
      default:
        UNREACHABLE();
    }
#if defined(UC_3_0)
    record_reference_map_.emplace(instruction_offset,
                                  RecordReference(&record, stack_map_info));
#else
    record_reference_map_.insert(std::make_pair(
        instruction_offset, RecordReference(&record, stack_map_info)));
#endif
  }
}

CodeGeneratorLLVM::RecordReference::RecordReference(
    const StackMaps::Record* _record, const StackMapInfo* _info)
    : record(_record), info(_info) {}
}  // namespace

Handle<Code> GenerateCode(Isolate* isolate, const CompilerState& state) {
  HandleScope handle_scope(isolate);
  CodeGeneratorLLVM code_generator(isolate);
  return handle_scope.CloseAndEscape(code_generator.Generate(state));
}
}  // namespace tf_llvm
}  // namespace internal
}  // namespace v8
