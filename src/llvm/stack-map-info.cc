#include "src/llvm/stack-map-info.h"

namespace v8 {
namespace internal {
namespace tf_llvm {
StackMapInfo::StackMapInfo(StackMapInfoType type) : type_(type) {}

HeapConstantInfo::HeapConstantInfo()
    : StackMapInfo(StackMapInfoType::kHeapConstant) {}

HeapConstantLocationInfo::HeapConstantLocationInfo()
    : StackMapInfo(StackMapInfoType::kHeapConstantLocation) {}

ExternalReferenceInfo::ExternalReferenceInfo()
    : StackMapInfo(StackMapInfoType::kExternalReference) {}

ExternalReferenceLocationInfo::ExternalReferenceLocationInfo()
    : StackMapInfo(StackMapInfoType::kExternalReferenceLocation) {}

CallInfo::CallInfo(LocationVector&& locations)
    : StackMapInfo(StackMapInfoType::kCallInfo),
      locations_(std::move(locations)),
      code_magic_(0),
      tailcall_(false) {}

StoreBarrierInfo::StoreBarrierInfo()
    : StackMapInfo(StackMapInfoType::kStoreBarrier) {}
}  // namespace tf_llvm
}  // namespace internal
}  // namespace v8
