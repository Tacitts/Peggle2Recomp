// Generic jump-table finder for Xbox 360 XEX images.
//
// XenonAnalyse matches four fixed instruction sequences. Peggle 2's compiler
// interleaves nops and reorders lis/rlwinm/addi, so instead this tool:
//   1. finds every `mtctr rX ; ... ; bctr`,
//   2. walks back to the bounds check `cmplwi crN, rIdx, N` + `bgt/ble crN`,
//   3. concretely interprets the straight-line code between them for each
//      index 0..N, reading table bytes from the image, to get each target.
// Output is a XenonRecomp switch-table TOML.
//
// Usage: jumptables <Default.xex> <out.toml>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
#include <file.h>
#include <image.h>
#include <xbox.h>

static uint32_t BE32(const void* p)
{
    auto* b = (const uint8_t*)p;
    return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
}

static uint32_t Mask(uint32_t mb, uint32_t me)
{
    uint32_t m = 0;
    for (uint32_t b = mb;; b = (b + 1) & 31)
    {
        m |= 0x80000000u >> b;
        if (b == me) break;
    }
    return m;
}

struct Insn
{
    uint32_t raw;
    uint32_t op() const { return raw >> 26; }
    uint32_t xop() const { return (raw >> 1) & 0x3FF; }
    uint32_t rD() const { return (raw >> 21) & 31; }
    uint32_t rA() const { return (raw >> 16) & 31; }
    uint32_t rB() const { return (raw >> 11) & 31; }
    int32_t simm() const { return int16_t(raw & 0xFFFF); }
    uint32_t uimm() const { return raw & 0xFFFF; }
};

static bool IsNop(Insn i) { return i.raw == 0x60000000; }
static bool IsMtctr(Insn i) { return i.op() == 31 && i.xop() == 467 && ((i.raw >> 11) & 0x3FF) == 0x120; }
static bool IsBctr(Insn i) { return i.raw == 0x4E800420; }
static bool IsBranch(Insn i) { return i.op() == 16 || i.op() == 18 || (i.op() == 19 && (i.xop() == 16 || i.xop() == 528)); }

// Interpreter state for a straight-line window.
struct State
{
    uint32_t v[32]{};
    bool known[32]{};
};

// Returns false if the instruction is not modelled.
static bool Step(const Image& image, Insn i, State& s, uint32_t* writtenReg)
{
    auto get = [&](uint32_t r, bool& ok) { if (!s.known[r]) ok = false; return s.v[r]; };
    auto set = [&](uint32_t r, uint32_t val, bool ok) { s.v[r] = val; s.known[r] = ok; if (writtenReg) *writtenReg = r; };
    auto load = [&](uint32_t addr, int size, bool& ok) -> uint32_t {
        // Image::Find does no bounds checking; resolve against sections ourselves.
        const uint8_t* p = nullptr;
        for (const auto& sec : image.sections)
            if (addr >= sec.base && uint64_t(addr) + size <= uint64_t(sec.base) + sec.size) { p = sec.data + (addr - sec.base); break; }
        if (!p) { ok = false; return 0; }
        if (size == 4) return BE32(p);
        if (size == 2) return (uint32_t(p[0]) << 8) | p[1];
        return p[0];
    };
    if (writtenReg) *writtenReg = 0xFF;
    bool ok = true;
    switch (i.op())
    {
    case 14: { uint32_t a = i.rA() ? get(i.rA(), ok) : 0; set(i.rD(), a + i.simm(), ok); return true; }          // addi
    case 15: { uint32_t a = i.rA() ? get(i.rA(), ok) : 0; set(i.rD(), a + (uint32_t(i.simm()) << 16), ok); return true; } // addis
    case 24: if (IsNop(i)) return true; set(i.rA(), get(i.rD(), ok) | i.uimm(), ok); return true;                       // ori
    case 21: {                                                                                                          // rlwinm
        uint32_t sh = (i.raw >> 11) & 31, mb = (i.raw >> 6) & 31, me = (i.raw >> 1) & 31;
        uint32_t x = get(i.rD(), ok);
        x = (x << sh) | (sh ? x >> (32 - sh) : 0);
        set(i.rA(), x & Mask(mb, me), ok);
        return true;
    }
    case 32: case 40: case 34: {                                                                                        // lwz/lhz/lbz
        int size = i.op() == 32 ? 4 : i.op() == 40 ? 2 : 1;
        uint32_t a = (i.rA() ? get(i.rA(), ok) : 0) + i.simm();
        uint32_t val = ok ? load(a, size, ok) : 0;
        set(i.rD(), val, ok);
        return true;
    }
    case 31:
        switch (i.xop())
        {
        case 23: case 279: case 87: {                                                                                   // lwzx/lhzx/lbzx
            int size = i.xop() == 23 ? 4 : i.xop() == 279 ? 2 : 1;
            uint32_t a = (i.rA() ? get(i.rA(), ok) : 0) + get(i.rB(), ok);
            uint32_t val = ok ? load(a, size, ok) : 0;
            set(i.rD(), val, ok);
            return true;
        }
        case 266: set(i.rD(), get(i.rA(), ok) + get(i.rB(), ok), ok); return true;                                     // add
        case 40:  set(i.rD(), get(i.rB(), ok) - get(i.rA(), ok), ok); return true;                                     // subf
        case 444: set(i.rA(), get(i.rD(), ok) | get(i.rB(), ok), ok); return true;                                     // or / mr
        case 954: { uint32_t x = get(i.rD(), ok); set(i.rA(), uint32_t(int32_t(int8_t(x))), ok); return true; }         // extsb
        case 922: { uint32_t x = get(i.rD(), ok); set(i.rA(), uint32_t(int32_t(int16_t(x))), ok); return true; }        // extsh
        case 986: { uint32_t x = get(i.rD(), ok); set(i.rA(), x, ok); return true; }                                    // extsw (low 32)
        case 151: case 215: case 407: case 149: case 663: case 727: return true;                                        // stwx/stbx/sthx/stdx/stfsx/stfdx
        }
        break;
    case 36: case 38: case 44: case 52: case 54: return true;                                                           // stw/stb/sth/stfs/stfd
    case 62: if ((i.raw & 3) == 1) { s.known[i.rA()] = false; } return true;                                            // std / stdu
    case 37: case 39: case 45: case 53: case 55: s.known[i.rA()] = false; return true;                                  // update forms
    }
    // Unmodelled, non-branch: conservatively forget whatever it might write.
    if (IsBranch(i)) return false;
    s.known[i.rD()] = false;
    s.known[i.rA()] = false;
    return true;
}

struct Table
{
    uint32_t base, bctr, r, def;
    std::vector<uint32_t> labels;
};

int main(int argc, char** argv)
{
    if (argc < 3) { printf("usage: jumptables <xex> <out.toml>\n"); return 1; }
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());

    std::vector<Table> tables;
    size_t bctrTotal = 0, rejectedNoCmp = 0, rejectedInterp = 0, rejectedRange = 0, rejectedIdx = 0;
    std::vector<uint32_t> unresolvedWithCmp;

    for (const auto& sec : image.sections)
    {
        if (!(sec.flags & SectionFlags_Code)) continue;
        const uint8_t* d = sec.data;
        size_t n = sec.size / 4;
        auto at = [&](size_t k) { return Insn{ BE32(d + k * 4) }; };
        auto addr = [&](size_t k) { return uint32_t(sec.base + k * 4); };

        for (size_t b = 1; b < n; b++)
        {
            if (!IsBctr(at(b))) continue;
            bctrTotal++;

            // mtctr within the previous 4 instructions, nothing but nops between.
            size_t mt = SIZE_MAX;
            for (size_t k = b; k-- > 0 && b - k <= 4;)
            {
                if (IsMtctr(at(k))) { mt = k; break; }
                if (!IsNop(at(k))) break;
            }
            if (mt == SIZE_MAX) continue;

            // Find the bounds branch + cmplwi on the same CR field. The window is generous
            // (64 insns): the compiler can schedule a long run of register setup between the
            // check and the table load (e.g. 0x8248A9D4 -> 0x8248AA38). Any branch inside the
            // window still makes the interpreter reject the candidate.
            std::optional<size_t> cmpK, brK;
            uint32_t crf = 0, idxReg = 0, maxIdx = 0, def = 0;
            bool fallthroughInRange = true;
            size_t windowStart = 0;
            for (size_t k = mt; k-- > 0 && mt - k <= 64;)
            {
                Insn i = at(k);
                if (!brK)
                {
                    uint32_t bo = (i.raw >> 21) & 31, bi = (i.raw >> 16) & 31;
                    bool isBc = i.op() == 16, isBclr = i.op() == 19 && i.xop() == 16;
                    if ((isBc || isBclr) && (bi & 3) == 1 && (bo == 12 || bo == 4))
                    {
                        crf = bi >> 2;
                        if (bo == 12) // bgt / bgtlr : branch out when above -> fallthrough is in range
                        {
                            def = isBc ? addr(k) + (int32_t(int16_t(i.raw & 0xFFFC))) : 0;
                            fallthroughInRange = true;
                        }
                        else          // ble : branch to in-range code, fallthrough is default
                        {
                            fallthroughInRange = false;
                            if (!isBc) break;
                            uint32_t target = addr(k) + int32_t(int16_t(i.raw & 0xFFFC));
                            if (target <= addr(k) || target > addr(mt)) break;
                            windowStart = (target - uint32_t(sec.base)) / 4;
                            def = addr(k) + 4;
                        }
                        brK = k;
                        continue;
                    }
                    // `ble in_range ; b default ; <table code>`: step over the unconditional
                    // branch to the default case. (A real branch inside the table window is
                    // still rejected by the interpreter.)
                    if (i.op() == 18 && !(i.raw & 1)) continue;
                    if (IsBranch(i)) break;
                    continue;
                }
                // cmplwi crf, rA, uimm  (op 10, L=0)
                if (i.op() == 10 && ((i.raw >> 23) & 7) == crf && !((i.raw >> 21) & 1))
                {
                    cmpK = k; idxReg = i.rA(); maxIdx = i.uimm();
                    break;
                }
                if (IsBranch(i)) break;
            }
            // No bounds check: the index may be bounded by a mask instead, e.g.
            //   clrlwi r8,r10,27 ; rlwinm r0,r8,2,0,29 ; ... lwzx ; mtctr ; bctr   (0x828918C8)
            // The masking instruction then plays the compare's role: it defines the index
            // register and the table size (mask + 1).
            if (!cmpK || !brK)
            {
                cmpK.reset();
                for (size_t k = mt; k-- > 0 && mt - k <= 16;)
                {
                    Insn i = at(k);
                    if (IsBranch(i)) break;
                    uint32_t sh = (i.raw >> 11) & 31, mb = (i.raw >> 6) & 31, me = (i.raw >> 1) & 31;
                    if (i.op() == 21 && sh == 0 && me == 31 && mb >= 24)   // clrlwi rA,rS,mb
                    {
                        cmpK = k; brK = k; idxReg = i.rA(); maxIdx = (1u << (32 - mb)) - 1;
                        break;
                    }
                    uint32_t m = i.uimm();
                    if (i.op() == 28 && m && m <= 0xFF && ((m + 1) & m) == 0)   // andi. rA,rS,2^k-1
                    {
                        cmpK = k; brK = k; idxReg = i.rA(); maxIdx = m;
                        break;
                    }
                }
                fallthroughInRange = true;
                def = 0;
            }
            if (!cmpK || !brK || maxIdx > 4096)
            {
                // Unguarded table stored inline right after the bctr, e.g. 0x821C7328:
                //   lis r12,hi ; rlwinm r0,r3,2,0,29 ; addi r12,r12,lo ; lwzx r0,r12,r0 ; mtctr r0 ; bctr
                //   .long case0, case1, ...      (cases start right where the table ends)
                // No bounds check: the size is where the first case begins. The switch register
                // is the rlwinm's source (the unscaled index).
                int idx = -1;
                for (size_t k = mt; k-- > 0 && mt - k <= 8;)
                {
                    Insn i = at(k);
                    if (IsBranch(i)) break;
                    if (i.op() == 21 && ((i.raw >> 11) & 31) == 2 && ((i.raw >> 6) & 31) == 0 && ((i.raw >> 1) & 31) == 29)
                    {
                        idx = int((i.raw >> 21) & 31);
                        break;
                    }
                }
                Table t{};
                if (idx >= 0 && b + 1 < n)
                {
                    uint32_t tableStart = addr(b + 1), firstCase = UINT32_MAX;
                    for (size_t k = b + 1; k < n && k - b <= 512; k++)
                    {
                        uint32_t v = BE32(d + k * 4);
                        if (addr(k) >= firstCase) break;   // reached the case code
                        if ((v & 3) || v <= tableStart || v >= sec.base + sec.size || v - addr(b) > 0x2000) break;
                        t.labels.push_back(v);
                        firstCase = std::min(firstCase, v);
                    }
                    bool exact = !t.labels.empty() && firstCase == tableStart + 4 * uint32_t(t.labels.size());
                    if (exact && t.labels.size() >= 2)
                    {
                        t.base = addr(b);
                        t.bctr = addr(b);
                        t.r = uint32_t(idx);
                        t.def = 0;
                        printf("  inline table at bctr 0x%X: %zu cases (switch on r%d)\n", t.bctr, t.labels.size(), idx);
                        tables.push_back(std::move(t));
                        continue;
                    }
                }
                rejectedNoCmp++;
                continue;
            }
            if (fallthroughInRange) windowStart = *brK + 1;

            // Pre-context: decodable instructions before the cmplwi (hoisted lis etc.).
            size_t pre = *cmpK;
            while (pre > 0 && *cmpK - pre < 8)
            {
                State probe{};
                Insn i = at(pre - 1);
                if (IsBranch(i) || !Step(image, i, probe, nullptr)) break;
                pre--;
            }

            Table t{};
            t.base = addr(windowStart);
            t.bctr = addr(b);
            t.r = idxReg;
            t.def = def;
            bool good = true;
            std::vector<State> states;
            for (uint32_t v = 0; v <= maxIdx && good; v++)
            {
                State s{};
                auto run = [&](size_t from, size_t to) {
                    for (size_t k = from; k < to; k++)
                    {
                        Insn i = at(k);
                        // Pin first: for mask-bounded tables the "compare" and "branch" are the
                        // same masking instruction.
                        if (k == *cmpK) { s.v[idxReg] = v; s.known[idxReg] = true; continue; }
                        if (k == *brK || IsNop(i)) continue;
                        if (!Step(image, i, s, nullptr)) return false;
                    }
                    return true;
                };
                // Index is the value compared, so pin it at the compare.
                if (!run(pre, *cmpK + 1)) { good = false; break; }
                if (!fallthroughInRange)
                {
                    // Skip the default path between the ble and its target.
                    if (!run(*cmpK + 1, *brK)) { good = false; break; }
                }
                if (!run(fallthroughInRange ? *cmpK + 1 : windowStart, mt)) { good = false; break; }
                uint32_t ctrReg = at(mt).rD();
                if (!s.known[ctrReg]) { good = false; break; }
                // Keep the register state at the bctr; the switch register is chosen below.
                states.push_back(s);
                uint32_t target = s.v[ctrReg];
                const Section* tsec = nullptr;
                for (const auto& s2 : image.sections) if (target >= s2.base && target < s2.base + s2.size) tsec = &s2;
                if ((target & 3) || !tsec || !(tsec->flags & SectionFlags_Code) || (target > t.bctr ? target - t.bctr : t.bctr - target) > 0x40000)
                {
                    rejectedRange++;
                    good = false;
                    break;
                }
                t.labels.push_back(target);
            }
            // Switch register: XenonRecomp emits switch(reg) { case 0..N-1 }, so pick a register
            // that holds index + c at the bctr for every index (c = 0 is the usual case; e.g.
            // 0x828968D0 masks r11 to 0..3, then indexes the table with r19 = r11 + 7). Cases
            // below c are unreachable and get the first label as filler.
            if (good)
            {
                uint32_t ctrReg = at(mt).rD();
                auto fits = [&](uint32_t r, uint32_t& c) {
                    for (uint32_t v = 0; v < states.size(); v++)
                    {
                        if (!states[v].known[r]) return false;
                        uint32_t d = states[v].v[r] - v;
                        if (v == 0) c = d;
                        else if (d != c) return false;
                    }
                    return c <= 64;
                };
                int chosen = -1;
                uint32_t offset = 0, c = 0;
                if (fits(idxReg, c) && c == 0) chosen = int(idxReg);
                for (uint32_t r = 3; chosen < 0 && r < 32; r++)
                    if (r != ctrReg && fits(r, c)) { chosen = int(r); offset = c; }
                if (chosen < 0) { rejectedIdx++; good = false; }
                else
                {
                    t.r = uint32_t(chosen);
                    if (offset)
                    {
                        std::vector<uint32_t> padded(offset, t.labels[0]);
                        padded.insert(padded.end(), t.labels.begin(), t.labels.end());
                        t.labels = std::move(padded);
                    }
                }
            }
            if (!good) { if (t.labels.empty()) rejectedInterp++; unresolvedWithCmp.push_back(addr(b)); continue; }
            tables.push_back(std::move(t));
        }
    }

    // Cross-check against .pdata: which tables live in a pdata-described function,
    // and do all their targets fall inside it?
    struct PFn { uint32_t base, size; };
    std::vector<PFn> pfns;
    if (const Section* pd = image.Find(".pdata"))
    {
        for (uint32_t off = 0; off + 8 <= pd->size; off += 8)
        {
            uint32_t b = BE32(pd->data + off), dw = BE32(pd->data + off + 4);
            if (b) pfns.push_back({ b, ((dw >> 8) & 0x3FFFFF) * 4 });
        }
    }
    std::sort(pfns.begin(), pfns.end(), [](auto& a, auto& b) { return a.base < b.base; });
    auto findPfn = [&](uint32_t a) -> const PFn* {
        auto it = std::upper_bound(pfns.begin(), pfns.end(), a, [](uint32_t v, const PFn& p) { return v < p.base; });
        if (it == pfns.begin()) return nullptr;
        --it;
        return a < it->base + it->size ? &*it : nullptr;
    };
    size_t inPdata = 0, inPdataEscaping = 0, notInPdata = 0;
    std::vector<std::pair<uint32_t, uint32_t>> extraFunctions; // for tables outside pdata
    for (const auto& t : tables)
    {
        const PFn* p = findPfn(t.bctr);
        if (p)
        {
            inPdata++;
            bool esc = false;
            for (auto l : t.labels) if (l < p->base || l >= p->base + p->size) esc = true;
            if (esc) { inPdataEscaping++; printf("  pdata fn 0x%X+0x%X: table at bctr 0x%X jumps outside\n", p->base, p->size, t.bctr); }
        }
        else
        {
            notInPdata++;
            // Leaf function with a jump table: XenonRecomp's analyser stops at the
            // bctr. Derive explicit bounds.
            const Section* sec = nullptr;
            for (const auto& s : image.sections) if (t.bctr >= s.base && t.bctr < s.base + s.size) sec = &s;
            auto word = [&](uint32_t a) { return Insn{ BE32(sec->data + (a - sec->base)) }; };
            auto isTerm = [&](Insn i) { return i.raw == 0x4E800020 || i.raw == 0x4E800420 || (i.op() == 18 && !(i.raw & 1)); };
            // Previous function end: nearest pdata function end or terminator going backwards.
            uint32_t start = t.base;
            uint32_t floor = uint32_t(sec->base);
            {
                auto it = std::upper_bound(pfns.begin(), pfns.end(), t.bctr, [](uint32_t v, const PFn& p) { return v < p.base; });
                if (it != pfns.begin()) { --it; floor = std::max(floor, it->base + it->size); }
            }
            auto walkBack = [&](uint32_t from) {
                while (from - 4 >= floor)
                {
                    Insn prev = word(from - 4);
                    if (isTerm(prev) || prev.raw == 0) break;
                    from -= 4;
                }
                return from;
            };
            start = walkBack(start);
            // A terminator just before `start` may be an early return: if the block
            // before it branches forward into [start, bctr], it is the same function.
            for (;;)
            {
                if (start - 4 < floor || word(start - 4).raw == 0) break;
                uint32_t prevBlockEnd = start - 4; // the terminator
                if (prevBlockEnd < floor + 4) break;
                uint32_t prevStart = walkBack(prevBlockEnd);
                bool joins = false;
                for (uint32_t a = prevStart; a <= prevBlockEnd; a += 4)
                {
                    Insn i = word(a);
                    if (i.op() == 16 && !(i.raw & 3))
                    {
                        uint32_t tgt = a + int32_t(int16_t(i.raw & 0xFFFC));
                        if (tgt >= start && tgt <= t.bctr + 4 * uint32_t(t.labels.size()) + 0x400) joins = true;
                    }
                }
                if (!joins || prevStart == start) break;
                start = prevStart;
            }
            // End: extend over table targets and forward branches, stop at a terminator past them.
            uint32_t need = t.bctr + 4;
            for (auto l : t.labels) need = std::max(need, l + 4);
            if (t.def) need = std::max(need, t.def + 4);
            uint32_t ceil = uint32_t(sec->base + sec->size);
            {
                auto it = std::upper_bound(pfns.begin(), pfns.end(), t.bctr, [](uint32_t v, const PFn& p) { return v < p.base; });
                if (it != pfns.end()) ceil = std::min(ceil, it->base);
            }
            uint32_t a = start, end = 0;
            for (; a < ceil; a += 4)
            {
                Insn i = word(a);
                bool inTable = a > t.bctr && a < t.bctr + 4 + 4 * uint32_t(t.labels.size()) && BE32(sec->data + (a - sec->base)) >= sec->base && BE32(sec->data + (a - sec->base)) < ceil;
                if (!inTable && (i.op() == 16 || (i.op() == 18 && !(i.raw & 1))))
                {
                    int32_t disp = i.op() == 16 ? int32_t(int16_t(i.raw & 0xFFFC)) : (int32_t((i.raw & 0x3FFFFFC) << 6) >> 6);
                    uint32_t tgt = a + disp;
                    if (!(i.raw & 2) && tgt > a && tgt < ceil && tgt - a < 0x4000) need = std::max(need, tgt + 4);
                }
                if (a + 4 >= need && isTerm(i) && !inTable) { end = a + 4; break; }
            }
            if (end)
            {
                extraFunctions.push_back({ start, end - start });
                printf("  leaf fn with table: 0x%X size 0x%X (bctr 0x%X)\n", start, end - start, t.bctr);
            }
            else printf("  WARNING: could not bound leaf fn for bctr 0x%X\n", t.bctr);
        }
    }
    printf("tables in pdata functions: %zu (escaping %zu), tables in non-pdata (leaf) functions: %zu\n", inPdata, inPdataEscaping, notInPdata);

    if (argc >= 4)
    {
        std::sort(extraFunctions.begin(), extraFunctions.end());
        extraFunctions.erase(std::unique(extraFunctions.begin(), extraFunctions.end()), extraFunctions.end());
        FILE* ff = fopen(argv[3], "w");
        if (ff)
        {
            fprintf(ff, "# Generated by tools/jumptables: explicit bounds for leaf functions containing jump tables\nfunctions = [\n");
            for (auto& [b, s] : extraFunctions) fprintf(ff, "    { address = 0x%X, size = 0x%X },\n", b, s);
            fprintf(ff, "]\n");
            fclose(ff);
        }
    }

    FILE* f = fopen(argv[2], "w");
    if (!f) { printf("cannot write %s\n", argv[2]); return 1; }
    fprintf(f, "# Generated by tools/jumptables (interpreting bounds-checked mtctr/bctr sequences)\n\n");
    for (const auto& t : tables)
    {
        fprintf(f, "[[switch]]\n# bctr at 0x%X\nbase = 0x%X\nr = %u\ndefault = 0x%X\nlabels = [\n", t.bctr, t.base, t.r, t.def);
        for (auto l : t.labels) fprintf(f, "    0x%X,\n", l);
        fprintf(f, "]\n\n");
    }
    fclose(f);

    printf("bctr total: %zu\njump tables resolved: %zu\n", bctrTotal, tables.size());
    printf("bounds-checked but unresolved: %zu (no-interp %zu, out-of-range %zu, index-clobbered %zu)\n",
        unresolvedWithCmp.size(), rejectedInterp, rejectedRange, rejectedIdx);
    for (size_t i = 0; i < unresolvedWithCmp.size() && i < 20; i++) printf("  unresolved bctr 0x%X\n", unresolvedWithCmp[i]);
    printf("bctr without a bounds check (virtual/indirect calls): %zu\n", bctrTotal - tables.size() - unresolvedWithCmp.size());
    return 0;
}
