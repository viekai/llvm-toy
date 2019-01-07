#include "src/llvm/llvm-tf-builder.h"
#include <llvm/Support/Compiler.h>
#include <bitset>
#include <sstream>
#include "src/heap/spaces.h"
#include "src/llvm/basic-block-manager.h"
#include "src/llvm/basic-block.h"
#include "src/llvm/output.h"
#include "src/objects.h"

namespace v8 {
namespace internal {
namespace tf_llvm {
namespace {
// copy from v8.
enum RememberedSetAction { EMIT_REMEMBERED_SET, OMIT_REMEMBERED_SET };
enum SaveFPRegsMode { kDontSaveFPRegs, kSaveFPRegs };

struct NotMergedPhiDesc {
  BasicBlock* pred;
  int value;
  LValue phi;
};

struct LLVMTFBuilderBasicBlockImpl {
  std::vector<NotMergedPhiDesc> not_merged_phis;
  std::unordered_map<int, LValue> values_;
  LBasicBlock native_bb = nullptr;
  LBasicBlock continuation = nullptr;
  bool started = false;
  bool ended = false;

  inline void set_value(int nid, LValue value) { values_[nid] = value; }

  inline LValue value(int nid) {
    auto found = values_.find(nid);
    EMASSERT(found != values_.end());
    return found->second;
  }

  inline std::unordered_map<int, LValue>& values() { return values_; }

  inline void StartBuild() {
    EMASSERT(!started);
    EMASSERT(!ended);
    started = true;
  }

  inline void EndBuild() {
    EMASSERT(started);
    EMASSERT(!ended);
    ended = true;
  }
};

LLVMTFBuilderBasicBlockImpl* EnsureImpl(BasicBlock* bb) {
  if (bb->GetImpl<LLVMTFBuilderBasicBlockImpl>())
    return bb->GetImpl<LLVMTFBuilderBasicBlockImpl>();
  std::unique_ptr<LLVMTFBuilderBasicBlockImpl> impl(
      new LLVMTFBuilderBasicBlockImpl);
  bb->SetImpl(impl.release());
  return bb->GetImpl<LLVMTFBuilderBasicBlockImpl>();
}

LLVMTFBuilderBasicBlockImpl* GetImpl(BasicBlock* bb) {
  return bb->GetImpl<LLVMTFBuilderBasicBlockImpl>();
}

void EnsureNativeBB(BasicBlock* bb, Output& output) {
  LLVMTFBuilderBasicBlockImpl* impl = EnsureImpl(bb);
  if (impl->native_bb) return;
  char buf[256];
  snprintf(buf, 256, "B%d", bb->id());
  LBasicBlock native_bb = output.appendBasicBlock(buf);
  impl->native_bb = native_bb;
  impl->continuation = native_bb;
}

LBasicBlock GetNativeBB(BasicBlock* bb) {
  return bb->GetImpl<LLVMTFBuilderBasicBlockImpl>()->native_bb;
}

LBasicBlock GetNativeBBContinuation(BasicBlock* bb) {
  return bb->GetImpl<LLVMTFBuilderBasicBlockImpl>()->continuation;
}

bool IsBBStartedToBuild(BasicBlock* bb) {
  auto impl = GetImpl(bb);
  if (!impl) return false;
  return impl->started;
}

bool IsBBEndedToBuild(BasicBlock* bb) {
  auto impl = GetImpl(bb);
  if (!impl) return false;
  return impl->ended;
}

void StartBuild(BasicBlock* bb, Output& output) {
  EnsureNativeBB(bb, output);
  GetImpl(bb)->StartBuild();
  output.positionToBBEnd(GetNativeBB(bb));
}

class CallOperandResolver {
 public:
  explicit CallOperandResolver(BasicBlock* current_bb, Output& output,
                               LValue target);
  ~CallOperandResolver() = default;
  void Resolve(OperandsVector::const_iterator operands_iterator,
               OperandsVector::const_iterator end,
               const RegistersForOperands& registers_for_operands);
  inline std::vector<LValue>& operand_values() { return operand_values_; }
  inline std::vector<LType>& operand_value_types() {
    return operand_value_types_;
  }
  inline size_t location_count() const { return locations_.size(); }
  inline CallInfo::LocationVector&& release_location() {
    return std::move(locations_);
  }

 private:
  static const int kV8CCRegisterParameterCount = 12;
  static const int kRootReg = 10;
  static const int kFPReg = 11;
  void SetOperandValue(int reg, LValue value);
  int FindNextReg();
  inline Output& output() { return *output_; }
  std::bitset<kV8CCRegisterParameterCount>
      allocatable_register_set_; /* 0 is allocatable */
  std::vector<LValue> operand_values_;
  std::vector<LType> operand_value_types_;
  CallInfo::LocationVector locations_;
  BasicBlock* current_bb_;
  Output* output_;
  LValue target_;
  int next_reg_;
};

class StoreBarrierResolver {
 public:
  StoreBarrierResolver(BasicBlock* bb, Output& output,
                       StackMapInfoMap* stack_map_info_map, int id,
                       int patch_point_id, bool needs_frame);
  void Resolve(LValue base, LValue offset, LValue value,
               WriteBarrierKind barrier_kind);

 private:
  LBasicBlock CreateContination();
  void CheckPageFlag(LValue base, int flags);
  void CallPatchpoint(LValue base, LValue offset, LValue remembered_set_action,
                      LValue save_fp_mode);
  void CheckSmi(LValue value);
  inline Output& output() { return *output_; }
  BasicBlock* current_bb_;
  Output* output_;
  StackMapInfoMap* stack_map_info_map_;
  int id_;
  int patch_point_id_;
  bool needs_frame_;
};

CallOperandResolver::CallOperandResolver(BasicBlock* current_bb, Output& output,
                                         LValue target)
    : operand_values_(kV8CCRegisterParameterCount,
                      LLVMGetUndef(output.repo().intPtr)),
      operand_value_types_(kV8CCRegisterParameterCount, output.repo().intPtr),
      current_bb_(current_bb),
      output_(&output),
      target_(target),
      next_reg_(0) {}

int CallOperandResolver::FindNextReg() {
  if (next_reg_ < 0) return -1;
  for (int i = next_reg_; i < kV8CCRegisterParameterCount; ++i) {
    if (!allocatable_register_set_.test(i)) {
      next_reg_ = i + 1;
      return i;
    }
  }
  next_reg_ = -1;
  return -1;
}

void CallOperandResolver::SetOperandValue(int reg, LValue llvm_val) {
  LType llvm_val_type = typeOf(llvm_val);
  if (reg >= 0) {
    operand_values_[reg] = llvm_val;
    operand_value_types_[reg] = llvm_val_type;
    allocatable_register_set_.set(reg);
  } else {
    operand_values_.push_back(llvm_val);
    operand_value_types_.push_back(llvm_val_type);
  }
}
void CallOperandResolver::Resolve(
    OperandsVector::const_iterator operands_iterator,
    OperandsVector::const_iterator end,
    const RegistersForOperands& registers_for_operands) {
  // setup register operands
  OperandsVector stack_operands;
  for (int reg : registers_for_operands) {
    EMASSERT(reg < kV8CCRegisterParameterCount);
    if (reg < 0) {
      stack_operands.push_back(*(operands_iterator++));
      continue;
    }
    LValue llvm_val = GetImpl(current_bb_)->value(*(operands_iterator++));
    SetOperandValue(reg, llvm_val);
  }
  // setup artifact operands' value
  SetOperandValue(kRootReg, output().root());
  SetOperandValue(kFPReg, output().fp());
  if (!LLVMIsUndef(target_)) {
    int target_reg = FindNextReg();
    SetOperandValue(target_reg, target_);
    locations_.push_back(target_reg);
  } else {
    locations_.push_back(-1);
  }

  for (auto operand : stack_operands) {
    LValue llvm_val = GetImpl(current_bb_)->value(operand);
    int reg = FindNextReg();
    SetOperandValue(reg, llvm_val);
    locations_.push_back(reg);
  }
}

StoreBarrierResolver::StoreBarrierResolver(BasicBlock* bb, Output& output,
                                           StackMapInfoMap* stack_map_info_map,
                                           int id, int patch_point_id,
                                           bool needs_frame)
    : current_bb_(bb),
      output_(&output),
      stack_map_info_map_(stack_map_info_map),
      id_(id),
      patch_point_id_(patch_point_id),
      needs_frame_(needs_frame) {}

void StoreBarrierResolver::Resolve(LValue base, LValue offset, LValue value,
                                   WriteBarrierKind barrier_kind) {
  auto impl = GetImpl(current_bb_);
  impl->continuation = CreateContination();
  CheckPageFlag(base, MemoryChunk::kPointersFromHereAreInterestingMask);
  if (barrier_kind > kPointerWriteBarrier) {
    CheckSmi(value);
  }
  CheckPageFlag(value, MemoryChunk::kPointersToHereAreInterestingMask);
  RememberedSetAction const remembered_set_action =
      barrier_kind > kMapWriteBarrier ? EMIT_REMEMBERED_SET
                                      : OMIT_REMEMBERED_SET;
  // now v8cc clobbers all fp.
  SaveFPRegsMode const save_fp_mode = kDontSaveFPRegs;
  CallPatchpoint(
      base, offset,
      output().constIntPtr(static_cast<int>(remembered_set_action) << 1),
      output().constIntPtr(static_cast<int>(save_fp_mode) << 1));
  output().buildBr(impl->continuation);
  output().positionToBBEnd(impl->continuation);
}

LBasicBlock StoreBarrierResolver::CreateContination() {
  char buf[256];
  snprintf(buf, 256, "B%d_value%d_continuation", current_bb_->id(), id_);
  LBasicBlock native_bb = output().appendBasicBlock(buf);
  return native_bb;
}

void StoreBarrierResolver::CheckPageFlag(LValue base, int mask) {
  LValue base_int =
      output().buildCast(LLVMPtrToInt, base, output().repo().intPtr);
  const int page_mask = ~((1 << kPageSizeBits) - 1);
  LValue memchunk_int =
      output().buildAnd(base_int, output().constIntPtr(page_mask));
  LValue memchunk_ref8 =
      output().buildCast(LLVMIntToPtr, memchunk_int, output().repo().ref8);
  LValue flag_slot = output().buildGEPWithByteOffset(
      memchunk_ref8, output().constInt32(MemoryChunk::kFlagsOffset),
      output().repo().ref32);
  LValue flag = output().buildLoad(flag_slot);
  LValue and_result = output().buildAnd(flag, output().constInt32(mask));
  LValue cmp =
      output().buildICmp(LLVMIntEQ, and_result, output().repo().int32Zero);

  char buf[256];
  snprintf(buf, 256, "B%d_value%d_checkpageflag_%d", current_bb_->id(), id_,
           mask);
  LBasicBlock continuation = output().appendBasicBlock(buf);
  output().buildCondBr(cmp, GetNativeBBContinuation(current_bb_), continuation);
  output().positionToBBEnd(continuation);
}

void StoreBarrierResolver::CallPatchpoint(LValue base, LValue offset,
                                          LValue remembered_set_action,
                                          LValue save_fp_mode) {
  // mov ip xxx,
  // blx ip
  // mov r2 xxx
  // 3 instructions.
  int instructions_count = 3;
  int patchid = patch_point_id_;
  // will not be true again.
  if (!needs_frame_) {
    // 2 for save/restore lr
    instructions_count += 2;
    output().ensureLR();
  }

  LValue call = output().buildCall(
      output().repo().patchpointVoidIntrinsic(), output().constInt64(patchid),
      output().constInt32(4 * instructions_count),
      constNull(output().repo().ref8), output().constInt32(6), base, offset,
      LLVMGetUndef(output().repo().int32), remembered_set_action, save_fp_mode,
      output().root());
  LLVMSetInstructionCallConv(call, LLVMV8SBCallConv);
  std::unique_ptr<StackMapInfo> info(new StoreBarrierInfo());
#if defined(UC_3_0)
  stack_map_info_map_->emplace(patchid, std::move(info));
#else
  stack_map_info_map_->insert(std::make_pair(patchid, std::move(info)));
#endif
}

void StoreBarrierResolver::CheckSmi(LValue value) {
  LValue value_int =
      output().buildCast(LLVMPtrToInt, value, output().repo().intPtr);
  LValue and_result = output().buildAnd(value_int, output().repo().intPtrOne);
  LValue cmp =
      output().buildICmp(LLVMIntEQ, and_result, output().repo().int32Zero);
  char buf[256];
  snprintf(buf, 256, "B%d_value%d_checksmi", current_bb_->id(), id_);
  LBasicBlock continuation = output().appendBasicBlock(buf);
  output().buildCondBr(cmp, GetNativeBBContinuation(current_bb_), continuation);
  output().positionToBBEnd(continuation);
}

}  // namespace

LLVMTFBuilder::LLVMTFBuilder(Output& output,
                             BasicBlockManager& basic_block_manager,
                             StackMapInfoMap& stack_map_info_map)
    : output_(&output),
      basic_block_manager_(&basic_block_manager),
      current_bb_(nullptr),
      stack_map_info_map_(&stack_map_info_map),
      state_point_id_next_(0) {}

void LLVMTFBuilder::End() {
  EMASSERT(!!current_bb_);
  EndCurrentBlock();
  ProcessPhiWorkList();
  output().positionToBBEnd(output().prologue());
  output().buildBr(GetNativeBB(
      basic_block_manager().findBB(*basic_block_manager().rpo().begin())));
  v8::internal::tf_llvm::ResetImpls<LLVMTFBuilderBasicBlockImpl>(
      basic_block_manager());
}

void LLVMTFBuilder::MergePredecessors(BasicBlock* bb) {
  if (bb->predecessors().empty()) return;
  if (bb->predecessors().size() == 1) {
    // Don't use phi if only one predecessor.
    BasicBlock* pred = bb->predecessors()[0];
    EMASSERT(IsBBStartedToBuild(pred));
    for (int live : bb->liveins()) {
      LValue value = GetImpl(pred)->value(live);
      GetImpl(bb)->set_value(live, value);
    }
    return;
  }
  BasicBlock* ref_pred = nullptr;
  if (!AllPredecessorStarted(bb, &ref_pred)) {
    EMASSERT(!!ref_pred);
    BuildPhiAndPushToWorkList(bb, ref_pred);
    return;
  }
  // Use phi.
  for (int live : bb->liveins()) {
    LValue ref_value = GetImpl(ref_pred)->value(live);
    LType ref_type = typeOf(ref_value);
    if (ref_type != output().taggedType()) {
      // FIXME: Should add EMASSERT that all values are the same.
      GetImpl(bb)->set_value(live, ref_value);
      continue;
    }
    LValue phi = output().buildPhi(ref_type);
    for (BasicBlock* pred : bb->predecessors()) {
      LValue value = GetImpl(pred)->value(live);
      LBasicBlock native = GetNativeBBContinuation(pred);
      addIncoming(phi, &value, &native, 1);
    }
    GetImpl(bb)->set_value(live, phi);
  }
}

bool LLVMTFBuilder::AllPredecessorStarted(BasicBlock* bb,
                                          BasicBlock** ref_pred) {
  bool ret_value = true;
  for (BasicBlock* pred : bb->predecessors()) {
    if (IsBBStartedToBuild(pred)) {
      if (!*ref_pred) *ref_pred = pred;
    } else {
      ret_value = false;
    }
  }
  return ret_value;
}

void LLVMTFBuilder::BuildPhiAndPushToWorkList(BasicBlock* bb,
                                              BasicBlock* ref_pred) {
  auto impl = EnsureImpl(bb);
  for (int live : bb->liveins()) {
    LValue ref_value = GetImpl(ref_pred)->value(live);
    LType ref_type = typeOf(ref_value);
    if (ref_type != output().taggedType()) {
      GetImpl(bb)->set_value(live, ref_value);
      continue;
    }
    LValue phi = output().buildPhi(ref_type);
    GetImpl(bb)->set_value(live, phi);
    for (BasicBlock* pred : bb->predecessors()) {
      if (!IsBBStartedToBuild(pred)) {
        impl->not_merged_phis.emplace_back();
        NotMergedPhiDesc& not_merged_phi = impl->not_merged_phis.back();
        not_merged_phi.phi = phi;
        not_merged_phi.value = live;
        not_merged_phi.pred = pred;
        continue;
      }
      LValue value = GetImpl(pred)->value(live);
      LBasicBlock native = GetNativeBBContinuation(pred);
      addIncoming(phi, &value, &native, 1);
    }
  }
  phi_rebuild_worklist_.push_back(bb);
}

void LLVMTFBuilder::ProcessPhiWorkList() {
  for (BasicBlock* bb : phi_rebuild_worklist_) {
    auto impl = bb->GetImpl<LLVMTFBuilderBasicBlockImpl>();
    for (auto& e : impl->not_merged_phis) {
      BasicBlock* pred = e.pred;
      EMASSERT(IsBBStartedToBuild(pred));
      LValue value = EnsurePhiInput(pred, e.value, typeOf(e.phi));
      LBasicBlock native = GetNativeBBContinuation(pred);
      addIncoming(e.phi, &value, &native, 1);
    }
    impl->not_merged_phis.clear();
  }
  phi_rebuild_worklist_.clear();
}

void LLVMTFBuilder::DoTailCall(int id, bool code,
                               const CallDescriptor& call_desc,
                               const OperandsVector& operands) {
  DoCall(id, code, call_desc, operands, true);
  output().buildUnreachable();
}

void LLVMTFBuilder::DoCall(int id, bool code, const CallDescriptor& call_desc,
                           const OperandsVector& operands, bool tailcall) {
  auto operands_iterator = operands.begin();
  LValue ret;
  LValue target;
  int64_t code_magic = 0;
  int addition_branch_instructions = 0;
  // layout
  // return value | register operands | stack operands | artifact operands
  if (code) {
    int code_value = *(operands_iterator++);
    auto found = code_uses_map_.find(code_value);
    if (found != code_uses_map_.end()) {
      code_magic = found->second;
      target = LLVMGetUndef(output().repo().intPtr);
      // ldr to ip
      addition_branch_instructions += 1;
    } else {
      target = output().buildGEPWithByteOffset(
          GetImpl(current_bb_)->value(code_value),
          output().constInt32(Code::kHeaderSize - kHeapObjectTag),
          output().repo().ref8);
    }
  } else {
    int addr_value = *(operands_iterator++);
    target = GetImpl(current_bb_)->value(addr_value);
  }
  if (tailcall) {
    if (basic_block_manager().needs_frame())
      addition_branch_instructions += 2;
    else
      output().ensureLR();
  }
  CallOperandResolver call_operand_resolver(current_bb_, output(), target);
  call_operand_resolver.Resolve(operands_iterator, operands.end(),
                                call_desc.registers_for_operands);
  std::vector<LValue> statepoint_operands;
  LType ret_type = output().repo().taggedType;
  if (call_desc.return_count == 2) {
    ret_type = structType(output().repo().context_, output().taggedType(),
                          output().taggedType());
  }
  LType callee_function_type = functionType(
      ret_type, call_operand_resolver.operand_value_types().data(),
      call_operand_resolver.operand_value_types().size(), NotVariadic);
  LType callee_type = pointerType(callee_function_type);
  int patchid = state_point_id_next_++;
  statepoint_operands.push_back(output().constInt64(patchid));
  statepoint_operands.push_back(
      output().constInt32(4 * (call_operand_resolver.location_count() +
                               addition_branch_instructions)));
  statepoint_operands.push_back(constNull(callee_type));
  statepoint_operands.push_back(output().constInt32(
      call_operand_resolver.operand_values().size()));    // # call params
  statepoint_operands.push_back(output().constInt32(0));  // flags
  for (LValue operand_value : call_operand_resolver.operand_values())
    statepoint_operands.push_back(operand_value);
  statepoint_operands.push_back(output().constInt32(0));  // # transition args
  statepoint_operands.push_back(output().constInt32(0));  // # deopt arguments
  int gc_paramter_start = statepoint_operands.size();
  // push current defines
  if (!tailcall) {
    auto& successor_liveins = current_bb_->successors().front()->liveins();
    auto& values = GetImpl(current_bb_)->values();
    for (int livein : successor_liveins) {
      if (livein == id) continue;
      auto found = values.find(livein);
      EMASSERT(found != values.end());
      LValue to_gc = found->second;
      if (typeOf(to_gc) != output().taggedType()) continue;
      statepoint_operands.push_back(to_gc);
    }
  }
  LValue statepoint_ret = output().buildCall(
      output().getStatePointFunction(callee_type), statepoint_operands.data(),
      statepoint_operands.size());
  LLVMSetInstructionCallConv(statepoint_ret, LLVMV8CallConv);
  LLVMSetTailCall(statepoint_ret, tailcall);
  if (!tailcall) {
    // 2. rebuild value
    auto& successor_liveins = current_bb_->successors().front()->liveins();
    auto& values = GetImpl(current_bb_)->values();
    for (int livein : successor_liveins) {
      if (livein == id) continue;
      auto found = values.find(livein);
      LValue to_gc = found->second;
      if (typeOf(to_gc) != output().taggedType()) continue;
      LValue relocated = output().buildCall(
          output().repo().gcRelocateIntrinsic(), statepoint_ret,
          output().constInt32(gc_paramter_start),
          output().constInt32(gc_paramter_start));
      found->second = relocated;
      gc_paramter_start++;
    }

    LValue intrinsic = output().repo().gcResultIntrinsic();
    if (call_desc.return_count == 2) {
      intrinsic = output().repo().gcResult2Intrinsic();
    }
    ret = output().buildCall(intrinsic, statepoint_ret);
    GetImpl(current_bb_)->set_value(id, ret);
  }
  // save patch point info
  std::unique_ptr<StackMapInfo> info(
      new CallInfo(std::move(call_operand_resolver.release_location())));
  static_cast<CallInfo*>(info.get())->set_tailcall(tailcall);
  static_cast<CallInfo*>(info.get())->set_code_magic(code_magic);
#if defined(UC_3_0)
  stack_map_info_map_->emplace(patchid, std::move(info));
#else
  stack_map_info_map_->insert(std::make_pair(patchid, std::move(info)));
#endif
}

LValue LLVMTFBuilder::EnsureWord32(LValue v) {
  LType type = typeOf(v);
  LLVMTypeKind kind = LLVMGetTypeKind(type);
  if (kind == LLVMPointerTypeKind) {
    return output().buildCast(LLVMPtrToInt, v, output().repo().int32);
  }
  if (type == output().repo().int1) {
    return output().buildCast(LLVMZExt, v, output().repo().int32);
  }
  EMASSERT(type == output().repo().int32);
  return v;
}

LValue LLVMTFBuilder::EnsurePhiInput(BasicBlock* pred, int index, LType type) {
  LValue val = GetImpl(pred)->value(index);
  LType value_type = typeOf(val);
  if (value_type == type) return val;
  LValue terminator =
      LLVMGetBasicBlockTerminator(GetNativeBBContinuation(pred));
  if ((value_type == output().repo().intPtr) &&
      (type == output().taggedType())) {
    output().positionBefore(terminator);
    LValue ret_val =
        output().buildCast(LLVMIntToPtr, val, output().taggedType());
    return ret_val;
  }
  LLVMTypeKind value_type_kind = LLVMGetTypeKind(value_type);
  if ((LLVMPointerTypeKind == value_type_kind) &&
      (type == output().repo().intPtr)) {
    output().positionBefore(terminator);
    LValue ret_val =
        output().buildCast(LLVMPtrToInt, val, output().repo().intPtr);
    return ret_val;
  }
  if ((value_type == output().repo().int1) &&
      (type == output().repo().intPtr)) {
    output().positionBefore(terminator);
    LValue ret_val = output().buildCast(LLVMZExt, val, output().repo().intPtr);
    return ret_val;
  }
  __builtin_trap();
}

LValue LLVMTFBuilder::EnsurePhiInputAndPosition(BasicBlock* pred, int index,
                                                LType type) {
  LValue value = EnsurePhiInput(pred, index, type);
  output().positionToBBEnd(GetNativeBB(current_bb_));
  return value;
}

void LLVMTFBuilder::EndCurrentBlock() {
  if (current_bb_->successors().empty()) output().buildUnreachable();
  GetImpl(current_bb_)->EndBuild();
  current_bb_ = nullptr;
}

void LLVMTFBuilder::VisitBlock(int id, bool,
                               const OperandsVector& predecessors) {
  BasicBlock* bb = basic_block_manager().findBB(id);
  current_bb_ = bb;
  StartBuild(bb, output());
  MergePredecessors(bb);
}

void LLVMTFBuilder::VisitGoto(int bid) {
  BasicBlock* succ = basic_block_manager().ensureBB(bid);
  EnsureNativeBB(succ, output());
  output().buildBr(GetNativeBB(succ));
  EndCurrentBlock();
}

void LLVMTFBuilder::VisitParameter(int id, int pid) {
  LValue value = output().registerParameter(pid);
  GetImpl(current_bb_)->set_value(id, value);
}

void LLVMTFBuilder::VisitLoadParentFramePointer(int id) {
  LValue fp = output().fp();
  if (basic_block_manager().needs_frame())
    GetImpl(current_bb_)->set_value(id, output().buildLoad(fp));
  else
    GetImpl(current_bb_)
        ->set_value(id, output().buildBitCast(fp, output().repo().ref8));
}

void LLVMTFBuilder::VisitLoadStackPointer(int id) {
  LValue value = output().buildCall(output().repo().stackSaveIntrinsic());
  GetImpl(current_bb_)->set_value(id, value);
}

void LLVMTFBuilder::VisitDebugBreak(int id) {
  char kUdf[] = "udf #0\n";
  char empty[] = "\0";
  output().buildInlineAsm(functionType(output().repo().voidType), kUdf,
                          sizeof(kUdf) - 1, empty, 0, true);
}

void LLVMTFBuilder::VisitInt32Constant(int id, int32_t value) {
  GetImpl(current_bb_)->set_value(id, output().constInt32(value));
}

static LType getMachineRepresentationType(Output& output,
                                          MachineRepresentation rep) {
  LType dstType;
  switch (rep) {
    case MachineRepresentation::kTaggedSigned:
      dstType = output.repo().intPtr;
      break;
    case MachineRepresentation::kTagged:
    case MachineRepresentation::kTaggedPointer:
      dstType = output.taggedType();
      break;
    case MachineRepresentation::kWord8:
      dstType = output.repo().int8;
      break;
    case MachineRepresentation::kWord16:
      dstType = output.repo().int16;
      break;
    case MachineRepresentation::kWord32:
      dstType = output.repo().int32;
      break;
    case MachineRepresentation::kWord64:
      dstType = output.repo().int64;
      break;
    case MachineRepresentation::kFloat32:
      dstType = output.repo().floatType;
      break;
    case MachineRepresentation::kFloat64:
      dstType = output.repo().doubleType;
      break;
    default:
      LLVM_BUILTIN_TRAP;
  }
  return dstType;
}

static LValue buildAccessPointer(Output& output, LValue value, LValue offset,
                                 MachineRepresentation rep) {
  LLVMTypeKind kind = LLVMGetTypeKind(typeOf(value));
  if (kind == LLVMIntegerTypeKind) {
    value = output.buildCast(LLVMIntToPtr, value, output.repo().ref8);
  }
  LValue pointer = output.buildGEPWithByteOffset(
      value, offset, pointerType(getMachineRepresentationType(output, rep)));
  return pointer;
}

void LLVMTFBuilder::VisitLoad(int id, MachineRepresentation rep,
                              MachineSemantic semantic, int base, int offset) {
  LValue pointer =
      buildAccessPointer(output(), GetImpl(current_bb_)->value(base),
                         GetImpl(current_bb_)->value(offset), rep);
  LValue value = output().buildLoad(pointer);
  LType castType = nullptr;
  LLVMOpcode opcode;
  switch (semantic) {
    case MachineSemantic::kUint32:
    case MachineSemantic::kInt32:
      switch (rep) {
        case MachineRepresentation::kWord8:
        case MachineRepresentation::kWord16:
          opcode =
              ((semantic == MachineSemantic::kInt32) ? LLVMSExt : LLVMZExt);
          castType = output().repo().int32;
          break;
        case MachineRepresentation::kWord32:
          break;
        default:
          LLVM_BUILTIN_TRAP;
      }
      break;
    case MachineSemantic::kUint64:
    case MachineSemantic::kInt64:
      switch (rep) {
        case MachineRepresentation::kWord8:
        case MachineRepresentation::kWord16:
        case MachineRepresentation::kWord32:
          opcode =
              ((semantic == MachineSemantic::kInt64) ? LLVMSExt : LLVMZExt);
          castType = output().repo().int64;
          break;
        case MachineRepresentation::kWord64:
          break;
        default:
          LLVM_BUILTIN_TRAP;
      }
    default:
      break;
  }
  if (castType) value = output().buildCast(opcode, value, castType);
  GetImpl(current_bb_)->set_value(id, value);
}

void LLVMTFBuilder::VisitStore(int id, MachineRepresentation rep,
                               WriteBarrierKind barrier, int base, int offset,
                               int value) {
  LValue pointer =
      buildAccessPointer(output(), GetImpl(current_bb_)->value(base),
                         GetImpl(current_bb_)->value(offset), rep);
  LValue llvm_val = GetImpl(current_bb_)->value(value);
  LType value_type = typeOf(llvm_val);
  LType pointer_element_type = getElementType(typeOf(pointer));
  if (pointer_element_type != value_type) {
    EMASSERT(value_type = output().repo().intPtr);
    llvm_val = output().buildCast(LLVMIntToPtr, llvm_val, pointer_element_type);
  }
  LValue val = output().buildStore(llvm_val, pointer);
  // store should not be recorded, whatever.
  GetImpl(current_bb_)->set_value(id, val);
  if (barrier != kNoWriteBarrier) {
    StoreBarrierResolver resolver(current_bb_, output(), stack_map_info_map_,
                                  id, state_point_id_next_++,
                                  basic_block_manager().needs_frame());
    resolver.Resolve(GetImpl(current_bb_)->value(base), pointer, llvm_val,
                     barrier);
  }
}

void LLVMTFBuilder::VisitBitcastWordToTagged(int id, int e) {
  GetImpl(current_bb_)
      ->set_value(
          id, output().buildCast(LLVMIntToPtr, GetImpl(current_bb_)->value(e),
                                 output().taggedType()));
}

void LLVMTFBuilder::VisitChangeInt32ToFloat64(int id, int e) {
  GetImpl(current_bb_)
      ->set_value(id,
                  output().buildCast(LLVMSIToFP, GetImpl(current_bb_)->value(e),
                                     output().repo().doubleType));
}

void LLVMTFBuilder::VisitChangeUint32ToFloat64(int id, int e) {
  GetImpl(current_bb_)
      ->set_value(id,
                  output().buildCast(LLVMUIToFP, GetImpl(current_bb_)->value(e),
                                     output().repo().doubleType));
}

void LLVMTFBuilder::VisitTruncateFloat64ToWord32(int id, int e) {
  GetImpl(current_bb_)
      ->set_value(id,
                  output().buildCast(LLVMFPToUI, GetImpl(current_bb_)->value(e),
                                     output().repo().int32));
}

void LLVMTFBuilder::VisitRoundFloat64ToInt32(int id, int e) {
  GetImpl(current_bb_)
      ->set_value(id,
                  output().buildCast(LLVMFPToSI, GetImpl(current_bb_)->value(e),
                                     output().repo().int32));
}

void LLVMTFBuilder::VisitInt32Add(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildNSWAdd(e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitInt32AddWithOverflow(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildCall(
      output().repo().addWithOverflow32Intrinsic(), e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitInt32Sub(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildNSWSub(e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitInt32SubWithOverflow(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildCall(
      output().repo().subWithOverflow32Intrinsic(), e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitInt32Mul(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildNSWMul(e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitInt32Div(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  e1_value =
      output().buildCast(LLVMSIToFP, e1_value, output().repo().doubleType);
  e2_value =
      output().buildCast(LLVMSIToFP, e2_value, output().repo().doubleType);
  LValue result = output().buildFDiv(e1_value, e2_value);
  result = output().buildCast(LLVMFPToSI, result, output().repo().int32);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitInt32Mod(int id, int e1, int e2) {
  __builtin_unreachable();
}

void LLVMTFBuilder::VisitInt32MulWithOverflow(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildCall(
      output().repo().mulWithOverflow32Intrinsic(), e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitWord32Shl(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildShl(e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitWord32Xor(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildXor(e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitWord32Shr(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildShr(e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitWord32Sar(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildSar(e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitWord32Mul(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildMul(e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitWord32And(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildAnd(e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitWord32Or(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildOr(e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitWord32Equal(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildICmp(LLVMIntEQ, e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitInt32LessThanOrEqual(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildICmp(LLVMIntSLE, e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitUint32LessThanOrEqual(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildICmp(LLVMIntULE, e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitUint32LessThan(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildICmp(LLVMIntULT, e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitInt32LessThan(int id, int e1, int e2) {
  LValue e1_value = EnsureWord32(GetImpl(current_bb_)->value(e1));
  LValue e2_value = EnsureWord32(GetImpl(current_bb_)->value(e2));
  LValue result = output().buildICmp(LLVMIntSLT, e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitBranch(int id, int cmp, int btrue, int bfalse) {
  BasicBlock* bbTrue = basic_block_manager().ensureBB(btrue);
  BasicBlock* bbFalse = basic_block_manager().ensureBB(bfalse);
  EnsureNativeBB(bbTrue, output());
  EnsureNativeBB(bbFalse, output());
  int expected_value = -1;
  if (bbTrue->is_deferred()) {
    if (!bbFalse->is_deferred()) expected_value = 0;
  } else if (bbFalse->is_deferred()) {
    expected_value = 1;
  }
  LValue cmp_val = GetImpl(current_bb_)->value(cmp);
  if (typeOf(cmp_val) == output().repo().intPtr) {
    // need to trunc before continue
    cmp_val = output().buildCast(LLVMTrunc, cmp_val, output().repo().int1);
  }
  if (expected_value != -1) {
    cmp_val = output().buildCall(output().repo().expectIntrinsic(), cmp_val,
                                 output().constInt1(expected_value));
  }
  output().buildCondBr(cmp_val, GetNativeBB(bbTrue), GetNativeBB(bbFalse));
  EndCurrentBlock();
}

void LLVMTFBuilder::VisitSwitch(int id, int val,
                                const OperandsVector& successors) {
  // Last successor must be Default.
  BasicBlock* default_block = basic_block_manager().ensureBB(successors.back());
  LValue cmp_val = GetImpl(current_bb_)->value(val);
  EnsureNativeBB(default_block, output());
  output().buildSwitch(cmp_val, GetNativeBB(default_block),
                       successors.size() - 1);
  EndCurrentBlock();
}

void LLVMTFBuilder::VisitIfValue(int id, int val) {
  BasicBlock* pred = current_bb_->predecessors().front();
  EMASSERT(IsBBEndedToBuild(pred));
  LValue switch_val =
      LLVMGetBasicBlockTerminator(GetNativeBBContinuation(pred));
  LLVMAddCase(switch_val, output().constInt32(val), GetNativeBB(current_bb_));
}

void LLVMTFBuilder::VisitIfDefault(int id) {}

void LLVMTFBuilder::VisitHeapConstant(int id, int64_t magic) {
  char buf[256];
  int len =
      snprintf(buf, 256, "mov $0, #%lld", static_cast<long long>(magic & 0xff));
  char kConstraint[] = "=r";
  LValue value =
      output().buildInlineAsm(functionType(output().taggedType()), buf, len,
                              kConstraint, sizeof(kConstraint) - 1, true);
  int patchid = state_point_id_next_++;
  output().buildCall(output().repo().stackmapIntrinsic(),
                     output().constInt64(patchid), output().repo().int32Zero,
                     value);
  std::unique_ptr<StackMapInfo> info(new HeapConstantInfo(magic));
#if defined(UC_3_0)
  stack_map_info_map_->emplace(patchid, std::move(info));
#else
  stack_map_info_map_->insert(std::make_pair(patchid, std::move(info)));
#endif
  GetImpl(current_bb_)->set_value(id, value);
}

void LLVMTFBuilder::VisitExternalConstant(int id, int64_t magic) {
  char buf[256];
  int len =
      snprintf(buf, 256, "mov $0, #%lld", static_cast<long long>(magic & 0xff));
  char kConstraint[] = "=r";
  LValue value = output().buildInlineAsm(
      functionType(pointerType(output().repo().int8)), buf, len, kConstraint,
      sizeof(kConstraint) - 1, true);
  int patchid = state_point_id_next_++;
  output().buildCall(output().repo().stackmapIntrinsic(),
                     output().constInt64(patchid), output().repo().int32Zero,
                     value);
  std::unique_ptr<StackMapInfo> info(new ExternalReferenceInfo(magic));
#if defined(UC_3_0)
  stack_map_info_map_->emplace(patchid, std::move(info));
#else
  stack_map_info_map_->insert(std::make_pair(patchid, std::move(info)));
#endif

  GetImpl(current_bb_)->set_value(id, value);
}

void LLVMTFBuilder::VisitPhi(int id, MachineRepresentation rep,
                             const OperandsVector& operands) {
  LType phi_type = getMachineRepresentationType(output(), rep);
  LValue phi = output().buildPhi(phi_type);
  auto operands_iterator = operands.cbegin();
  bool should_add_to_tf_phi_worklist = false;
  for (BasicBlock* pred : current_bb_->predecessors()) {
    if (IsBBStartedToBuild(pred)) {
      LValue value =
          EnsurePhiInputAndPosition(pred, (*operands_iterator), phi_type);
      LBasicBlock llvbb_ = GetNativeBBContinuation(pred);
      addIncoming(phi, &value, &llvbb_, 1);
    } else {
      should_add_to_tf_phi_worklist = true;
      LLVMTFBuilderBasicBlockImpl* impl = GetImpl(current_bb_);
      impl->not_merged_phis.emplace_back();
      auto& not_merged_phi = impl->not_merged_phis.back();
      not_merged_phi.phi = phi;
      not_merged_phi.pred = pred;
      not_merged_phi.value = *operands_iterator;
    }
    ++operands_iterator;
  }
  if (should_add_to_tf_phi_worklist)
    phi_rebuild_worklist_.push_back(current_bb_);
  GetImpl(current_bb_)->set_value(id, phi);
}

void LLVMTFBuilder::VisitCall(int id, bool code,
                              const CallDescriptor& call_desc,
                              const OperandsVector& operands) {
  DoCall(id, code, call_desc, operands, false);
}

void LLVMTFBuilder::VisitTailCall(int id, bool code,
                                  const CallDescriptor& call_desc,
                                  const OperandsVector& operands) {
  DoTailCall(id, code, call_desc, operands);
}

void LLVMTFBuilder::VisitRoot(int id, int index) {
  LValue offset = output().buildGEPWithByteOffset(
      output().root(), output().constInt32(index * sizeof(void*)),
      pointerType(output().taggedType()));
  LValue value = output().buildLoad(offset);
  GetImpl(current_bb_)->set_value(id, value);
}

void LLVMTFBuilder::VisitCodeForCall(int id, int64_t magic) {
#if defined(UC_3_0)
  code_uses_map_.emplace(id, magic);
#else
  code_uses_map_.insert(std::make_pair(id, magic));
#endif
}

void LLVMTFBuilder::VisitSmiConstant(int id, void* smi_value) {
  LValue value = output().constTagged(smi_value);
  GetImpl(current_bb_)->set_value(id, value);
}

void LLVMTFBuilder::VisitFloat64Constant(int id, double float_value) {
  LValue value = constReal(output().repo().doubleType, float_value);
  GetImpl(current_bb_)->set_value(id, value);
}

void LLVMTFBuilder::VisitProjection(int id, int e, int index) {
  LValue projection = GetImpl(current_bb_)->value(e);
  LValue value = output().buildExtractValue(projection, index);
  GetImpl(current_bb_)->set_value(id, value);
}

void LLVMTFBuilder::VisitFloat64Add(int id, int e1, int e2) {
  LValue e1_value = GetImpl(current_bb_)->value(e1);
  LValue e2_value = GetImpl(current_bb_)->value(e2);
  LValue result = output().buildFAdd(e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitFloat64Sub(int id, int e1, int e2) {
  LValue e1_value = GetImpl(current_bb_)->value(e1);
  LValue e2_value = GetImpl(current_bb_)->value(e2);
  LValue result = output().buildFSub(e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitFloat64Mul(int id, int e1, int e2) {
  LValue e1_value = GetImpl(current_bb_)->value(e1);
  LValue e2_value = GetImpl(current_bb_)->value(e2);
  LValue result = output().buildFMul(e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitFloat64Div(int id, int e1, int e2) {
  LValue e1_value = GetImpl(current_bb_)->value(e1);
  LValue e2_value = GetImpl(current_bb_)->value(e2);
  LValue result = output().buildFDiv(e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitFloat64Mod(int id, int e1, int e2) {
  __builtin_unreachable();
}

void LLVMTFBuilder::VisitFloat64LessThan(int id, int e1, int e2) {
  LValue e1_value = GetImpl(current_bb_)->value(e1);
  LValue e2_value = GetImpl(current_bb_)->value(e2);
  LValue result = output().buildFCmp(LLVMRealOLT, e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitFloat64LessThanOrEqual(int id, int e1, int e2) {
  LValue e1_value = GetImpl(current_bb_)->value(e1);
  LValue e2_value = GetImpl(current_bb_)->value(e2);
  LValue result = output().buildFCmp(LLVMRealOLE, e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitFloat64Equal(int id, int e1, int e2) {
  LValue e1_value = GetImpl(current_bb_)->value(e1);
  LValue e2_value = GetImpl(current_bb_)->value(e2);
  LValue result = output().buildFCmp(LLVMRealOEQ, e1_value, e2_value);
  GetImpl(current_bb_)->set_value(id, result);
}

void LLVMTFBuilder::VisitFloat64Neg(int id, int e) {
  LValue e_value = GetImpl(current_bb_)->value(e);
  LValue value = output().buildFNeg(e_value);
  GetImpl(current_bb_)->set_value(id, value);
}

void LLVMTFBuilder::VisitFloat64Abs(int id, int e) {
  LValue e_value = GetImpl(current_bb_)->value(e);
  LValue value =
      output().buildCall(output().repo().doubleAbsIntrinsic(), e_value);
  GetImpl(current_bb_)->set_value(id, value);
}

void LLVMTFBuilder::VisitReturn(int id, int pop_count,
                                const OperandsVector& operands) {
  if (operands.size() == 1)
    output().buildReturn(GetImpl(current_bb_)->value(operands[0]),
                         GetImpl(current_bb_)->value(pop_count));
  else
    __builtin_trap();
}
}  // namespace tf_llvm
}  // namespace internal
}  // namespace v8