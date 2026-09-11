#include "index_cache.hpp"

#include "string_utils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <random>
#include <system_error>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Format
//
// Bumping kFormatVersion invalidates every stored shard, which is the intended
// way to ship a change to any of the structures below.  The static_asserts at
// the bottom of this file exist so that adding a field to one of them fails to
// compile until someone has decided whether it is serialized and bumped this.
constexpr uint32_t kMagic = 0x5849564c;  // "LVIX", little end first
constexpr uint32_t kFormatVersion = 1;
// Written and compared verbatim.  Shards are a local, per-machine cache, so
// numbers are stored in native byte order and a file produced by a differently
// ordered build is simply rejected.
constexpr uint32_t kByteOrderMark = 0x01020304;

// ─────────────────────────────────────────────────────────────────────────────
// Hashing
//
// xxHash64, implemented here rather than taken from slang's detail:: namespace.
// A digest decides whether cached work is reused, so it has to stay stable
// across dependency bumps; slang's is an internal that is free to change and
// would silently turn every shard into a miss.
constexpr uint64_t kPrime1 = 11400714785074694791ULL;
constexpr uint64_t kPrime2 = 14029467366897019727ULL;
constexpr uint64_t kPrime3 = 1609587929392839161ULL;
constexpr uint64_t kPrime4 = 9650029242287828579ULL;
constexpr uint64_t kPrime5 = 2870177450012600261ULL;

uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

uint64_t read64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint32_t read32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint64_t round64(uint64_t acc, uint64_t input) {
    acc += input * kPrime2;
    acc = rotl64(acc, 31);
    return acc * kPrime1;
}

uint64_t merge_round(uint64_t acc, uint64_t val) {
    acc ^= round64(0, val);
    return acc * kPrime1 + kPrime4;
}

uint64_t xxhash64(std::string_view data, uint64_t seed) {
    const auto* p = reinterpret_cast<const uint8_t*>(data.data());
    const auto* const end = p + data.size();
    uint64_t h;

    if (data.size() >= 32) {
        const auto* const limit = end - 32;
        uint64_t v1 = seed + kPrime1 + kPrime2;
        uint64_t v2 = seed + kPrime2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - kPrime1;
        do {
            v1 = round64(v1, read64(p));
            p += 8;
            v2 = round64(v2, read64(p));
            p += 8;
            v3 = round64(v3, read64(p));
            p += 8;
            v4 = round64(v4, read64(p));
            p += 8;
        } while (p <= limit);

        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = merge_round(h, v1);
        h = merge_round(h, v2);
        h = merge_round(h, v3);
        h = merge_round(h, v4);
    }
    else {
        h = seed + kPrime5;
    }

    h += static_cast<uint64_t>(data.size());

    while (p + 8 <= end) {
        h ^= round64(0, read64(p));
        h = rotl64(h, 27) * kPrime1 + kPrime4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= static_cast<uint64_t>(read32(p)) * kPrime1;
        h = rotl64(h, 23) * kPrime2 + kPrime3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<uint64_t>(*p) * kPrime5;
        h = rotl64(h, 11) * kPrime1;
        ++p;
    }

    h ^= h >> 33;
    h *= kPrime2;
    h ^= h >> 29;
    h *= kPrime3;
    h ^= h >> 32;
    return h;
}

// ─────────────────────────────────────────────────────────────────────────────
// Writer
//
// Strings go through a table.  A shard's reference entries carry a `name` and a
// `symbol_debug` each, and both repeat heavily — an OpenTitan-sized project has
// millions of reference entries drawn from tens of thousands of distinct names,
// so writing them inline would make the cache larger than the sources it
// indexes and slower to read than the parse it replaces.
class Writer {
public:
    void u8(uint8_t v) { raw(&v, sizeof(v)); }
    void u32(uint32_t v) { raw(&v, sizeof(v)); }
    void u64(uint64_t v) { raw(&v, sizeof(v)); }
    void i32(int32_t v) { raw(&v, sizeof(v)); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void digest(const IndexCache::Digest& d) {
        u64(d.lo);
        u64(d.hi);
    }

    /// Intern @p s and write its table index.
    void str(std::string_view s) {
        const auto [it, inserted] = string_ids_.try_emplace(std::string(s), strings_.size());
        if (inserted)
            strings_.emplace_back(s);
        u32(static_cast<uint32_t>(it->second));
    }

    template <typename T, typename F> void seq(const std::vector<T>& items, F&& write_one) {
        u32(static_cast<uint32_t>(items.size()));
        for (const auto& item : items)
            write_one(item);
    }

    /// Body first, string table second: interning happens while the body is
    /// written, so the table is only complete once it is done.
    std::string finish(const std::string& header) const {
        std::string table;
        {
            Writer tw;
            tw.u32(static_cast<uint32_t>(strings_.size()));
            for (const auto& s : strings_) {
                tw.u32(static_cast<uint32_t>(s.size()));
                tw.raw(s.data(), s.size());
            }
            table = tw.buffer_;
        }
        std::string out;
        out.reserve(header.size() + table.size() + buffer_.size() + sizeof(uint64_t));
        out += header;
        Writer len;
        len.u64(static_cast<uint64_t>(table.size()));
        out += len.buffer_;
        out += table;
        out += buffer_;
        return out;
    }

    void raw(const void* data, size_t size) {
        buffer_.append(static_cast<const char*>(data), size);
    }

    const std::string& buffer() const { return buffer_; }

private:
    std::string buffer_;
    std::vector<std::string> strings_;
    std::unordered_map<std::string, size_t> string_ids_;
};

/// Every read is bounds-checked and sets a sticky failure flag rather than
/// throwing.  A corrupt or truncated shard has to degrade to a cache miss, and
/// the alternative — trusting a length field read out of a file that may be
/// half-written — is a way to allocate 2^32 strings from a stray byte.
class Reader {
public:
    Reader(std::string_view bytes) : bytes_(bytes) {}

    uint8_t u8() { return read<uint8_t>(); }
    uint32_t u32() { return read<uint32_t>(); }
    uint64_t u64() { return read<uint64_t>(); }
    int32_t i32() { return read<int32_t>(); }
    bool boolean() { return u8() != 0; }
    IndexCache::Digest digest() {
        IndexCache::Digest d;
        d.lo = u64();
        d.hi = u64();
        return d;
    }

    std::string_view str() {
        const auto id = u32();
        if (id >= strings_.size()) {
            failed_ = true;
            return {};
        }
        return strings_[id];
    }

    /// Read a count, then that many elements.  The count is checked against the
    /// bytes remaining before anything is reserved: a element is at least one
    /// byte, so a count larger than what is left cannot be honest.
    template <typename T, typename F> std::vector<T> seq(F&& read_one) {
        std::vector<T> items;
        const auto count = u32();
        if (failed_ || count > remaining())
            return (failed_ = true, items);
        items.reserve(count);
        for (uint32_t i = 0; i < count && !failed_; ++i)
            items.push_back(read_one());
        return items;
    }

    bool read_string_table() {
        const auto table_size = u64();
        if (failed_ || table_size > remaining())
            return false;
        const auto table = bytes_.substr(offset_, table_size);
        offset_ += table_size;

        Reader tr(table);
        const auto count = tr.u32();
        if (tr.failed_ || count > tr.remaining())
            return false;
        strings_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto size = tr.u32();
            if (tr.failed_ || size > tr.remaining())
                return false;
            strings_.push_back(table.substr(tr.offset_, size));
            tr.offset_ += size;
        }
        return true;
    }

    bool failed() const { return failed_; }
    size_t remaining() const { return bytes_.size() - offset_; }

private:
    template <typename T> T read() {
        T v{};
        if (remaining() < sizeof(T)) {
            failed_ = true;
            return v;
        }
        std::memcpy(&v, bytes_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        return v;
    }

    std::string_view bytes_;
    size_t offset_{0};
    std::vector<std::string_view> strings_;
    bool failed_{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// Entry codecs

void write_port(Writer& w, const PortEntry& p) {
    w.str(p.name);
    w.u32(p.file_id);
    w.str(p.direction);
    w.str(p.type);
    w.str(p.decl_type);
    w.str(p.signal_decl_type);
    w.str(p.default_value);
    w.i32(p.line);
    w.i32(p.col);
}

PortEntry read_port(Reader& r) {
    PortEntry p;
    p.name = r.str();
    p.file_id = r.u32();
    p.direction = r.str();
    p.type = r.str();
    p.decl_type = r.str();
    p.signal_decl_type = r.str();
    p.default_value = r.str();
    p.line = r.i32();
    p.col = r.i32();
    return p;
}

void write_modport(Writer& w, const ModportEntry& m) {
    w.str(m.name);
    w.u32(m.file_id);
    w.i32(m.line);
    w.i32(m.col);
}

ModportEntry read_modport(Reader& r) {
    ModportEntry m;
    m.name = r.str();
    m.file_id = r.u32();
    m.line = r.i32();
    m.col = r.i32();
    return m;
}

void write_module(Writer& w, const ModuleEntry& m) {
    w.str(m.name);
    w.u32(m.file_id);
    w.i32(m.line);
    w.i32(m.col);
    w.i32(m.header_semi_line);
    w.i32(m.header_semi_col);
    w.boolean(m.has_port_list);
    w.boolean(m.ansi_port_list);
    w.boolean(m.port_list_has_ports);
    w.i32(m.port_list_close_line);
    w.i32(m.port_list_close_col);
    w.seq(m.ports, [&](const PortEntry& p) { write_port(w, p); });
    w.seq(m.modports, [&](const ModportEntry& mp) { write_modport(w, mp); });
    // port_by_name is rebuilt on load: it is a lookup over `ports`, so storing
    // it would let the file disagree with itself.
}

ModuleEntry read_module(Reader& r) {
    ModuleEntry m;
    m.name = r.str();
    m.file_id = r.u32();
    m.line = r.i32();
    m.col = r.i32();
    m.header_semi_line = r.i32();
    m.header_semi_col = r.i32();
    m.has_port_list = r.boolean();
    m.ansi_port_list = r.boolean();
    m.port_list_has_ports = r.boolean();
    m.port_list_close_line = r.i32();
    m.port_list_close_col = r.i32();
    m.ports = r.seq<PortEntry>([&] { return read_port(r); });
    m.modports = r.seq<ModportEntry>([&] { return read_modport(r); });
    for (size_t i = 0; i < m.ports.size(); ++i)
        m.port_by_name.try_emplace(m.ports[i].name, i);
    return m;
}

void write_connection(Writer& w, const NamedPortConn& c) {
    w.str(c.port_name);
    w.str(c.signal_name);
    w.u32(c.file_id);
    w.i32(c.line);
    w.i32(c.col);
    w.i32(c.hint_col);
}

NamedPortConn read_connection(Reader& r) {
    NamedPortConn c;
    c.port_name = r.str();
    c.signal_name = r.str();
    c.file_id = r.u32();
    c.line = r.i32();
    c.col = r.i32();
    c.hint_col = r.i32();
    return c;
}

void write_instance(Writer& w, const InstanceEntry& i) {
    w.str(i.module_name);
    w.str(i.instance_name);
    w.str(i.parent_module);
    w.u32(i.file_id);
    w.i32(i.line);
    w.i32(i.start_line);
    w.i32(i.end_line);
    w.seq(i.connections, [&](const NamedPortConn& c) { write_connection(w, c); });
}

InstanceEntry read_instance(Reader& r) {
    InstanceEntry i;
    i.module_name = r.str();
    i.instance_name = r.str();
    i.parent_module = r.str();
    i.file_id = r.u32();
    i.line = r.i32();
    i.start_line = r.i32();
    i.end_line = r.i32();
    i.connections = r.seq<NamedPortConn>([&] { return read_connection(r); });
    return i;
}

void write_field(Writer& w, const FieldEntry& f) {
    w.str(f.name);
    w.str(f.type);
    w.u32(f.file_id);
    w.i32(f.line);
    w.i32(f.col);
}

FieldEntry read_field(Reader& r) {
    FieldEntry f;
    f.name = r.str();
    f.type = r.str();
    f.file_id = r.u32();
    f.line = r.i32();
    f.col = r.i32();
    return f;
}

void write_method(Writer& w, const MethodEntry& m) {
    w.str(m.name);
    w.str(m.return_type);
    w.str(m.params);
    w.boolean(m.is_task);
    w.u32(m.file_id);
    w.i32(m.line);
    w.i32(m.col);
}

MethodEntry read_method(Reader& r) {
    MethodEntry m;
    m.name = r.str();
    m.return_type = r.str();
    m.params = r.str();
    m.is_task = r.boolean();
    m.file_id = r.u32();
    m.line = r.i32();
    m.col = r.i32();
    return m;
}

void write_class(Writer& w, const ClassEntry& c) {
    w.str(c.name);
    w.u32(c.file_id);
    w.str(c.base_class);
    w.str(c.parent_scope);
    w.seq(c.fields, [&](const FieldEntry& f) { write_field(w, f); });
    w.seq(c.methods, [&](const MethodEntry& m) { write_method(w, m); });
    w.i32(c.line);
    w.i32(c.col);
}

ClassEntry read_class(Reader& r) {
    ClassEntry c;
    c.name = r.str();
    c.file_id = r.u32();
    c.base_class = r.str();
    c.parent_scope = r.str();
    c.fields = r.seq<FieldEntry>([&] { return read_field(r); });
    c.methods = r.seq<MethodEntry>([&] { return read_method(r); });
    c.line = r.i32();
    c.col = r.i32();
    return c;
}

void write_enum_member(Writer& w, const EnumMemberEntry& e) {
    w.str(e.name);
    w.u32(e.file_id);
    w.i32(e.line);
    w.i32(e.col);
}

EnumMemberEntry read_enum_member(Reader& r) {
    EnumMemberEntry e;
    e.name = r.str();
    e.file_id = r.u32();
    e.line = r.i32();
    e.col = r.i32();
    return e;
}

void write_typedef(Writer& w, const TypedefEntry& t) {
    w.str(t.name);
    w.str(t.resolved);
    w.str(t.parent_scope);
    w.u32(t.file_id);
    w.boolean(t.is_enum);
    w.boolean(t.is_struct);
    w.seq(t.enum_members, [&](const EnumMemberEntry& e) { write_enum_member(w, e); });
    w.seq(t.fields, [&](const FieldEntry& f) { write_field(w, f); });
    w.i32(t.line);
    w.i32(t.col);
}

TypedefEntry read_typedef(Reader& r) {
    TypedefEntry t;
    t.name = r.str();
    t.resolved = r.str();
    t.parent_scope = r.str();
    t.file_id = r.u32();
    t.is_enum = r.boolean();
    t.is_struct = r.boolean();
    t.enum_members = r.seq<EnumMemberEntry>([&] { return read_enum_member(r); });
    t.fields = r.seq<FieldEntry>([&] { return read_field(r); });
    t.line = r.i32();
    t.col = r.i32();
    return t;
}

void write_macro(Writer& w, const MacroEntry& m) {
    w.str(m.name);
    w.u32(m.file_id);
    w.boolean(m.is_function_like);
    w.seq(m.params, [&](const std::string& p) { w.str(p); });
    w.i32(m.line);
}

MacroEntry read_macro(Reader& r) {
    MacroEntry m;
    m.name = r.str();
    m.file_id = r.u32();
    m.is_function_like = r.boolean();
    m.params = r.seq<std::string>([&] { return std::string(r.str()); });
    m.line = r.i32();
    return m;
}

void write_value(Writer& w, const ValueEntry& v) {
    w.str(v.name);
    w.str(v.type);
    w.str(v.kind);
    w.str(v.parent_scope);
    w.str(v.generate_label);
    w.str(v.default_value);
    w.u32(v.file_id);
    w.i32(v.scope_start_line);
    w.i32(v.scope_end_line);
    w.i32(v.line);
    w.i32(v.col);
    w.str(v.signature);
}

ValueEntry read_value(Reader& r) {
    ValueEntry v;
    v.name = r.str();
    v.type = r.str();
    v.kind = r.str();
    v.parent_scope = r.str();
    v.generate_label = r.str();
    v.default_value = r.str();
    v.file_id = r.u32();
    v.scope_start_line = r.i32();
    v.scope_end_line = r.i32();
    v.line = r.i32();
    v.col = r.i32();
    v.signature = r.str();
    return v;
}

void write_import(Writer& w, const ImportEntry& i) {
    w.str(i.package_name);
    w.str(i.symbol_name);
    w.boolean(i.wildcard);
    w.str(i.parent_scope);
    w.u32(i.file_id);
    w.i32(i.start_line);
    w.i32(i.end_line);
}

ImportEntry read_import(Reader& r) {
    ImportEntry i;
    i.package_name = r.str();
    i.symbol_name = r.str();
    i.wildcard = r.boolean();
    i.parent_scope = r.str();
    i.file_id = r.u32();
    i.start_line = r.i32();
    i.end_line = r.i32();
    return i;
}

void write_reference(Writer& w, const ReferenceEntry& e) {
    w.str(e.name);
    w.u32(e.file_id);
    w.u64(e.symbol_id.lo);
    w.u64(e.symbol_id.hi);
    w.str(e.symbol_debug);
    w.i32(e.line);
    w.i32(e.col);
    w.i32(e.end_col);
}

ReferenceEntry read_reference(Reader& r) {
    ReferenceEntry e;
    e.name = r.str();
    e.file_id = r.u32();
    e.symbol_id.lo = r.u64();
    e.symbol_id.hi = r.u64();
    e.symbol_debug = r.str();
    e.line = r.i32();
    e.col = r.i32();
    e.end_col = r.i32();
    return e;
}

void write_scoped_map(Writer& w, const std::unordered_map<std::string, size_t>& map) {
    w.u32(static_cast<uint32_t>(map.size()));
    for (const auto& [key, index] : map) {
        w.str(key);
        w.u32(static_cast<uint32_t>(index));
    }
}

/// Read a scoped-name map, dropping any entry whose index does not address
/// @p bound.  These map into `classes` / `typedefs` / `values`, and an entry
/// pointing past the end of its table would be read as a symbol on the request
/// path.  A shard that disagrees with itself this way is rejected outright by
/// the caller; the bound check here is what makes that detectable.
bool read_scoped_map(Reader& r, size_t bound, std::unordered_map<std::string, size_t>& out) {
    const auto count = r.u32();
    if (r.failed() || count > r.remaining())
        return false;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto key = r.str();
        const auto index = r.u32();
        if (r.failed() || index >= bound)
            return false;
        out.try_emplace(std::string(key), index);
    }
    return true;
}

std::string read_whole_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A field added to any of these without a matching codec update would be
// silently dropped from every cached shard, which surfaces as a symbol that
// resolves before a restart and not after.  Freeze the sizes: a change here is
// a compile error that points at this file, and the fix is to serialize the new
// field and bump kFormatVersion.
static_assert(sizeof(PortEntry) == 208, "PortEntry changed: update codec + kFormatVersion");
static_assert(sizeof(ModportEntry) == 48, "ModportEntry changed: update codec + kFormatVersion");
static_assert(sizeof(ModuleEntry) == 168, "ModuleEntry changed: update codec + kFormatVersion");
static_assert(sizeof(NamedPortConn) == 80, "NamedPortConn changed: update codec + kFormatVersion");
static_assert(sizeof(InstanceEntry) == 136, "InstanceEntry changed: update codec + kFormatVersion");
static_assert(sizeof(FieldEntry) == 80, "FieldEntry changed: update codec + kFormatVersion");
static_assert(sizeof(MethodEntry) == 112, "MethodEntry changed: update codec + kFormatVersion");
static_assert(sizeof(ClassEntry) == 160, "ClassEntry changed: update codec + kFormatVersion");
static_assert(sizeof(EnumMemberEntry) == 48, "EnumMemberEntry changed: update codec + kFormatVersion");
static_assert(sizeof(TypedefEntry) == 160, "TypedefEntry changed: update codec + kFormatVersion");
static_assert(sizeof(MacroEntry) == 72, "MacroEntry changed: update codec + kFormatVersion");
static_assert(sizeof(ValueEntry) == 248, "ValueEntry changed: update codec + kFormatVersion");
static_assert(sizeof(ImportEntry) == 120, "ImportEntry changed: update codec + kFormatVersion");
static_assert(sizeof(ReferenceEntry) == 104, "ReferenceEntry changed: update codec + kFormatVersion");
static_assert(sizeof(SyntaxIndex) == 800, "SyntaxIndex changed: update codec + kFormatVersion");

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

IndexCache::Digest IndexCache::digest_bytes(std::string_view bytes) {
    // Two seeds rather than one hash of two halves: an input that collides
    // under one seed has no reason to collide under the other.
    return Digest{xxhash64(bytes, 0), xxhash64(bytes, 0x9e3779b97f4a7c15ULL)};
}

std::optional<IndexCache::Digest> IndexCache::digest_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad())
        return std::nullopt;
    return digest_bytes(bytes);
}

IndexCache::Digest IndexCache::config_digest(const std::vector<std::string>& defines,
                                             const std::vector<fs::path>& include_dirs) {
    // Order matters to the preprocessor -- a later `define wins, and include
    // directories are searched in order -- so the digest is over the sequence
    // as given, not over a sorted or de-duplicated view of it.  The separator
    // keeps ["AB", "C"] from hashing the same as ["A", "BC"].
    std::string joined;
    for (const auto& define : defines) {
        joined += define;
        joined += '\n';
    }
    joined += "\x1e";
    for (const auto& dir : include_dirs) {
        joined += dir.string();
        joined += '\n';
    }
    return digest_bytes(joined);
}

fs::path IndexCache::directory_for(const fs::path& project_root) {
    return project_root / ".cache" / "lazyverilog" / "index";
}

std::optional<IndexCache> IndexCache::open(const fs::path& project_root) {
    if (project_root.empty())
        return std::nullopt;

    auto directory = directory_for(project_root);
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec || !fs::is_directory(directory, ec))
        return std::nullopt;

    // Same courtesy clangd extends: the cache lives inside the project, so it
    // would otherwise show up in `git status` for everyone who runs the server.
    const auto ignore = directory / ".gitignore";
    if (!fs::exists(ignore, ec)) {
        std::ofstream out(ignore, std::ios::binary);
        if (out)
            out << "# Generated by lazyverilog.\n*\n";
    }
    return IndexCache(std::move(directory));
}

fs::path IndexCache::shard_path(std::string_view uri) const {
    // Basename for a human reading the directory, path hash for uniqueness:
    // two files with the same name in different directories are the normal
    // case in an RTL tree, not an edge case.
    const auto path = path_from_file_uri(std::string(uri));
    auto name = fs::path(path).filename().string();
    if (name.empty())
        name = "shard";

    const auto hash = digest_bytes(uri);
    std::array<char, 17> hex{};
    std::snprintf(hex.data(), hex.size(), "%016llx", static_cast<unsigned long long>(hash.lo));
    return directory_ / (name + "." + std::string(hex.data()) + ".idx");
}

std::optional<IndexCache::Loaded> IndexCache::load(std::string_view uri) const {
    const auto bytes = read_whole_file(shard_path(uri));
    if (bytes.empty())
        return std::nullopt;
    return deserialize_index_shard(bytes);
}

void IndexCache::store(std::string_view uri, const Key& key, const SyntaxIndex& index,
                       bool stands_alone) const {
    const auto final_path = shard_path(uri);

    // Unique temporary name, then rename.  Workers write shards concurrently
    // and a reader may be another process entirely; rename is what makes a
    // shard appear whole or not at all.
    //
    // The name has to be unique across *processes*, not just across this
    // process's workers: two servers on one project (two editors, or an editor
    // and a second window) index it at the same time, and a counter that starts
    // at zero in every process gives them both the same temporary.  Both then
    // open it with trunc and write into one inode, and whichever renames first
    // publishes the interleaving.  A torn shard that still parses is the worst
    // outcome this cache has -- it passes the magic, the version, the string
    // table and every bounds check, and is served as a real index -- so the
    // salt is drawn once per process and mixed into every name.
    static const uint64_t process_salt = [] {
        std::random_device rd;
        return (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
    }();
    static std::atomic<uint64_t> counter{0};
    std::array<char, 33> suffix{};
    std::snprintf(suffix.data(), suffix.size(), "%016llx%016llx",
                  static_cast<unsigned long long>(process_salt),
                  static_cast<unsigned long long>(counter.fetch_add(1, std::memory_order_relaxed)));
    auto temp_path = final_path;
    temp_path += "." + std::string(suffix.data()) + ".tmp";

    {
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out)
            return;
        const auto bytes = serialize_index_shard(key, index, stands_alone);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out)
            return;
    }

    std::error_code ec;
    fs::rename(temp_path, final_path, ec);
    if (ec)
        fs::remove(temp_path, ec);
}

// ─────────────────────────────────────────────────────────────────────────────

std::string serialize_index_shard(const IndexCache::Key& key, const SyntaxIndex& index,
                                  bool stands_alone) {
    std::string header;
    {
        Writer h;
        h.u32(kMagic);
        h.u32(kFormatVersion);
        h.u32(kByteOrderMark);
        header = h.buffer();
    }

    Writer w;
    w.boolean(stands_alone);
    w.digest(key.content);
    w.digest(key.config);
    w.u32(static_cast<uint32_t>(key.dependencies.size()));
    for (const auto& [dep_uri, dep_digest] : key.dependencies) {
        w.str(dep_uri);
        w.digest(dep_digest);
    }

    w.seq(index.source_files, [&](const std::string& s) { w.str(s); });
    w.seq(index.include_dependencies, [&](const std::string& s) { w.str(s); });
    w.seq(index.modules, [&](const ModuleEntry& m) { write_module(w, m); });
    w.seq(index.instances, [&](const InstanceEntry& i) { write_instance(w, i); });

    w.u32(static_cast<uint32_t>(index.interface_names.size()));
    for (const auto& name : index.interface_names)
        w.str(name);
    w.u32(static_cast<uint32_t>(index.package_names.size()));
    for (const auto& name : index.package_names)
        w.str(name);
    w.u32(static_cast<uint32_t>(index.package_symbols.size()));
    for (const auto& [package, symbols] : index.package_symbols) {
        w.str(package);
        w.seq(symbols, [&](const std::string& s) { w.str(s); });
    }

    w.seq(index.classes, [&](const ClassEntry& c) { write_class(w, c); });
    w.seq(index.typedefs, [&](const TypedefEntry& t) { write_typedef(w, t); });
    w.seq(index.macros, [&](const MacroEntry& m) { write_macro(w, m); });
    w.seq(index.values, [&](const ValueEntry& v) { write_value(w, v); });
    w.seq(index.imports, [&](const ImportEntry& i) { write_import(w, i); });
    w.seq(index.references, [&](const ReferenceEntry& e) { write_reference(w, e); });

    // The scoped-lookup maps are stored, not re-derived.  Deriving them needs
    // the whole-tree knowledge the build had -- which scopes are packages, and
    // which class owns a typedef -- and a shard on its own does not have it.
    // See SyntaxIndex::split_by_source_file(), which carries them for the same
    // reason.
    write_scoped_map(w, index.package_value_by_scoped_name);
    write_scoped_map(w, index.package_type_by_scoped_name);
    write_scoped_map(w, index.package_class_by_scoped_name);

    return w.finish(header);
}

std::optional<IndexCache::Loaded> deserialize_index_shard(std::string_view bytes) {
    Reader r(bytes);
    if (r.u32() != kMagic || r.u32() != kFormatVersion || r.u32() != kByteOrderMark)
        return std::nullopt;
    if (!r.read_string_table())
        return std::nullopt;

    IndexCache::Loaded loaded;
    auto& key = loaded.key;
    auto& index = loaded.index;

    loaded.stands_alone = r.boolean();
    key.content = r.digest();
    key.config = r.digest();
    const auto dep_count = r.u32();
    if (r.failed() || dep_count > r.remaining())
        return std::nullopt;
    key.dependencies.reserve(dep_count);
    for (uint32_t i = 0; i < dep_count; ++i) {
        auto dep_uri = std::string(r.str());
        key.dependencies.emplace_back(std::move(dep_uri), r.digest());
    }

    index.source_files = r.seq<std::string>([&] { return std::string(r.str()); });
    index.include_dependencies = r.seq<std::string>([&] { return std::string(r.str()); });
    index.modules = r.seq<ModuleEntry>([&] { return read_module(r); });
    index.instances = r.seq<InstanceEntry>([&] { return read_instance(r); });

    const auto read_name_set = [&](std::unordered_set<std::string>& out) {
        const auto count = r.u32();
        if (r.failed() || count > r.remaining())
            return false;
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
            out.insert(std::string(r.str()));
        return !r.failed();
    };
    if (!read_name_set(index.interface_names) || !read_name_set(index.package_names))
        return std::nullopt;

    const auto package_count = r.u32();
    if (r.failed() || package_count > r.remaining())
        return std::nullopt;
    for (uint32_t i = 0; i < package_count; ++i) {
        auto package = std::string(r.str());
        auto symbols = r.seq<std::string>([&] { return std::string(r.str()); });
        index.package_symbols.emplace(std::move(package), std::move(symbols));
    }

    index.classes = r.seq<ClassEntry>([&] { return read_class(r); });
    index.typedefs = r.seq<TypedefEntry>([&] { return read_typedef(r); });
    index.macros = r.seq<MacroEntry>([&] { return read_macro(r); });
    index.values = r.seq<ValueEntry>([&] { return read_value(r); });
    index.imports = r.seq<ImportEntry>([&] { return read_import(r); });
    index.references = r.seq<ReferenceEntry>([&] { return read_reference(r); });

    if (!read_scoped_map(r, index.values.size(), index.package_value_by_scoped_name) ||
        !read_scoped_map(r, index.typedefs.size(), index.package_type_by_scoped_name) ||
        !read_scoped_map(r, index.classes.size(), index.package_class_by_scoped_name))
        return std::nullopt;

    if (r.failed())
        return std::nullopt;

    // Rebuild what a build derives rather than records.  Same order and same
    // first-wins rule the build uses, so a loaded shard and a freshly built one
    // answer identically.
    index.source_file_ids.reserve(index.source_files.size());
    for (size_t i = 0; i < index.source_files.size(); ++i)
        index.source_file_ids.emplace(index.source_files[i], static_cast<SourceFileID>(i));
    for (size_t i = 0; i < index.modules.size(); ++i)
        index.module_by_name.try_emplace(index.modules[i].name, i);
    for (size_t i = 0; i < index.classes.size(); ++i)
        index.class_by_name.try_emplace(index.classes[i].name, i);
    for (size_t i = 0; i < index.typedefs.size(); ++i)
        index.typedef_by_name.try_emplace(index.typedefs[i].name, i);

    // Every stored file_id has to address the table it was written against; a
    // shard whose entries point outside it would hand the request path a URI
    // from another file, or none.
    const auto valid_file_id = [&](SourceFileID id) {
        return id == kInvalidSourceFileID || id < index.source_files.size();
    };
    for (const auto& module : index.modules) {
        if (!valid_file_id(module.file_id))
            return std::nullopt;
    }
    for (const auto& reference : index.references) {
        if (!valid_file_id(reference.file_id))
            return std::nullopt;
    }
    for (const auto& value : index.values) {
        if (!valid_file_id(value.file_id))
            return std::nullopt;
    }

    return loaded;
}
