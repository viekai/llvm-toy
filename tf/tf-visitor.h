#ifndef TFVISITOR_H
#define TFVISITOR_H
#include <stdint.h>
#include <vector>
enum class MachineRepresentation {
  kNone,
  kBit,
  kWord8,
  kWord16,
  kWord32,
  kWord64,
  kTaggedSigned,
  kTaggedPointer,
  kTagged,
  // FP representations must be last, and in order of increasing size.
  kFloat32,
  kFloat64,
  kSimd128,
  kFirstFPRepresentation = kFloat32,
  kLastRepresentation = kSimd128
};

enum class MachineSemantic {
  kNone,
  kBool,
  kInt32,
  kUint32,
  kInt64,
  kUint64,
  kNumber,
  kAny
};

enum WriteBarrierKind : uint8_t {
  kNoWriteBarrier,
  kMapWriteBarrier,
  kPointerWriteBarrier,
  kFullWriteBarrier
};

using OperandsVector = std::vector<int>;
using RegistersForOperands = std::vector<int>;

#define INSTRUCTIONS(V)                                                       \
  V(Parameter, (int id, int pid))                                             \
  V(LoadParentFramePointer, (int id))                                         \
  V(Int32Constant, (int id, int32_t value))                                   \
  V(Load, (int id, MachineRepresentation rep, MachineSemantic semantic,       \
           int base, int offset))                                             \
  V(Store, (int id, MachineRepresentation rep, WriteBarrierKind barrier,      \
            int base, int offset, int value))                                 \
  V(BitcastWordToTagged, (int id, int e))                                     \
  V(Int32Add, (int id, int e1, int e2))                                       \
  V(Int32Sub, (int id, int e1, int e2))                                       \
  V(Int32Mul, (int id, int e1, int e2))                                       \
  V(Int32LessThanOrEqual, (int id, int e1, int e2))                           \
  V(Int32LessThan, (int id, int e1, int e2))                                  \
  V(Uint32LessThanOrEqual, (int id, int e1, int e2))                          \
  V(Word32Shl, (int id, int e1, int e2))                                      \
  V(Word32Shr, (int id, int e1, int e2))                                      \
  V(Word32Sar, (int id, int e1, int e2))                                      \
  V(Word32Mul, (int id, int e1, int e2))                                      \
  V(Word32And, (int id, int e1, int e2))                                      \
  V(Word32Equal, (int id, int e1, int e2))                                    \
  V(Branch, (int id, int cmp, int btrue, int bfalse))                         \
  V(HeapConstant, (int id, int64_t magic))                                    \
  V(ExternalConstant, (int id, int64_t magic))                                \
  V(Phi, (int id, MachineRepresentation rep, const OperandsVector& operands)) \
  V(Call,                                                                     \
    (int id, bool code, const RegistersForOperands& registers_for_operands,   \
     const OperandsVector& operands))                                         \
  V(TailCall,                                                                 \
    (int id, bool code, const RegistersForOperands& registers_for_operands,   \
     const OperandsVector& operands))

class TFVisitor {
 public:
  virtual ~TFVisitor() = default;
  virtual void VisitBlock(int id, const OperandsVector& predecessors) = 0;
  virtual void VisitGoto(int bid) = 0;
#define DECL_METHOD(name, signature) virtual void Visit##name signature = 0;

  INSTRUCTIONS(DECL_METHOD)
#undef DECL_METHOD
};
#endif  // TFVISITOR_H