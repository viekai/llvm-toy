// Copyright 2019 UCWeb Co., Ltd.

#ifndef OUTPUT_H
#define OUTPUT_H
#include <string>
#include <unordered_map>
#include <vector>
#include "src/llvm/intrinsic-repository.h"
namespace v8 {
namespace internal {
namespace tf_llvm {
struct CompilerState;
struct RegisterParameter {
  int name;
  LType type;
  RegisterParameter(int _name, LType _type) : name(_name), type(_type) {}
};

using RegisterParameterDesc = std::vector<RegisterParameter>;

static const int kV8CCRegisterParameterCount = 12;
static const int kV8CCMaxStackParameterToReg = 5;
static const int kRootReg = 10;
static const int kFPReg = 11;

class Output {
 public:
  Output(CompilerState& state);
  ~Output();
  void initializeBuild(const RegisterParameterDesc&, bool v8cc, bool is_wasm);
  void initializeFunction(const RegisterParameterDesc&, bool v8cc,
                          bool is_wasm);
  LBasicBlock appendBasicBlock(const char* name = "");
  LBasicBlock appendBasicBlock(LValue function, const char* name = "");
  void positionToBBEnd(LBasicBlock);
  void positionBefore(LValue);
  LValue constInt32(int);
  LValue constIntPtr(intptr_t);
  LValue constInt64(long long);
  LValue constTagged(void*);
  LValue buildStructGEP(LValue structVal, unsigned field);
  LValue buildGEPWithByteOffset(LValue base, LValue offset, LType dstType);
  LValue buildGEP(LValue base, LValue offset);
  LValue buildLoad(LValue toLoad);
  LValue buildStore(LValue val, LValue pointer);
  LValue buildNeg(LValue val);
  LValue buildAdd(LValue lhs, LValue rhs);
  LValue buildFAdd(LValue lhs, LValue rhs);
  LValue buildNSWAdd(LValue lhs, LValue rhs);
  LValue buildSub(LValue lhs, LValue rhs);
  LValue buildFSub(LValue lhs, LValue rhs);
  LValue buildNSWSub(LValue lhs, LValue rhs);
  LValue buildMul(LValue lhs, LValue rhs);
  LValue buildSRem(LValue lhs, LValue rhs);
  LValue buildSDiv(LValue lhs, LValue rhs);
  LValue buildFMul(LValue lhs, LValue rhs);
  LValue buildFDiv(LValue lhs, LValue rhs);
  LValue buildFCmp(LRealPredicate cond, LValue lhs, LValue rhs);
  LValue buildFNeg(LValue input);
  LValue buildNSWMul(LValue lhs, LValue rhs);
  LValue buildShl(LValue lhs, LValue rhs);
  LValue buildShr(LValue lhs, LValue rhs);
  LValue buildSar(LValue lhs, LValue rhs);
  LValue buildAnd(LValue lhs, LValue rhs);
  LValue buildOr(LValue lhs, LValue rhs);
  LValue buildXor(LValue lhs, LValue rhs);
  LValue buildBr(LBasicBlock bb);
  LValue buildSwitch(LValue, LBasicBlock, unsigned);
  LValue buildCondBr(LValue condition, LBasicBlock taken, LBasicBlock notTaken);
  LValue buildRet(LValue ret);
  LValue buildRetVoid(void);
  LValue buildSelect(LValue condition, LValue taken, LValue notTaken);
  LValue buildICmp(LIntPredicate cond, LValue left, LValue right);
  LValue buildPhi(LType type);
  LValue buildAlloca(LType);

  LValue buildCall(LValue function, const LValue* args, unsigned numArgs);

  template <typename VectorType>
  inline LValue buildCall(LValue function, const VectorType& vector) {
    return buildCall(function, vector.begin(), vector.size());
  }
  inline LValue buildCall(LValue function) {
    return buildCall(function, nullptr, 0U);
  }
  inline LValue buildCall(LValue function, LValue arg1) {
    return buildCall(function, &arg1, 1);
  }
  template <typename... Args>
  LValue buildCall(LValue function, LValue arg1, Args... args) {
    LValue argsArray[] = {arg1, args...};
    return buildCall(function, argsArray, sizeof(argsArray) / sizeof(LValue));
  }

  LValue buildInvoke(LValue function, const LValue* args, unsigned numArgs,
                     LBasicBlock then, LBasicBlock exception);

  LValue buildCast(LLVMOpcode Op, LLVMValueRef Val, LLVMTypeRef DestTy);
  LValue buildBitCast(LValue val, LType type);
  LValue buildPointerCast(LValue val, LType type);
  LValue getStatePointFunction(LType callee_type);
  LValue getGCResultFunction(LType return_type);

  LValue buildInlineAsm(LType, char*, size_t, char*, size_t, bool);
  LValue buildLoadMagic(LType, int64_t magic);

  void buildUnreachable();
  LValue buildExtractValue(LValue aggVal, unsigned index);
  LValue buildInsertValue(LValue aggVal, unsigned index, LValue value);
  LValue buildLandingPad();
  void setLineNumber(int linenum);
  void finalize();
  LValue addFunction(const char* name, LType type);
  LType getLLVMTypeFromMachineType(const MachineType& mt);

  inline IntrinsicRepository& repo() { return repo_; }
  inline LBasicBlock prologue() const { return prologue_; }
  inline LType taggedType() const { return repo_.taggedType; }
  inline LValue parameter(int i) { return parameters_[i]; }
  inline LValue root() { return root_; }
  inline LValue fp() { return fp_; }
  inline LValue parent_fp() { return parent_fp_; }
  inline LValue bitcast_space() { return bitcast_space_; }
  inline int stack_parameter_count() const { return stack_parameter_count_; }
  bool is_wasm() const;

 private:
  LValue setInstrDebugLoc(LValue);
  void AddFunctionCommonAttr(LValue function);
  void finalizeDebugInfo();
  CompilerState& state_;
  IntrinsicRepository repo_;
  LBuilder builder_;
  LLVMDIBuilderRef di_builder_;
  LBasicBlock prologue_;
  LValue root_;
  LValue fp_;
  LValue parent_fp_;
  LValue bitcast_space_;
  LLVMMetadataRef subprogram_;
  size_t stack_parameter_count_;
  std::vector<LValue> parameters_;
  std::unordered_map<LType, LValue> gc_function_map_;
};
}  // namespace tf_llvm
}  // namespace internal
}  // namespace v8
#endif /* OUTPUT_H */
