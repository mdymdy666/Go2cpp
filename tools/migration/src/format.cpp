/**
 * @file format.cpp
 * @brief 迁移快照文件（v3 格式）的序列化、签名与校验实现。
 *
 * 全部写字节、读字节都走本地 `Writer` / `Reader`，不 memcpy 结构体，
 * 从而避免 padding、对齐与编译器差异影响格式稳定性。
 */
#include "migration/format.hpp"

#include "migration/signature.hpp"
#include "migration/symbol_table.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace migration {
namespace {

// ---------------------------------------------------------------------------
// CRC32
// ---------------------------------------------------------------------------
const std::array<std::uint32_t, 256>& CrcTable() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

// ---------------------------------------------------------------------------
// 小端读写
// ---------------------------------------------------------------------------
class Writer {
public:
    void U8(std::uint8_t v) { m_buf.push_back(v); }
    void U16(std::uint16_t v) {
        U8(static_cast<std::uint8_t>(v & 0xFFU));
        U8(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
    }
    void U32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) U8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
    }
    void U64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) U8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
    }
    void I64(std::int64_t v) { U64(static_cast<std::uint64_t>(v)); }
    void Bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        m_buf.insert(m_buf.end(), p, p + size);
    }
    std::vector<std::uint8_t>& data() { return m_buf; }
    const std::vector<std::uint8_t>& data() const { return m_buf; }

private:
    std::vector<std::uint8_t> m_buf;
};

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : m_data(data), m_size(size) {}

    bool U8(std::uint8_t* out) {
        if (!Take(1)) return false;
        *out = m_data[m_pos++];
        return true;
    }
    bool U16(std::uint16_t* out) {
        if (!Take(2)) return false;
        *out = static_cast<std::uint16_t>(m_data[m_pos] | (m_data[m_pos + 1] << 8));
        m_pos += 2;
        return true;
    }
    bool U32(std::uint32_t* out) {
        if (!Take(4)) return false;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(m_data[m_pos + i]) << (8 * i);
        m_pos += 4;
        *out = v;
        return true;
    }
    bool U64(std::uint64_t* out) {
        if (!Take(8)) return false;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(m_data[m_pos + i]) << (8 * i);
        m_pos += 8;
        *out = v;
        return true;
    }
    bool I64(std::int64_t* out) {
        std::uint64_t v = 0;
        if (!U64(&v)) return false;
        *out = static_cast<std::int64_t>(v);
        return true;
    }
    bool Bytes(void* out, std::size_t size) {
        if (!Take(size)) return false;
        std::memcpy(out, m_data + m_pos, size);
        m_pos += size;
        return true;
    }
    std::size_t pos() const { return m_pos; }

private:
    bool Take(std::size_t n) {
        if (m_pos + n > m_size) return false;
        return true;
    }

    const std::uint8_t* m_data;
    std::size_t m_size;
    std::size_t m_pos{0};
};

constexpr std::size_t Align8(std::size_t n) { return (n + 7U) & ~static_cast<std::size_t>(7U); }

std::string ErrnoText(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

// ---------------------------------------------------------------------------
// ELF build-id
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> ReadSelfExecutable() {
    std::vector<std::uint8_t> data;
    const int fd = ::open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return data;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0 ||
        static_cast<std::uint64_t>(st.st_size) > (64ULL << 20)) {
        ::close(fd);
        return data;
    }
    data.resize(static_cast<std::size_t>(st.st_size));
    std::size_t got = 0;
    while (got < data.size()) {
        const ssize_t n = ::read(fd, data.data() + got, data.size() - got);
        if (n <= 0) break;
        got += static_cast<std::size_t>(n);
    }
    ::close(fd);
    data.resize(got);
    return data;
}

std::uint64_t Fnv1a64(const void* data, std::size_t size, std::uint64_t seed = 0xCBF29CE484222325ULL) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = seed;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

std::uint64_t ReadBuildIdFromElf(const std::vector<std::uint8_t>& elf) {
    if (elf.size() < sizeof(Elf64_Ehdr)) return 0;
    Elf64_Ehdr eh{};
    std::memcpy(&eh, elf.data(), sizeof(eh));
    if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) return 0;
    if (eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB) return 0;
    if (eh.e_phoff == 0 || eh.e_phnum == 0) return 0;
    if (eh.e_phentsize != sizeof(Elf64_Phdr)) return 0;
    const std::uint64_t ph_end = eh.e_phoff + static_cast<std::uint64_t>(eh.e_phnum) * sizeof(Elf64_Phdr);
    if (ph_end > elf.size()) return 0;

    for (std::uint16_t i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph{};
        std::memcpy(&ph, elf.data() + eh.e_phoff + static_cast<std::uint64_t>(i) * sizeof(Elf64_Phdr),
                    sizeof(ph));
        if (ph.p_type != PT_NOTE) continue;
        if (ph.p_offset + ph.p_filesz > elf.size()) continue;
        std::uint64_t cursor = ph.p_offset;
        const std::uint64_t end = ph.p_offset + ph.p_filesz;
        while (cursor + sizeof(Elf64_Nhdr) <= end) {
            Elf64_Nhdr nh{};
            std::memcpy(&nh, elf.data() + cursor, sizeof(nh));
            const std::uint64_t name_off = cursor + sizeof(nh);
            const std::uint64_t desc_off = name_off + ((nh.n_namesz + 3U) & ~3U);
            const std::uint64_t desc_size = nh.n_descsz;
            if (desc_off + desc_size > end) break;
            if (nh.n_type == NT_GNU_BUILD_ID && nh.n_namesz == 4 &&
                std::memcmp(elf.data() + name_off, "GNU", 3) == 0) {
                return Fnv1a64(elf.data() + desc_off, desc_size);
            }
            cursor = desc_off + ((desc_size + 3U) & ~3U);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 段表项的编码
// ---------------------------------------------------------------------------
struct SectionRecord {
    std::uint32_t type{0};
    std::uint32_t flags{0};
    std::uint64_t offset{0};
    std::uint64_t size{0};
    std::uint64_t va{0};
    std::uint64_t aux{0};
    std::uint32_t crc{0};
    std::uint32_t reserved{0};
};

void WriteSectionRecord(Writer* w, const SectionRecord& s) {
    w->U32(s.type);
    w->U32(s.flags);
    w->U64(s.offset);
    w->U64(s.size);
    w->U64(s.va);
    w->U64(s.aux);
    w->U32(s.crc);
    w->U32(s.reserved);
}

bool ReadSectionRecord(Reader* r, SectionRecord* s) {
    return r->U32(&s->type) && r->U32(&s->flags) && r->U64(&s->offset) &&
           r->U64(&s->size) && r->U64(&s->va) && r->U64(&s->aux) &&
           r->U32(&s->crc) && r->U32(&s->reserved);
}

struct RawHeader {
    char magic[8]{};
    std::uint32_t version{0};
    std::uint32_t header_size{0};
    std::uint32_t flags{0};
    std::uint32_t section_count{0};
    std::uint16_t arch{0};
    std::uint8_t pointer_size{0};
    std::uint8_t endianness{0};
    std::uint64_t build_id_hash{0};
    std::uint64_t file_size{0};
    std::uint64_t fiber_id{0};
    std::uint64_t stack_va{0};
    std::uint64_t stack_size{0};
    std::uint64_t context_va{0};
    std::uint64_t stack_bytes{0};
    std::uint64_t arena_va{0};
    std::uint64_t arena_size{0};
    std::uint64_t channel_va{0};
    std::uint64_t waiter_va{0};
    std::uint32_t wait_kind{0};
    std::uint32_t crc{0};
    std::uint32_t symbol_epoch{0};
    std::uint64_t stack_base_va{0};
    std::uint64_t symbol_hash{0};
    std::uint32_t symbol_count{0};
    std::uint32_t reloc_count{0};
    std::uint32_t frame_count{0};
    std::uint32_t safepoint_count{0};
    std::uint64_t ctx_offset{0};
    std::uint64_t waiter_offset{0};
    std::uint64_t symbol_table_hash{0};
    std::uint64_t signature_offset{0};
    std::uint32_t handle_count{0};
    std::uint32_t safepoint_schema{0};
    std::uint64_t signature_size{0};
    std::uint64_t reserved5{0};
    std::uint64_t reserved6{0};
};

constexpr std::size_t kCrcOffset = 120;  ///< header 内 CRC 字段偏移

bool DecodeHeader(const std::uint8_t* data, std::size_t size, RawHeader* h) {
    if (size < kHeaderSize) return false;
    Reader r(data, kHeaderSize);
    if (!r.Bytes(h->magic, 8)) return false;
    if (!r.U32(&h->version) || !r.U32(&h->header_size) || !r.U32(&h->flags) ||
        !r.U32(&h->section_count) || !r.U16(&h->arch) || !r.U8(&h->pointer_size) ||
        !r.U8(&h->endianness) || !r.U64(&h->build_id_hash) || !r.U64(&h->file_size) ||
        !r.U64(&h->fiber_id) || !r.U64(&h->stack_va) || !r.U64(&h->stack_size) ||
        !r.U64(&h->context_va) || !r.U64(&h->stack_bytes) || !r.U64(&h->arena_va) ||
        !r.U64(&h->arena_size) || !r.U64(&h->channel_va) || !r.U64(&h->waiter_va) ||
        !r.U32(&h->wait_kind) || !r.U32(&h->crc) || !r.U32(&h->symbol_epoch) ||
        !r.U64(&h->stack_base_va) || !r.U64(&h->symbol_hash) ||
        !r.U32(&h->symbol_count) || !r.U32(&h->reloc_count) ||
        !r.U32(&h->frame_count) || !r.U32(&h->safepoint_count) ||
        !r.U64(&h->ctx_offset) || !r.U64(&h->waiter_offset) ||
        !r.U64(&h->symbol_table_hash) || !r.U64(&h->signature_offset) ||
        !r.U32(&h->handle_count) || !r.U32(&h->safepoint_schema) ||
        !r.U64(&h->signature_size) || !r.U64(&h->reserved5) ||
        !r.U64(&h->reserved6)) {
        return false;
    }
    return r.pos() == kHeaderSize;
}

void EncodeHeader(Writer* w, const RawHeader& h) {
    w->Bytes(h.magic, 8);
    w->U32(h.version);
    w->U32(h.header_size);
    w->U32(h.flags);
    w->U32(h.section_count);
    w->U16(h.arch);
    w->U8(h.pointer_size);
    w->U8(h.endianness);
    w->U64(h.build_id_hash);
    w->U64(h.file_size);
    w->U64(h.fiber_id);
    w->U64(h.stack_va);
    w->U64(h.stack_size);
    w->U64(h.context_va);
    w->U64(h.stack_bytes);
    w->U64(h.arena_va);
    w->U64(h.arena_size);
    w->U64(h.channel_va);
    w->U64(h.waiter_va);
    w->U32(h.wait_kind);
    w->U32(h.crc);
    w->U32(h.symbol_epoch);
    w->U64(h.stack_base_va);
    w->U64(h.symbol_hash);
    w->U32(h.symbol_count);
    w->U32(h.reloc_count);
    w->U32(h.frame_count);
    w->U32(h.safepoint_count);
    w->U64(h.ctx_offset);
    w->U64(h.waiter_offset);
    w->U64(h.symbol_table_hash);
    w->U64(h.signature_offset);
    w->U32(h.handle_count);
    w->U32(h.safepoint_schema);
    w->U64(h.signature_size);
    w->U64(h.reserved5);
    w->U64(h.reserved6);
}

// ---------------------------------------------------------------------------
// 各段内容编码
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> EncodeRegisters(const SavedRegisters& r) {
    Writer w;
    w.U64(r.r12);
    w.U64(r.r13);
    w.U64(r.r14);
    w.U64(r.r15);
    w.U64(r.rbx);
    w.U64(r.rbp);
    w.U64(r.rip);
    return std::move(w.data());
}

bool DecodeRegisters(const std::uint8_t* data, std::size_t size, SavedRegisters* r) {
    Reader rd(data, size);
    return rd.U64(&r->r12) && rd.U64(&r->r13) && rd.U64(&r->r14) && rd.U64(&r->r15) &&
           rd.U64(&r->rbx) && rd.U64(&r->rbp) && rd.U64(&r->rip);
}

std::vector<std::uint8_t> EncodeChannel(const ChannelState& c) {
    Writer w;
    w.U64(c.va);
    w.U64(c.capacity);
    w.U64(c.count);
    w.U64(c.head_offset);
    w.U64(c.tail_offset);
    w.U32(c.closed);
    w.U32(c.reserved);
    for (std::int64_t v : c.buffer) w.I64(v);
    return std::move(w.data());
}

bool DecodeChannel(const std::uint8_t* data, std::size_t size, ChannelState* c) {
    Reader r(data, size);
    if (!r.U64(&c->va) || !r.U64(&c->capacity) || !r.U64(&c->count) ||
        !r.U64(&c->head_offset) || !r.U64(&c->tail_offset) || !r.U32(&c->closed) ||
        !r.U32(&c->reserved)) {
        return false;
    }
    for (std::int64_t& v : c->buffer) {
        if (!r.I64(&v)) return false;
    }
    return true;
}

std::vector<std::uint8_t> EncodeFiber(const FiberStateSnapshot& f,
                                      std::uint32_t fn_symbol_id,
                                      std::uint32_t channel_symbol_id) {
    Writer w;
    w.U64(f.ctx);
    w.U64(f.caller);
    w.U64(f.stack_base);
    w.U64(f.stack_size);
    w.U64(f.top);
    w.U64(f.fiber_id);
    w.U32(f.state);
    w.U32(f.reserved);
    w.U64(f.fn);
    w.U64(f.channel);
    w.U64(f.waiter);
    w.I64(f.result);
    w.U64(f.arg);
    w.U64(f.ctx_offset);
    w.U64(f.waiter_offset);
    // 符号引用：{symbol_id, 0, offset}；id 0 表示无引用。
    w.U32(fn_symbol_id);
    w.U32(0);
    w.I64(f.fn_symbol.offset);
    w.U32(channel_symbol_id);
    w.U32(0);
    w.I64(f.channel_symbol.offset);
    w.U32(f.safepoint_id);
    w.U32(f.safepoint_reserved);
    return std::move(w.data());
}

bool DecodeFiber(const std::uint8_t* data, std::size_t size, FiberStateSnapshot* f,
                 std::uint32_t* fn_symbol_id, std::uint32_t* channel_symbol_id) {
    Reader r(data, size);
    if (!r.U64(&f->ctx) || !r.U64(&f->caller) || !r.U64(&f->stack_base) ||
        !r.U64(&f->stack_size) || !r.U64(&f->top) || !r.U64(&f->fiber_id) ||
        !r.U32(&f->state) || !r.U32(&f->reserved) || !r.U64(&f->fn) ||
        !r.U64(&f->channel) || !r.U64(&f->waiter) || !r.I64(&f->result) ||
        !r.U64(&f->arg) || !r.U64(&f->ctx_offset) || !r.U64(&f->waiter_offset)) {
        return false;
    }
    std::uint32_t pad = 0;
    if (!r.U32(fn_symbol_id) || !r.U32(&pad) || !r.I64(&f->fn_symbol.offset)) return false;
    if (!r.U32(channel_symbol_id) || !r.U32(&pad) ||
        !r.I64(&f->channel_symbol.offset)) {
        return false;
    }
    if (!r.U32(&f->safepoint_id) || !r.U32(&f->safepoint_reserved)) return false;
    return true;
}

std::vector<std::uint8_t> EncodeSymbols(const std::vector<SymbolRecord>& symbols) {
    Writer w;
    w.U32(static_cast<std::uint32_t>(symbols.size()));
    for (const auto& s : symbols) {
        w.U32(s.id);
        w.U32(s.kind);
        w.U64(s.size);
        w.U32(static_cast<std::uint32_t>(s.name.size()));
        w.Bytes(s.name.data(), s.name.size());
        w.U32(static_cast<std::uint32_t>(s.display.size()));
        w.Bytes(s.display.data(), s.display.size());
    }
    return std::move(w.data());
}

bool DecodeSymbols(const std::uint8_t* data, std::size_t size,
                   std::vector<SymbolRecord>* symbols) {
    Reader r(data, size);
    std::uint32_t count = 0;
    if (!r.U32(&count) || count > 4096) return false;
    symbols->clear();
    symbols->reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        SymbolRecord s{};
        std::uint32_t name_len = 0;
        std::uint32_t display_len = 0;
        if (!r.U32(&s.id) || !r.U32(&s.kind) || !r.U64(&s.size) || !r.U32(&name_len)) {
            return false;
        }
        if (name_len > size) return false;
        s.name.resize(name_len);
        if (!r.Bytes(s.name.data(), name_len)) return false;
        if (!r.U32(&display_len) || display_len > size) return false;
        s.display.resize(display_len);
        if (!r.Bytes(s.display.data(), display_len)) return false;
        symbols->push_back(std::move(s));
    }
    return true;
}

std::vector<std::uint8_t> EncodeSymbolManifest(
    const std::vector<std::string>& manifest) {
    Writer w;
    w.U32(static_cast<std::uint32_t>(manifest.size()));
    for (const auto& name : manifest) {
        w.U32(static_cast<std::uint32_t>(name.size()));
        w.Bytes(name.data(), name.size());
    }
    return std::move(w.data());
}

bool DecodeSymbolManifest(const std::uint8_t* data, std::size_t size,
                          std::vector<std::string>* manifest) {
    Reader r(data, size);
    std::uint32_t count = 0;
    if (!r.U32(&count) || count > 65536) return false;
    manifest->clear();
    manifest->reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t name_len = 0;
        if (!r.U32(&name_len) || name_len > 4096 || name_len > size) return false;
        std::string name;
        name.resize(name_len);
        if (!r.Bytes(name.data(), name_len)) return false;
        if (name.empty()) return false;
        manifest->push_back(std::move(name));
    }
    return true;
}

std::vector<std::uint8_t> EncodeRelocations(const std::vector<Relocation>& relocs) {
    Writer w;
    w.U32(static_cast<std::uint32_t>(relocs.size()));
    for (const auto& rel : relocs) {
        w.U32(rel.kind);
        w.U32(rel.target_id);
        w.U64(rel.slot_offset);
        w.I64(rel.addend);
    }
    return std::move(w.data());
}

bool DecodeRelocations(const std::uint8_t* data, std::size_t size,
                       std::vector<Relocation>* relocs) {
    Reader r(data, size);
    std::uint32_t count = 0;
    if (!r.U32(&count) || count > (1U << 20)) return false;
    relocs->clear();
    relocs->reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        Relocation rel{};
        if (!r.U32(&rel.kind) || !r.U32(&rel.target_id) || !r.U64(&rel.slot_offset) ||
            !r.I64(&rel.addend)) {
            return false;
        }
        relocs->push_back(rel);
    }
    return true;
}

std::vector<std::uint8_t> EncodeFrames(const std::vector<FrameRecord>& frames) {
    Writer w;
    w.U32(static_cast<std::uint32_t>(frames.size()));
    for (const auto& frame : frames) {
        w.U64(frame.rbp_offset);
        w.U64(frame.ret_slot_offset);
        w.U32(frame.symbol_id);
        w.U32(0);
        w.I64(frame.ret_offset);
    }
    return std::move(w.data());
}

bool DecodeFrames(const std::uint8_t* data, std::size_t size,
                  std::vector<FrameRecord>* frames) {
    Reader r(data, size);
    std::uint32_t count = 0;
    if (!r.U32(&count) || count > 4096) return false;
    frames->clear();
    frames->reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        FrameRecord frame{};
        std::uint32_t pad = 0;
        if (!r.U64(&frame.rbp_offset) || !r.U64(&frame.ret_slot_offset) ||
            !r.U32(&frame.symbol_id) || !r.U32(&pad) || !r.I64(&frame.ret_offset)) {
            return false;
        }
        frames->push_back(frame);
    }
    return true;
}

std::vector<std::uint8_t> EncodeSafepoints(const std::vector<SafepointSlot>& slots) {
    Writer w;
    w.U32(static_cast<std::uint32_t>(slots.size()));
    for (const auto& slot : slots) {
        w.U32(slot.id);
        w.U32(slot.symbol_id);
        w.U64(slot.slot_offset);
        w.I64(slot.old_offset);
    }
    return std::move(w.data());
}

bool DecodeSafepoints(const std::uint8_t* data, std::size_t size,
                      std::vector<SafepointSlot>* slots) {
    Reader r(data, size);
    std::uint32_t count = 0;
    if (!r.U32(&count) || count > 4096) return false;
    slots->clear();
    slots->reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        SafepointSlot slot{};
        if (!r.U32(&slot.id) || !r.U32(&slot.symbol_id) || !r.U64(&slot.slot_offset) ||
            !r.I64(&slot.old_offset)) {
            return false;
        }
        slots->push_back(slot);
    }
    return true;
}

std::vector<std::uint8_t> EncodeHandles(const std::vector<HandleRecord>& handles) {
    Writer w;
    w.U32(static_cast<std::uint32_t>(handles.size()));
    for (const auto& handle : handles) {
        w.U32(handle.id);
        w.U32(handle.reserved);
        w.U64(handle.old_address);
        w.U64(handle.object_size);
        w.U32(static_cast<std::uint32_t>(handle.type.size()));
        w.Bytes(handle.type.data(), handle.type.size());
        w.U64(static_cast<std::uint64_t>(handle.payload.size()));
        w.Bytes(handle.payload.data(), handle.payload.size());
    }
    return std::move(w.data());
}

bool DecodeHandles(const std::uint8_t* data, std::size_t size,
                   std::vector<HandleRecord>* handles) {
    Reader r(data, size);
    std::uint32_t count = 0;
    if (!r.U32(&count) || count > 4096) return false;
    handles->clear();
    handles->reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        HandleRecord handle{};
        std::uint32_t type_len = 0;
        std::uint64_t payload_len = 0;
        if (!r.U32(&handle.id) || !r.U32(&handle.reserved) ||
            !r.U64(&handle.old_address) || !r.U64(&handle.object_size) ||
            !r.U32(&type_len) || type_len > 256) {
            return false;
        }
        handle.type.resize(type_len);
        if (!r.Bytes(handle.type.data(), type_len)) return false;
        if (!r.U64(&payload_len) || payload_len > (16ULL << 20)) return false;
        handle.payload.resize(static_cast<std::size_t>(payload_len));
        if (!r.Bytes(handle.payload.data(), handle.payload.size())) return false;
        handles->push_back(std::move(handle));
    }
    return true;
}

std::vector<std::uint8_t> EncodeAudit(const PointerAudit& a) {
    Writer w;
    w.U32(a.stack_ptrs);
    w.U32(a.symbol_ptrs);
    w.U32(a.code_ptrs);
    w.U32(a.heap_ptrs);
    w.U32(a.handle_ptrs);
    w.U32(a.other_ptrs);
    w.U32(a.slots);
    return std::move(w.data());
}

bool DecodeAudit(const std::uint8_t* data, std::size_t size, PointerAudit* a) {
    Reader r(data, size);
    return r.U32(&a->stack_ptrs) && r.U32(&a->symbol_ptrs) && r.U32(&a->code_ptrs) &&
           r.U32(&a->heap_ptrs) && r.U32(&a->handle_ptrs) && r.U32(&a->other_ptrs) &&
           r.U32(&a->slots);
}

std::vector<std::uint8_t> EncodeSignature(const SignatureState& signature) {
    Writer w;
    w.Bytes(kSignatureMagic, sizeof(kSignatureMagic));
    w.U32(signature.algorithm);
    w.U32(signature.flags);
    w.Bytes(signature.key_id.data(), signature.key_id.size());
    w.Bytes(signature.mac.data(), signature.mac.size());
    return std::move(w.data());
}

bool DecodeSignature(const std::uint8_t* data, std::size_t size,
                     SignatureState* signature) {
    if (size != kSignaturePayloadSize) return false;
    Reader r(data, size);
    char magic[8] = {};
    if (!r.Bytes(magic, sizeof(magic))) return false;
    if (std::memcmp(magic, kSignatureMagic, sizeof(magic)) != 0) return false;
    if (!r.U32(&signature->algorithm) || !r.U32(&signature->flags)) return false;
    if (!r.Bytes(signature->key_id.data(), signature->key_id.size())) return false;
    if (!r.Bytes(signature->mac.data(), signature->mac.size())) return false;
    signature->present = true;
    return true;
}

/// 在整文件缓冲区里定位签名段 MAC 字段（用于签名计算时置 0）。
bool FindMacOffset(const std::vector<std::uint8_t>& data, std::uint64_t signature_offset,
                   std::size_t* mac_offset) {
    if (signature_offset == 0) return false;
    if (signature_offset + kSignaturePayloadSize > data.size()) return false;
    // 段内布局：magic[8] + alg[4] + flags[4] + key_id[16] + mac[32]
    *mac_offset = static_cast<std::size_t>(signature_offset) + 32;
    return *mac_offset + kSignatureMacSize <= data.size();
}

}  // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
std::uint32_t Crc32(const void* data, std::size_t size, std::uint32_t seed) {
    const auto& table = CrcTable();
    std::uint32_t c = seed ^ 0xFFFFFFFFU;
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) c = table[(c ^ p[i]) & 0xFFU] ^ (c >> 8);
    return c ^ 0xFFFFFFFFU;
}

std::uint64_t SelfBuildIdHash() {
    const auto elf = ReadSelfExecutable();
    if (elf.empty()) return 0;
    const std::uint64_t id = ReadBuildIdFromElf(elf);
    if (id != 0) return id;
    const std::size_t head = elf.size() < 4096 ? elf.size() : 4096;
    return Fnv1a64(elf.data(), head, static_cast<std::uint64_t>(elf.size()));
}

bool WriteSnapshotFile(const std::string& path, const Snapshot& snapshot,
                       std::string* error) {
    if (snapshot.stack.size() != snapshot.stack_bytes) {
        *error = "stack 段长度与 stack_bytes 不一致";
        return false;
    }
    if (snapshot.stack_bytes != 0 && snapshot.stack_va < snapshot.context_va) {
        *error = "context_va 高于 stack_va";
        return false;
    }

    // ---- 签名策略 ----
    SigningKey key;
    std::string key_error;
    const bool key_configured = LoadSigningKey(&key, &key_error);
    if (!key_configured && !key_error.empty()) {
        *error = key_error;
        return false;
    }
    const char* insecure = std::getenv("MIGRATION_INSECURE_NO_SIGNATURE");
    const bool allow_unsigned = insecure != nullptr && std::strcmp(insecure, "1") == 0;
    // 测试开关优先：即使配置了密钥也强制生成未签名快照（负例用）。
    const bool sign = key_configured && !allow_unsigned;
    if (!sign && !allow_unsigned) {
        *error =
            "未配置签名密钥：请设置 MIGRATION_SIGNING_KEY 或 "
            "MIGRATION_SIGNING_KEY_FILE（仅测试可设 MIGRATION_INSECURE_NO_SIGNATURE=1 "
            "生成未签名快照）";
        return false;
    }

    const std::vector<std::uint8_t> stack = snapshot.stack;
    const std::vector<std::uint8_t> regs = EncodeRegisters(snapshot.registers);
    const std::vector<std::uint8_t> channel = EncodeChannel(snapshot.channel);
    const auto find_symbol_id = [&snapshot](const std::string& name) -> std::uint32_t {
        if (name.empty()) return 0;
        for (const auto& sym : snapshot.symbols) {
            if (sym.name == name) return sym.id;
        }
        return 0;
    };
    const std::vector<std::uint8_t> fiber =
        EncodeFiber(snapshot.fiber, find_symbol_id(snapshot.fiber.fn_symbol.name),
                    find_symbol_id(snapshot.fiber.channel_symbol.name));
    const std::vector<std::uint8_t> symbols = EncodeSymbols(snapshot.symbols);
    const std::vector<std::uint8_t> manifest = EncodeSymbolManifest(snapshot.symbol_manifest);
    const std::vector<std::uint8_t> relocs = EncodeRelocations(snapshot.relocations);
    const std::vector<std::uint8_t> frames = EncodeFrames(snapshot.frames);
    const std::vector<std::uint8_t> safepoints = EncodeSafepoints(snapshot.safepoint_slots);
    const std::vector<std::uint8_t> handles = EncodeHandles(snapshot.handles);
    const std::vector<std::uint8_t> audit = EncodeAudit(snapshot.audit);
    const std::string summary = snapshot.summary;
    const std::vector<std::uint8_t> summary_bytes(summary.begin(), summary.end());
    SignatureState signature = snapshot.signature;
    signature.present = sign;
    if (sign) {
        signature.algorithm = kSignatureAlgorithmHmacSha256;
        signature.key_id = key.key_id;
        signature.mac.fill(0);
    }
    const std::vector<std::uint8_t> signature_bytes = EncodeSignature(signature);

    std::uint32_t flags = snapshot.flags & ~static_cast<std::uint32_t>(kFlagSigned);
    if (sign) flags |= kFlagSigned;
    if (!snapshot.handles.empty()) flags |= kFlagHasHandles;
    if (!snapshot.safepoint_slots.empty()) flags |= kFlagHasSafepoints;

    struct Pending {
        SectionRecord rec;
        const std::vector<std::uint8_t>* payload;
    };
    std::vector<Pending> sections;
    sections.push_back({{kSectionStack, 0, 0, stack.size(), snapshot.context_va, 0, 0, 0}, &stack});
    sections.push_back({{kSectionRegisters, 0, 0, regs.size(), snapshot.context_va, 0, 0, 0}, &regs});
    sections.push_back({{kSectionChannel, 0, 0, channel.size(), snapshot.channel_va, 0, 0, 0}, &channel});
    sections.push_back({{kSectionFiberState, 0, 0, fiber.size(), snapshot.context_va, 0, 0, 0}, &fiber});
    sections.push_back({{kSectionSymbols, 0, 0, symbols.size(), 0, 0, 0, 0}, &symbols});
    sections.push_back(
        {{kSectionSymbolManifest, 0, 0, manifest.size(), 0, 0, 0, 0}, &manifest});
    sections.push_back({{kSectionRelocations, 0, 0, relocs.size(), 0, 0, 0, 0}, &relocs});
    sections.push_back({{kSectionFrames, 0, 0, frames.size(), 0, 0, 0, 0}, &frames});
    sections.push_back({{kSectionSafepoints, 0, 0, safepoints.size(), 0, 0, 0, 0}, &safepoints});
    sections.push_back({{kSectionHandles, 0, 0, handles.size(), 0, 0, 0, 0}, &handles});
    sections.push_back({{kSectionPointerAudit, 0, 0, audit.size(), 0, 0, 0, 0}, &audit});
    sections.push_back({{kSectionSummary, 0, 0, summary_bytes.size(), 0, 0, 0, 0}, &summary_bytes});
    if (sign) {
        sections.push_back(
            {{kSectionSignature, 0, 0, signature_bytes.size(), 0, 0, 0, 0}, &signature_bytes});
    }

    std::uint64_t cursor = kHeaderSize + static_cast<std::uint64_t>(sections.size()) * kSectionEntrySize;
    for (auto& p : sections) {
        cursor = Align8(cursor);
        p.rec.offset = cursor;
        p.rec.size = p.payload->size();
        p.rec.crc = Crc32(p.payload->data(), p.payload->size());
        cursor += p.payload->size();
    }
    const std::uint64_t file_size = Align8(cursor);

    RawHeader h{};
    std::memcpy(h.magic, kMagic, sizeof(kMagic));
    h.version = kFormatVersion;
    h.header_size = kHeaderSize;
    h.flags = flags;
    h.section_count = static_cast<std::uint32_t>(sections.size());
    h.arch = kArchX86_64;
    h.pointer_size = 8;
    h.endianness = 0;
    h.build_id_hash = snapshot.build_id_hash != 0 ? snapshot.build_id_hash : SelfBuildIdHash();
    h.file_size = file_size;
    h.fiber_id = snapshot.fiber_id;
    h.stack_va = snapshot.stack_va;
    h.stack_size = snapshot.stack_size;
    h.context_va = snapshot.context_va;
    h.stack_bytes = snapshot.stack_bytes;
    h.arena_va = snapshot.arena_va;
    h.arena_size = snapshot.arena_size;
    h.channel_va = snapshot.channel_va;
    h.waiter_va = snapshot.waiter_va;
    h.wait_kind = snapshot.wait_kind;
    h.crc = 0;
    h.symbol_epoch = snapshot.symbol_epoch;
    h.stack_base_va = snapshot.stack_base_va;
    h.symbol_hash = snapshot.symbol_hash;
    h.symbol_count = static_cast<std::uint32_t>(snapshot.symbols.size());
    h.reloc_count = static_cast<std::uint32_t>(snapshot.relocations.size());
    h.frame_count = static_cast<std::uint32_t>(snapshot.frames.size());
    h.safepoint_count = static_cast<std::uint32_t>(snapshot.safepoint_slots.size());
    h.ctx_offset = snapshot.fiber.ctx_offset;
    h.waiter_offset = snapshot.fiber.waiter_offset;
    h.symbol_table_hash = snapshot.symbol_table_hash;
    h.signature_offset = 0;
    h.handle_count = static_cast<std::uint32_t>(snapshot.handles.size());
    h.safepoint_schema = kSafepointSchema;
    h.signature_size = sign ? kSignaturePayloadSize : 0;
    if (sign) {
        // 签名段总是最后一节。
        h.signature_offset = sections.back().rec.offset;
        if (sections.back().rec.type != kSectionSignature) {
            *error = "内部错误：签名段不在文件末尾";
            return false;
        }
    }

    Writer out;
    EncodeHeader(&out, h);
    for (const auto& p : sections) WriteSectionRecord(&out, p.rec);
    out.data().resize(static_cast<std::size_t>(file_size));
    for (const auto& p : sections) {
        std::memcpy(out.data().data() + p.rec.offset, p.payload->data(), p.payload->size());
    }

    // 1) HMAC：CRC 字段与 MAC 字段都置 0 后覆盖整文件。
    //    先签名后算 CRC —— HMAC 的计算已把 CRC 字段清零，因此随后补写 CRC
    //    不会改变签名覆盖的内容（读端同样把两个字段清零）。
    if (sign) {
        std::vector<std::uint8_t> copy = out.data();
        std::memset(copy.data() + kCrcOffset, 0, 4);
        std::size_t mac_offset = 0;
        if (!FindMacOffset(copy, h.signature_offset, &mac_offset)) {
            *error = "内部错误：签名段布局异常";
            return false;
        }
        std::memset(copy.data() + mac_offset, 0, kSignatureMacSize);
        std::uint8_t mac[kSignatureMacSize];
        HmacSha256(key.bytes.data(), key.bytes.size(), copy.data(), copy.size(), mac);
        std::memcpy(out.data().data() + mac_offset, mac, sizeof(mac));
    }

    // 2) CRC：CRC 字段置 0 后覆盖整文件（含已写好的签名段）。
    std::uint32_t crc = Crc32(out.data().data(), out.data().size());
    std::memcpy(out.data().data() + kCrcOffset, &crc, sizeof(crc));

    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        *error = ErrnoText("打开快照文件失败");
        return false;
    }
    const std::size_t written = std::fwrite(out.data().data(), 1, out.data().size(), f);
    const bool flushed = std::fflush(f) == 0;
    const bool closed = std::fclose(f) == 0;
    if (written != out.data().size() || !flushed || !closed) {
        *error = ErrnoText("写入快照文件失败");
        return false;
    }
    return true;
}

bool ReadSnapshotFile(const std::string& path, Snapshot* out, std::string* error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        *error = ErrnoText("打开快照文件失败");
        return false;
    }
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        *error = "定位快照文件末尾失败";
        return false;
    }
    const long size = std::ftell(f);
    if (size < static_cast<long>(kHeaderSize)) {
        std::fclose(f);
        *error = "快照文件过短";
        return false;
    }
    std::rewind(f);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) {
        *error = "读取快照文件不完整";
        return false;
    }

    RawHeader h{};
    if (!DecodeHeader(data.data(), data.size(), &h)) {
        *error = "header 解码失败";
        return false;
    }
    if (std::memcmp(h.magic, kMagic, sizeof(kMagic)) != 0) {
        *error = "magic 不匹配（不是 G2CMIG03 v3 文件）";
        return false;
    }
    if (h.version != kFormatVersion) {
        *error = "不支持的格式版本 " + std::to_string(h.version) + "（期望 3）";
        return false;
    }
    if (h.header_size != kHeaderSize || h.pointer_size != 8 || h.endianness != 0) {
        *error = "header 尺寸/指针宽度/字节序不受支持";
        return false;
    }
    if (h.arch != kArchX86_64) {
        *error = "arch 不是 x86-64";
        return false;
    }
    if (h.file_size != data.size()) {
        *error = "file_size 与实际长度不一致";
        return false;
    }
    if (h.symbol_epoch != kSymbolEpoch) {
        *error = "符号系统版本不兼容：快照 epoch=" + std::to_string(h.symbol_epoch) +
                 "，当前 epoch=" + std::to_string(kSymbolEpoch);
        return false;
    }
    {
        std::vector<std::uint8_t> copy = data;
        std::memset(copy.data() + kCrcOffset, 0, 4);
        if (h.crc != Crc32(copy.data(), copy.size())) {
            *error = "整文件 CRC 校验失败（文件损坏或被截断）";
            return false;
        }
    }
    if (h.section_count == 0 || h.section_count > 64) {
        *error = "段数量非法";
        return false;
    }
    const std::uint64_t table_end =
        kHeaderSize + static_cast<std::uint64_t>(h.section_count) * kSectionEntrySize;
    if (table_end > data.size()) {
        *error = "段表越过文件末尾";
        return false;
    }

    // ---- 段表 ----
    std::vector<SectionRecord> sections;
    sections.reserve(h.section_count);
    {
        Reader r(data.data() + kHeaderSize, data.size() - kHeaderSize);
        for (std::uint32_t i = 0; i < h.section_count; ++i) {
            SectionRecord rec{};
            if (!ReadSectionRecord(&r, &rec)) {
                *error = "段表解码失败";
                return false;
            }
            if (rec.offset > data.size() || rec.size > data.size() - rec.offset) {
                *error = "段 " + std::to_string(i) + " 越过文件末尾";
                return false;
            }
            if (rec.type == kSectionSignature) {
                // 签名段的 MAC 是最后写入的，段 CRC 记录的是「MAC 置 0」时的
                // 内容；MAC 本身由整文件 HMAC 保护。
                if (rec.size != kSignaturePayloadSize) {
                    *error = "签名段长度非法";
                    return false;
                }
                std::vector<std::uint8_t> copy(
                    data.data() + rec.offset,
                    data.data() + rec.offset + static_cast<std::size_t>(rec.size));
                std::memset(copy.data() + 32, 0, kSignatureMacSize);
                if (Crc32(copy.data(), copy.size()) != rec.crc) {
                    *error = "段 " + std::to_string(i) + " CRC 校验失败";
                    return false;
                }
            } else if (Crc32(data.data() + rec.offset,
                             static_cast<std::size_t>(rec.size)) != rec.crc) {
                *error = "段 " + std::to_string(i) + " CRC 校验失败";
                return false;
            }
            sections.push_back(rec);
        }
    }

    // ---- 签名 ----
    Snapshot snap{};
    const SectionRecord* signature_record = nullptr;
    for (const auto& rec : sections) {
        if (rec.type == kSectionSignature) {
            if (signature_record != nullptr) {
                *error = "存在多个签名段";
                return false;
            }
            signature_record = &rec;
        }
    }
    const bool signed_flag = (h.flags & kFlagSigned) != 0;
    if (!signed_flag) {
        if (signature_record != nullptr) {
            *error = "头部未标记签名，但文件里存在签名段（不一致）";
            return false;
        }
        *error =
            "快照未签名：生产环境拒绝恢复未签名快照。请用设置过 "
            "MIGRATION_SIGNING_KEY[_FILE] 的进程重新 dump，或确认快照来源可信。";
        return false;
    }
    if (signature_record == nullptr) {
        *error = "头部标记了签名，但文件缺少签名段";
        return false;
    }
    if (h.signature_offset != signature_record->offset ||
        h.signature_size != kSignaturePayloadSize ||
        signature_record->size != kSignaturePayloadSize) {
        *error = "签名段偏移/长度与 header 不一致";
        return false;
    }
    if (!DecodeSignature(data.data() + signature_record->offset,
                         static_cast<std::size_t>(signature_record->size),
                         &snap.signature)) {
        *error = "签名段解码失败";
        return false;
    }
    if (snap.signature.algorithm != kSignatureAlgorithmHmacSha256) {
        *error = "不支持的签名算法 id=" + std::to_string(snap.signature.algorithm);
        return false;
    }
    {
        SigningKey key;
        std::string key_error;
        if (!LoadSigningKey(&key, &key_error)) {
            if (!key_error.empty()) {
                *error = key_error;
            } else {
                *error =
                    "未配置签名密钥：无法校验快照签名。请设置 MIGRATION_SIGNING_KEY 或 "
                    "MIGRATION_SIGNING_KEY_FILE。";
            }
            return false;
        }
        std::array<std::uint8_t, kSignatureKeyIdSize> expected{};
        std::uint8_t digest[32];
        Sha256(key.bytes.data(), key.bytes.size(), digest);
        std::memcpy(expected.data(), digest, expected.size());
        if (std::memcmp(expected.data(), snap.signature.key_id.data(),
                        expected.size()) != 0) {
            *error = "签名密钥不匹配：快照 key_id=" +
                     HexEncode(snap.signature.key_id.data(), 8) +
                     "，当前密钥 key_id=" + HexEncode(expected.data(), 8);
            return false;
        }
        std::vector<std::uint8_t> copy = data;
        std::memset(copy.data() + kCrcOffset, 0, 4);
        std::memset(copy.data() + signature_record->offset + 32, 0, kSignatureMacSize);
        std::uint8_t mac[kSignatureMacSize];
        HmacSha256(key.bytes.data(), key.bytes.size(), copy.data(), copy.size(), mac);
        if (std::memcmp(mac, snap.signature.mac.data(), sizeof(mac)) != 0) {
            *error = "签名校验失败：快照内容被篡改，或密钥错误（key_id 相同但 MAC 不符）";
            return false;
        }
        snap.signature.verified = true;
    }

    // ---- 头部字段 ----
    snap.flags = h.flags;
    snap.symbol_epoch = h.symbol_epoch;
    snap.build_id_hash = h.build_id_hash;
    snap.symbol_hash = h.symbol_hash;
    snap.symbol_table_hash = h.symbol_table_hash;
    snap.fiber_id = h.fiber_id;
    snap.stack_va = h.stack_va;
    snap.stack_size = h.stack_size;
    snap.stack_base_va = h.stack_base_va;
    snap.context_va = h.context_va;
    snap.stack_bytes = h.stack_bytes;
    snap.arena_va = h.arena_va;
    snap.arena_size = h.arena_size;
    snap.channel_va = h.channel_va;
    snap.waiter_va = h.waiter_va;
    snap.wait_kind = h.wait_kind;

    bool have_stack = false;
    bool have_regs = false;
    bool have_channel = false;
    bool have_fiber = false;
    bool have_symbols = false;
    bool have_relocs = false;
    for (const auto& rec : sections) {
        const std::uint8_t* payload = data.data() + rec.offset;
        switch (rec.type) {
            case kSectionStack:
                if (rec.size != h.stack_bytes) {
                    *error = "stack 段长度与 header 不一致";
                    return false;
                }
                snap.stack.assign(payload, payload + rec.size);
                have_stack = true;
                break;
            case kSectionRegisters:
                if (!DecodeRegisters(payload, static_cast<std::size_t>(rec.size),
                                     &snap.registers)) {
                    *error = "registers 段解码失败";
                    return false;
                }
                have_regs = true;
                break;
            case kSectionChannel:
                if (!DecodeChannel(payload, static_cast<std::size_t>(rec.size),
                                   &snap.channel)) {
                    *error = "channel 段解码失败";
                    return false;
                }
                have_channel = true;
                break;
            case kSectionFiberState: {
                std::uint32_t fn_id = 0;
                std::uint32_t ch_id = 0;
                if (!DecodeFiber(payload, static_cast<std::size_t>(rec.size), &snap.fiber,
                                 &fn_id, &ch_id)) {
                    *error = "fiber 段解码失败";
                    return false;
                }
                snap.fiber.fn_symbol_id = fn_id;
                snap.fiber.channel_symbol_id = ch_id;
                have_fiber = true;
                break;
            }
            case kSectionSymbols:
                if (!DecodeSymbols(payload, static_cast<std::size_t>(rec.size),
                                   &snap.symbols)) {
                    *error = "symbols 段解码失败";
                    return false;
                }
                have_symbols = true;
                break;
            case kSectionSymbolManifest:
                if (!DecodeSymbolManifest(payload, static_cast<std::size_t>(rec.size),
                                          &snap.symbol_manifest)) {
                    *error = "symbol manifest 段解码失败";
                    return false;
                }
                break;
            case kSectionRelocations:
                if (!DecodeRelocations(payload, static_cast<std::size_t>(rec.size),
                                       &snap.relocations)) {
                    *error = "relocations 段解码失败";
                    return false;
                }
                have_relocs = true;
                break;
            case kSectionFrames:
                if (!DecodeFrames(payload, static_cast<std::size_t>(rec.size), &snap.frames)) {
                    *error = "frames 段解码失败";
                    return false;
                }
                break;
            case kSectionSafepoints:
                if (!DecodeSafepoints(payload, static_cast<std::size_t>(rec.size),
                                      &snap.safepoint_slots)) {
                    *error = "safepoints 段解码失败";
                    return false;
                }
                break;
            case kSectionHandles:
                if (!DecodeHandles(payload, static_cast<std::size_t>(rec.size),
                                   &snap.handles)) {
                    *error = "handles 段解码失败";
                    return false;
                }
                break;
            case kSectionPointerAudit:
                if (!DecodeAudit(payload, static_cast<std::size_t>(rec.size), &snap.audit)) {
                    *error = "pointer audit 段解码失败";
                    return false;
                }
                break;
            case kSectionSummary:
                snap.summary.assign(reinterpret_cast<const char*>(payload),
                                    static_cast<std::size_t>(rec.size));
                break;
            default:
                break;
        }
    }
    if (!have_stack || !have_regs || !have_fiber || !have_symbols || !have_relocs) {
        *error = "缺少必需段（stack/registers/fiber/symbols/relocations）";
        return false;
    }
    if (snap.wait_kind != kWaitNone && !have_channel) {
        *error = "存在等待状态但缺少 channel 段";
        return false;
    }
    if (snap.stack_bytes != 0 &&
        snap.stack_va != snap.context_va + snap.stack_bytes) {
        *error = "stack_va 与 context_va/stack_bytes 不自洽";
        return false;
    }
    if (snap.fiber.ctx != snap.context_va || snap.fiber.top != snap.stack_va ||
        snap.fiber.stack_size != snap.stack_size) {
        *error = "fiber 段与 header 的栈/上下文描述不一致";
        return false;
    }
    if (snap.fiber.state != 2) {
        *error = "快照中的 Fiber 不是挂起状态（state=" +
                 std::to_string(snap.fiber.state) + "），无可恢复的上下文";
        return false;
    }
    if (snap.symbols.size() != h.symbol_count ||
        snap.relocations.size() != h.reloc_count ||
        snap.frames.size() != h.frame_count ||
        snap.safepoint_slots.size() != h.safepoint_count ||
        snap.handles.size() != h.handle_count) {
        *error = "符号/重定位/帧链/挂起点/句柄数量与 header 不一致";
        return false;
    }
    if (h.safepoint_schema != kSafepointSchema) {
        *error = "挂起点段版本不兼容：" + std::to_string(h.safepoint_schema) +
                 "（期望 " + std::to_string(kSafepointSchema) + "）";
        return false;
    }
    // 符号表清单自洽：清单的哈希必须等于头部记录的整表哈希。
    if (!snap.symbol_manifest.empty() &&
        SymbolTable::HashNames(snap.symbol_manifest) != h.symbol_table_hash) {
        *error = "符号表清单与头部 symbol_table_hash 不一致";
        return false;
    }
    // 符号引用必须能在符号目录里解析到名字。
    for (const auto& sym : snap.symbols) {
        if (sym.id == snap.fiber.fn_symbol_id) snap.fiber.fn_symbol.name = sym.name;
        if (sym.id == snap.fiber.channel_symbol_id) {
            snap.fiber.channel_symbol.name = sym.name;
        }
    }
    if (snap.fiber.fn_symbol_id != 0 && snap.fiber.fn_symbol.name.empty()) {
        *error = "fiber 的入口符号 id 在符号目录中不存在";
        return false;
    }
    if (snap.fiber.channel_symbol_id != 0 && snap.fiber.channel_symbol.name.empty()) {
        *error = "fiber 的通道符号 id 在符号目录中不存在";
        return false;
    }
    for (const auto& rel : snap.relocations) {
        switch (rel.kind) {
            case kRelocSymbol: {
                bool found = false;
                for (const auto& sym : snap.symbols) {
                    if (sym.id == rel.target_id) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    *error = "重定位引用了不存在的符号 id " +
                             std::to_string(rel.target_id);
                    return false;
                }
                break;
            }
            case kRelocHandle: {
                bool found = false;
                for (const auto& handle : snap.handles) {
                    if (handle.id == rel.target_id) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    *error = "重定位引用了不存在的句柄 id " +
                             std::to_string(rel.target_id);
                    return false;
                }
                break;
            }
            case kRelocSafepoint: {
                bool found = false;
                for (const auto& slot : snap.safepoint_slots) {
                    if (slot.id == rel.target_id) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    *error = "重定位引用了不存在的挂起点 id " +
                             std::to_string(rel.target_id);
                    return false;
                }
                break;
            }
            case kRelocStack:
                break;
            default:
                *error = "未知的重定位类型 " + std::to_string(rel.kind);
                return false;
        }
    }
    // 挂起点槽位越界检查。
    for (const auto& slot : snap.safepoint_slots) {
        if (slot.id == 0 || slot.slot_offset + 8 > snap.stack_size) {
            *error = "挂起点槽位非法（id=" + std::to_string(slot.id) + "）";
            return false;
        }
    }
    // 句柄 id 唯一、类型非空。
    for (std::size_t i = 0; i < snap.handles.size(); ++i) {
        if (snap.handles[i].id == 0 || snap.handles[i].type.empty()) {
            *error = "句柄表存在非法条目（id=0 或类型为空）";
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (snap.handles[i].id != snap.handles[j].id) continue;
            *error = "句柄 id 重复：" + std::to_string(snap.handles[i].id);
            return false;
        }
    }
    // build-id 校验：快照必须来自同一个可执行文件（SO 版本可以不同，因为
    // 代码地址已符号化；可执行文件不同则栈布局/编译选项都可能不同）。
    {
        const std::uint64_t self_id = SelfBuildIdHash();
        if (h.build_id_hash != 0 && self_id != 0 && h.build_id_hash != self_id) {
            *error = "build-id 不匹配：快照来自另一个可执行文件（代码地址不可复用）";
            return false;
        }
    }
    *out = std::move(snap);
    return true;
}

std::string DescribeSnapshot(const Snapshot& s) {
    char buf[512];
    std::string text;
    std::snprintf(buf, sizeof(buf),
                  "format=v3 fiber_id=%llu wait_kind=%u symbol_epoch=%u "
                  "symbol_hash=0x%llx symbol_table_hash=0x%llx\n",
                  static_cast<unsigned long long>(s.fiber_id), s.wait_kind,
                  s.symbol_epoch, static_cast<unsigned long long>(s.symbol_hash),
                  static_cast<unsigned long long>(s.symbol_table_hash));
    text += buf;
    std::snprintf(buf, sizeof(buf),
                  "stack_old=[0x%llx,0x%llx) size=%llu ctx_off=0x%llx bytes=%llu\n",
                  static_cast<unsigned long long>(s.stack_base_va),
                  static_cast<unsigned long long>(s.stack_va),
                  static_cast<unsigned long long>(s.stack_size),
                  static_cast<unsigned long long>(s.fiber.ctx_offset),
                  static_cast<unsigned long long>(s.stack_bytes));
    text += buf;
    std::snprintf(buf, sizeof(buf), "channel_old=0x%llx waiter_off=0x%llx\n",
                  static_cast<unsigned long long>(s.channel_va),
                  static_cast<unsigned long long>(s.fiber.waiter_offset));
    text += buf;
    std::snprintf(buf, sizeof(buf),
                  "fiber state=%u id=%llu fn=%s+%lld channel=%s+%lld safepoint_id=0x%x\n",
                  s.fiber.state, static_cast<unsigned long long>(s.fiber.fiber_id),
                  s.fiber.fn_symbol.name.c_str(),
                  static_cast<long long>(s.fiber.fn_symbol.offset),
                  s.fiber.channel_symbol.name.c_str(),
                  static_cast<long long>(s.fiber.channel_symbol.offset),
                  s.fiber.safepoint_id);
    text += buf;
    std::snprintf(buf, sizeof(buf),
                  "regs r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx rbx=0x%llx rbp=0x%llx rip=0x%llx\n",
                  static_cast<unsigned long long>(s.registers.r12),
                  static_cast<unsigned long long>(s.registers.r13),
                  static_cast<unsigned long long>(s.registers.r14),
                  static_cast<unsigned long long>(s.registers.r15),
                  static_cast<unsigned long long>(s.registers.rbx),
                  static_cast<unsigned long long>(s.registers.rbp),
                  static_cast<unsigned long long>(s.registers.rip));
    text += buf;
    std::snprintf(buf, sizeof(buf),
                  "audit stack=%u symbol=%u code=%u heap=%u handle=%u other=%u slots=%u\n",
                  s.audit.stack_ptrs, s.audit.symbol_ptrs, s.audit.code_ptrs,
                  s.audit.heap_ptrs, s.audit.handle_ptrs, s.audit.other_ptrs,
                  s.audit.slots);
    text += buf;
    if (s.signature.present) {
        text += "signature=HMAC-SHA256 key_id=" +
                HexEncode(s.signature.key_id.data(), 8) +
                (s.signature.verified ? " verified=yes\n" : " verified=no\n");
    } else {
        text += "signature=未签名\n";
    }
    std::snprintf(buf, sizeof(buf),
                  "symbols=%zu relocs=%zu frames=%zu safepoints=%zu handles=%zu "
                  "symbol_manifest=%zu\n",
                  s.symbols.size(), s.relocations.size(), s.frames.size(),
                  s.safepoint_slots.size(), s.handles.size(),
                  s.symbol_manifest.size());
    text += buf;
    for (const auto& sym : s.symbols) {
        text += "  sym[" + std::to_string(sym.id) + "] " + sym.name;
        if (!sym.display.empty() && sym.display != sym.name) {
            text += " (" + sym.display + ")";
        }
        text += " size=" + std::to_string(sym.size) + " kind=" +
                std::to_string(sym.kind) + "\n";
    }
    for (const auto& frame : s.frames) {
        std::snprintf(buf, sizeof(buf), "  frame rbp_off=0x%llx ret_off=0x%llx sym_id=%u\n",
                      static_cast<unsigned long long>(frame.rbp_offset),
                      static_cast<unsigned long long>(frame.ret_slot_offset),
                      frame.symbol_id);
        text += buf;
    }
    for (const auto& slot : s.safepoint_slots) {
        std::snprintf(buf, sizeof(buf),
                      "  safepoint id=0x%x sym_id=%u slot_off=0x%llx old_offset=0x%llx\n",
                      slot.id, slot.symbol_id,
                      static_cast<unsigned long long>(slot.slot_offset),
                      static_cast<unsigned long long>(slot.old_offset));
        text += buf;
    }
    for (const auto& handle : s.handles) {
        std::snprintf(buf, sizeof(buf),
                      "  handle id=%u type=%s bytes=%zu old_addr=0x%llx object_size=%llu\n",
                      handle.id, handle.type.c_str(), handle.payload.size(),
                      static_cast<unsigned long long>(handle.old_address),
                      static_cast<unsigned long long>(handle.object_size));
        text += buf;
    }
    if (!s.summary.empty()) {
        text += "summary: ";
        text += s.summary;
        text += "\n";
    }
    return text;
}

}  // namespace migration
