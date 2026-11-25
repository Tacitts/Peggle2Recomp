// Diagnostic for a XEX image: sections, entry point, save/restore helpers,
// and the instruction patterns that precede every bctr (jump table candidates).
//
// Usage: xexdump <Default.xex> [--bctr-samples N] [--dump-image out.bin]
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <file.h>
#include <disasm.h>
#include <image.h>
#include <xbox.h>

#include <unordered_map>
#include <xex.h>
extern std::unordered_map<size_t, const char*> XamExports;
extern std::unordered_map<size_t, const char*> XboxKernelExports;

static uint32_t BE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

struct Pattern { const char* name; std::vector<uint8_t> bytes; };

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: xexdump <xex> [--bctr-samples N] [--dump-image out.bin]\n"); return 1; }
    int samples = 8;
    const char* dumpPath = nullptr;
    for (int i = 2; i < argc; i++)
    {
        if (!strcmp(argv[i], "--bctr-samples") && i + 1 < argc) samples = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-image") && i + 1 < argc) dumpPath = argv[++i];
    }

    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());

    // --imports: list every import record. Function thunks were already patched by
    // Image::ParseImage (nop,nop,nop,blr); variable slots were byte-swapped in place.
    for (int i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "--imports")) continue;
        auto* hdr = reinterpret_cast<const Xex2ImportHeader*>(getOptHeaderPtr(file.data(), XEX_HEADER_IMPORT_LIBRARIES));
        if (!hdr) { printf("no imports\n"); return 0; }
        std::vector<std::string> names;
        const char* str = reinterpret_cast<const char*>(hdr + 1);
        size_t off = 0;
        for (uint32_t k = 0; k < hdr->numImports; k++)
        {
            names.emplace_back(str + off);
            off += ((names.back().size() + 1) + 3) & ~size_t(3);
        }
        auto* lib = (const Xex2ImportLibrary*)((const char*)hdr + sizeof(Xex2ImportHeader) + hdr->sizeOfStringTable);
        for (size_t l = 0; l < names.size(); l++)
        {
            const auto* exports = names[l] == "xam.xex" ? &XamExports : names[l] == "xboxkrnl.exe" ? &XboxKernelExports : nullptr;
            auto* desc = (const Xex2ImportDescriptor*)(lib + 1);
            for (uint32_t k = 0; k < lib->numberOfImports; k++)
            {
                uint32_t slot = desc[k].firstThunk;
                const Section* sec = nullptr;
                for (const auto& s : image.sections) if (slot >= s.base && slot + 4 <= s.base + s.size) sec = &s;
                if (!sec) continue;
                const uint8_t* p = sec->data + (slot - sec->base);
                if (BE32(p) == 0x60000000)
                {
                    auto sym = image.symbols.find(slot);
                    printf("FUNC %s 0x%08X %s\n", names[l].c_str(), slot, sym != image.symbols.end() ? sym->name.c_str() : "?");
                }
                else
                {
                    uint32_t raw; memcpy(&raw, p, 4); // already native-endian after ParseImage
                    uint32_t ordinal = raw & 0xFFFF, type = raw >> 24;
                    const char* nm = "?";
                    if (exports) { auto it = exports->find(ordinal); if (it != exports->end()) nm = it->second; }
                    printf("VAR  %s 0x%08X %s ordinal=0x%X type=%u\n", names[l].c_str(), slot, nm, ordinal, type);
                }
            }
            lib = (const Xex2ImportLibrary*)((const char*)(lib + 1) + lib->numberOfImports * sizeof(Xex2ImportDescriptor));
        }
        return 0;
    }

    // --disasm <hexaddr> <count>: print instructions and exit.
    for (int i = 2; i + 2 < argc; i++)
    {
        if (strcmp(argv[i], "--disasm")) continue;
        size_t a = strtoull(argv[i + 1], nullptr, 16);
        int count = atoi(argv[i + 2]);
        for (int k = 0; k < count; k++, a += 4)
        {
            const Section* sec = nullptr;
            for (const auto& s : image.sections) if (a >= s.base && a + 4 <= s.base + s.size) sec = &s;
            if (!sec) break;
            const void* p = sec->data + (a - sec->base);
            ppc_insn insn;
            ppc::Disassemble(p, a, insn);
            printf("%08zX  %08X  %-8s %s\n", a, BE32((const uint8_t*)p), insn.opcode ? insn.opcode->name : "??", insn.opcode ? insn.op_str : "");
        }
        return 0;
    }
    printf("image base 0x%zX size 0x%X entry 0x%zX\n", image.base, image.size, image.entry_point);
    for (const auto& s : image.sections)
        printf("  section %-8s 0x%08zX size 0x%08X flags %d\n", s.name.c_str(), s.base, s.size, s.flags);

    if (dumpPath && image.data)
    {
        FILE* f = fopen(dumpPath, "wb");
        if (f) { fwrite(image.data.get(), 1, image.size, f); fclose(f); printf("wrote image to %s\n", dumpPath); }
    }

    // Register save/restore helpers (byte patterns from the XenonRecomp README).
    const Pattern pats[] = {
        { "restgprlr_14_address", { 0xe9, 0xc1, 0xff, 0x68 } },
        { "savegprlr_14_address", { 0xf9, 0xc1, 0xff, 0x68 } },
        { "restfpr_14_address",   { 0xc9, 0xcc, 0xff, 0x70 } },
        { "savefpr_14_address",   { 0xd9, 0xcc, 0xff, 0x70 } },
        { "restvmx_14_address",   { 0x39, 0x60, 0xfe, 0xe0, 0x7d, 0xcb, 0x60, 0xce } },
        { "savevmx_14_address",   { 0x39, 0x60, 0xfe, 0xe0, 0x7d, 0xcb, 0x61, 0xce } },
        { "restvmx_64_address",   { 0x39, 0x60, 0xfc, 0x00, 0x10, 0x0b, 0x60, 0xcb } },
        { "savevmx_64_address",   { 0x39, 0x60, 0xfc, 0x00, 0x10, 0x0b, 0x61, 0xcb } },
    };

    std::map<std::string, int> bctrPatterns;
    std::map<std::string, std::vector<size_t>> bctrExamples;
    size_t insnCount = 0, bctrCount = 0;

    for (const auto& s : image.sections)
    {
        if (!(s.flags & SectionFlags_Code)) continue;
        for (const auto& p : pats)
        {
            for (size_t off = 0; off + p.bytes.size() <= s.size; off += 4)
            {
                if (!memcmp(s.data + off, p.bytes.data(), p.bytes.size()))
                {
                    printf("%s = 0x%zX\n", p.name, s.base + off);
                    break;
                }
            }
        }

        const uint32_t* code = (const uint32_t*)s.data;
        size_t n = s.size / 4;
        insnCount += n;
        for (size_t i = 0; i < n; i++)
        {
            if (BE32((const uint8_t*)&code[i]) != 0x4E800420) continue; // bctr
            bctrCount++;
            std::string sig;
            for (int k = 7; k >= 1; k--)
            {
                if (i < (size_t)k) continue;
                ppc_insn insn;
                ppc::Disassemble(&code[i - k], s.base + (i - k) * 4, insn);
                sig += insn.opcode ? insn.opcode->name : "?";
                sig += ' ';
            }
            sig += "bctr";
            bctrPatterns[sig]++;
            auto& ex = bctrExamples[sig];
            if (ex.size() < 3) ex.push_back(s.base + i * 4);
        }
    }

    printf("code instructions: %zu, bctr: %zu, distinct 7-insn prefixes: %zu\n", insnCount, bctrCount, bctrPatterns.size());
    std::vector<std::pair<int, std::string>> sorted;
    for (auto& [k, v] : bctrPatterns) sorted.push_back({ v, k });
    std::sort(sorted.rbegin(), sorted.rend());
    for (int i = 0; i < (int)sorted.size() && i < samples; i++)
    {
        printf("%6d  %s   e.g.", sorted[i].first, sorted[i].second.c_str());
        for (auto a : bctrExamples[sorted[i].second]) printf(" 0x%zX", a);
        printf("\n");
    }
    return 0;
}
