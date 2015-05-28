#ifndef STACKMAPS_H
#define STACKMAPS_H
#include <assert.h>
#include <stdint.h>
#include <vector>
#include <unordered_map>
#include <bitset>
namespace jit {

class DataView {
public:
    DataView(const uint8_t* data)
        : m_data(data)
    {
    }
    template <typename T>
    T read(off_t off, bool littenEndian)
    {
        assert(littenEndian == true);
        T t = *reinterpret_cast<const T*>(m_data + off);
        return t;
    }

private:
    const uint8_t* m_data;
};

typedef std::bitset<32> RegisterSet;

class DWARFRegister {
public:
    DWARFRegister()
        : m_dwarfRegNum(-1)
    {
    }

    explicit DWARFRegister(int16_t dwarfRegNum)
        : m_dwarfRegNum(dwarfRegNum)
    {
    }

    int16_t dwarfRegNum() const { return m_dwarfRegNum; }
    uint16_t reg() const { return m_dwarfRegNum; }

private:
    int16_t m_dwarfRegNum;
};
struct StackMaps {
    struct ParseContext {
        unsigned version;
        DataView* view;
        unsigned offset;
    };

    struct Constant {
        int64_t integer;

        void parse(ParseContext&);
    };

    struct StackSize {
        uint64_t functionOffset;
        uint64_t size;

        void parse(ParseContext&);
    };

    struct Location {
        enum Kind : int8_t {
            Unprocessed,
            Register,
            Direct,
            Indirect,
            Constant,
            ConstantIndex
        };

        DWARFRegister dwarfReg;
        uint8_t size;
        Kind kind;
        int32_t offset;

        void parse(ParseContext&);
    };

    // FIXME: Investigate how much memory this takes and possibly prune it from the
    // format we keep around in FTL::JITCode. I suspect that it would be most awesome to
    // have a CompactStackMaps struct that lossily stores only that subset of StackMaps
    // and Record that we actually need for OSR exit.
    // https://bugs.webkit.org/show_bug.cgi?id=130802
    struct LiveOut {
        DWARFRegister dwarfReg;
        uint8_t size;

        void parse(ParseContext&);
    };

    struct Record {
        uint32_t patchpointID;
        uint32_t instructionOffset;
        uint16_t flags;

        std::vector<Location> locations;
        std::vector<LiveOut> liveOuts;

        bool parse(ParseContext&);
        RegisterSet liveOutsSet() const;
        RegisterSet locationSet() const;
        RegisterSet usedRegisterSet() const;
    };

    unsigned version;
    std::vector<StackSize> stackSizes;
    std::vector<Constant> constants;
    std::vector<Record> records;

    bool parse(DataView*); // Returns true on parse success, false on failure. Failure means that LLVM is signaling compile failure to us.

    typedef std::unordered_map<uint32_t, std::vector<Record> > RecordMap;

    RecordMap computeRecordMap() const;

    unsigned stackSize() const;
};
}
#endif /* STACKMAPS_H */