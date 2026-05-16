#include "formatter.hpp"
#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Token types — mirrors FTT enum in formatter.py
// ---------------------------------------------------------------------------

enum class FTT {
    Unknown, Whitespace, Identifier, Keyword, NumericLiteral, StringLiteral,
    UnaryOperator, BinaryOperator, OpenGroup, CloseGroup, Hierarchy,
    CommentBlock, EolComment, Semicolon, Comma, Colon, Hash, AtSign,
    IncludeDirective,
};

enum class SD { MustAppend, MustWrap, Preserve, Undecided };

struct Tok {
    FTT ftt;
    std::string text;
    std::string lo;  // lowercase
    int pos{0};
};

// ---------------------------------------------------------------------------
// Keyword sets
// ---------------------------------------------------------------------------

static const std::unordered_set<std::string> SV_KW = {
    "module","macromodule","endmodule","interface","endinterface",
    "program","endprogram","package","endpackage","class","endclass",
    "function","endfunction","task","endtask","begin","end",
    "fork","join","join_any","join_none",
    "case","casex","casez","caseinside","endcase",
    "generate","endgenerate","covergroup","endgroup",
    "property","endproperty","sequence","endsequence",
    "checker","endchecker","clocking","endclocking",
    "config","endconfig","primitive","endprimitive",
    "specify","endspecify","table","endtable",
    "input","output","inout","ref",
    "logic","wire","reg","bit","byte","shortint","int","longint",
    "integer","real","realtime","shortreal","time","string","chandle","event",
    "always","always_comb","always_ff","always_latch","initial","final","assign",
    "if","else","for","foreach","while","do","repeat","forever",
    "return","break","continue",
    "typedef","struct","union","enum","packed","unpacked",
    "parameter","localparam","defparam",
    "virtual","static","automatic","const","var",
    "default","void","type","signed","unsigned","modport","genvar",
    "import","export","extern","protected","local",
    "posedge","negedge","edge","or","and","not",
    "assert","assume","cover","restrict",
    "unique","unique0","priority",
    "inside","dist","rand","randc","constraint",
    "super","this","null","new",
    "expect","wait","wait_order","disable","force","release",
    "deassign","pullup","pulldown",
    "supply0","supply1","tri","tri0","tri1","triand","trior","trireg",
    "wand","wor","uwire",
    "with","bind","let","cross","bins","binsof",
    "extends","implements","throughout","within","iff","intersect","first_match",
    "matches","tagged","wildcard","solve","before","pure","context",
    "timeprecision","timeunit","forkjoin","randcase","randsequence","randomize",
    "coverpoint","strong","weak",
};

static const std::unordered_set<std::string> TYPE_KW = {
    "logic","wire","reg","bit","byte","shortint","int","longint",
    "integer","real","realtime","shortreal","time","string","chandle",
    "event","void","signed","unsigned","packed",
};

static const std::unordered_set<std::string> INDENT_OPEN = {
    "module","macromodule","interface","program","package","class",
    "function","task","begin","fork",
    "case","casex","casez","caseinside",
    "generate","covergroup","property","sequence",
    "checker","clocking","config","primitive","specify",
};

static const std::unordered_set<std::string> BLOCK_OPEN = {
    "begin","fork","case","casex","casez","caseinside",
    "generate","covergroup","property","sequence",
    "checker","clocking","config","primitive","specify",
};

static const std::unordered_set<std::string> INDENT_CLOSE = {
    "endmodule","endinterface","endprogram","endpackage","endclass",
    "endfunction","endtask","end","join","join_any","join_none",
    "endcase","endgenerate","endgroup","endproperty","endsequence",
    "endchecker","endclocking","endconfig","endprimitive","endspecify","endtable",
};

static const std::unordered_set<std::string> ALWAYS_UNARY = {
    "~","!","~&","~|","~^","^~","++","--",
};

static const std::unordered_set<std::string> ALWAYS_BINARY = {
    "===","!==","==","!=",">=","->","<->","&&","||","**","##","|->",
    "+=","-=","*=","/=","%=","&=","|=","^=","<<=",">>=","<<<=",">>>=",
    "*","/","%",
};

static const std::unordered_set<std::string> PP_COND_WITH = {"`ifdef","`ifndef","`elsif"};
static const std::unordered_set<std::string> PP_COND_BARE = {"`else","`endif"};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string lower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

static bool has(const std::unordered_set<std::string>& s, const std::string& k) {
    return s.count(k) > 0;
}

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

static std::vector<Tok> tokenize(const std::string& src) {
    std::vector<Tok> toks;
    int n = (int)src.size();
    int i = 0;
    FTT prev = FTT::Unknown;

    auto push = [&](FTT f, int s, int e) {
        Tok t;
        t.ftt = f;
        t.text = src.substr(s, e - s);
        t.lo   = lower(t.text);
        t.pos  = s;
        toks.push_back(std::move(t));
        if (f != FTT::Whitespace && f != FTT::Unknown) prev = f;
    };

    while (i < n) {
        char c = src[i];

        // Whitespace
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            int s = i;
            while (i < n && (src[i]==' '||src[i]=='\t'||src[i]=='\r'||src[i]=='\n')) ++i;
            push(FTT::Whitespace, s, i);
            continue;
        }

        // EOL comment
        if (c=='/' && i+1<n && src[i+1]=='/') {
            int s = i;
            while (i<n && src[i]!='\n') ++i;
            push(FTT::EolComment, s, i);
            continue;
        }

        // Block comment
        if (c=='/' && i+1<n && src[i+1]=='*') {
            int s = i; i += 2;
            while (i+1<n && !(src[i]=='*' && src[i+1]=='/')) ++i;
            if (i+1<n) i += 2;
            push(FTT::CommentBlock, s, i);
            continue;
        }

        // String literal
        if (c=='"') {
            int s = i++;
            while (i<n && src[i]!='"') { if (src[i]=='\\') ++i; ++i; }
            if (i<n) ++i;
            push(FTT::StringLiteral, s, i);
            continue;
        }

        // `include directive
        if (c=='`' && src.compare(i,8,"`include")==0
                && (i+8>=(size_t)n || !std::isalnum((unsigned char)src[i+8]))) {
            int s = i; i += 8;
            while (i<n && (src[i]==' '||src[i]=='\t')) ++i;
            if (i<n && src[i]=='"') {
                ++i;
                while (i<n && src[i]!='"') ++i;
                if (i<n) ++i;
            }
            push(FTT::IncludeDirective, s, i);
            continue;
        }

        // Based literal starting with digits: 4'b... or plain number
        if (std::isdigit((unsigned char)c)) {
            int s = i;
            while (i<n && std::isdigit((unsigned char)src[i])) ++i;
            if (i<n && src[i]=='\'') {
                char nc = (i+1<n) ? (char)std::tolower((unsigned char)src[i+1]) : 0;
                if (nc=='b'||nc=='o'||nc=='d'||nc=='h'||nc=='x') {
                    ++i; ++i;
                    while (i<n && (std::isalnum((unsigned char)src[i])||src[i]=='_')) ++i;
                    push(FTT::NumericLiteral, s, i);
                    continue;
                }
            }
            while (i<n && (std::isalnum((unsigned char)src[i])||src[i]=='.'||src[i]=='_')) ++i;
            push(FTT::NumericLiteral, s, i);
            continue;
        }

        // Unbased literal '0 '1 'x 'z 'b... etc.
        if (c=='\'') {
            char nc = (i+1<n) ? (char)std::tolower((unsigned char)src[i+1]) : 0;
            if (nc=='b'||nc=='o'||nc=='d'||nc=='h'||nc=='0'||nc=='1'||nc=='x'||nc=='z') {
                int s = i; i += 2;
                while (i<n && (std::isalnum((unsigned char)src[i])||src[i]=='_')) ++i;
                push(FTT::NumericLiteral, s, i);
                continue;
            }
            // cast operator or bare quote — Unknown
            push(FTT::Unknown, i, i+1); ++i;
            continue;
        }

        // :: (scope)
        if (c==':' && i+1<n && src[i+1]==':') {
            push(FTT::Hierarchy, i, i+2); i += 2; continue;
        }

        // Word: identifier, keyword, SV macro
        if (std::isalpha((unsigned char)c)||c=='_'||c=='`'||c=='$') {
            int s = i++;
            while (i<n && (std::isalnum((unsigned char)src[i])||src[i]=='_'||src[i]=='$')) ++i;
            std::string w = src.substr(s, i-s);
            std::string wl = lower(w);
            FTT f = has(SV_KW, wl) ? FTT::Keyword : FTT::Identifier;
            Tok t; t.ftt=f; t.text=w; t.lo=wl; t.pos=s;
            toks.push_back(std::move(t));
            prev = f;
            continue;
        }

        // Multi-char operators — longest first
        {
            static const struct { const char* s; size_t n; FTT f; } mops[] = {
                {"<<<=",4,FTT::BinaryOperator},{">>>=",4,FTT::BinaryOperator},
                {"===",3,FTT::BinaryOperator},{"!==",3,FTT::BinaryOperator},
                {"<<=",3,FTT::BinaryOperator},{">>=",3,FTT::BinaryOperator},
                {"<<<",3,FTT::BinaryOperator},{">>>",3,FTT::BinaryOperator},
                {"<->",3,FTT::BinaryOperator},
                {"|->",3,FTT::BinaryOperator},
                {"==",2,FTT::BinaryOperator},{"!=",2,FTT::BinaryOperator},
                {"<=",2,FTT::BinaryOperator},{">=",2,FTT::BinaryOperator},
                {"->",2,FTT::BinaryOperator},
                {"+=",2,FTT::BinaryOperator},{"-=",2,FTT::BinaryOperator},
                {"*=",2,FTT::BinaryOperator},{"/=",2,FTT::BinaryOperator},
                {"%=",2,FTT::BinaryOperator},{"&=",2,FTT::BinaryOperator},
                {"|=",2,FTT::BinaryOperator},{"^=",2,FTT::BinaryOperator},
                {"<<",2,FTT::BinaryOperator},{">>",2,FTT::BinaryOperator},
                {"##",2,FTT::BinaryOperator},
                {"&&",2,FTT::BinaryOperator},{"||",2,FTT::BinaryOperator},
                {"**",2,FTT::BinaryOperator},
                {"++",2,FTT::UnaryOperator},{"--",2,FTT::UnaryOperator},
                {"~&",2,FTT::UnaryOperator},{"~|",2,FTT::UnaryOperator},
                {"~^",2,FTT::UnaryOperator},{"^~",2,FTT::UnaryOperator},
            };
            bool matched = false;
            for (auto& m : mops) {
                if (i+(int)m.n <= n && src.compare(i, m.n, m.s)==0) {
                    push(m.f, i, i+(int)m.n);
                    i += (int)m.n;
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
        }

        // Open / close groups
        if (c=='('||c=='['||c=='{') { push(FTT::OpenGroup, i, i+1); ++i; continue; }
        if (c==')'||c==']'||c=='}') { push(FTT::CloseGroup, i, i+1); ++i; continue; }

        // Punctuation with dedicated types
        if (c==';') { push(FTT::Semicolon, i, i+1); ++i; continue; }
        if (c==',') { push(FTT::Comma,     i, i+1); ++i; continue; }
        if (c=='.') { push(FTT::Hierarchy, i, i+1); ++i; continue; }
        if (c==':') { push(FTT::Colon,     i, i+1); ++i; continue; }
        if (c=='#') { push(FTT::Hash,      i, i+1); ++i; continue; }
        if (c=='@') { push(FTT::AtSign,    i, i+1); ++i; continue; }

        // Single-char operators: context-sensitive unary/binary
        if (c=='+'||c=='-'||c=='&'||c=='|'||c=='^'||c=='~'||c=='!'
                ||c=='<'||c=='>'||c=='='||c=='?'||c=='*'||c=='/'||c=='%'||c=='\\') {
            std::string op(1, c);
            FTT f;
            if (has(ALWAYS_UNARY, op)) {
                f = FTT::UnaryOperator;
            } else if (has(ALWAYS_BINARY, op)) {
                f = FTT::BinaryOperator;
            } else {
                // context-sensitive: unary after op, open_group, or start
                bool ctx_unary = (prev==FTT::Unknown||prev==FTT::BinaryOperator
                                  ||prev==FTT::UnaryOperator||prev==FTT::OpenGroup);
                if (ctx_unary && (c=='+'||c=='-'||c=='&'||c=='|'||c=='^'))
                    f = FTT::UnaryOperator;
                else
                    f = FTT::BinaryOperator;
            }
            push(f, i, i+1); ++i; continue;
        }

        // Unknown
        push(FTT::Unknown, i, i+1); ++i;
    }
    return toks;
}

// ---------------------------------------------------------------------------
// Disabled ranges (verilog_format: off/on and `define)
// ---------------------------------------------------------------------------

static std::vector<std::pair<int,int>> find_disabled(const std::string& src) {
    std::vector<std::pair<int,int>> out;
    static const std::regex FMT_OFF(
        R"(//\s*verilog_format\s*:\s*off\b[^\n]*)", std::regex::icase);
    static const std::regex FMT_ON(
        R"(//\s*verilog_format\s*:\s*on\b[^\n]*)", std::regex::icase);
    static const std::regex DEFINE_RE(R"(`define\b(?:[^\n]*?\\\n)*[^\n]*)");

    {
        auto it = std::sregex_iterator(src.begin(), src.end(), FMT_OFF);
        for (; it != std::sregex_iterator(); ++it) {
            int off_pos = (int)it->position();
            int search_from = off_pos + (int)it->length();
            auto it2 = std::sregex_iterator(
                src.begin() + search_from, src.end(), FMT_ON);
            int end = (it2 != std::sregex_iterator())
                      ? search_from + (int)it2->position()
                      : (int)src.size();
            out.push_back({off_pos, end});
        }
    }
    {
        auto it = std::sregex_iterator(src.begin(), src.end(), DEFINE_RE);
        for (; it != std::sregex_iterator(); ++it)
            out.push_back({(int)it->position(),
                           (int)(it->position() + it->length())});
    }
    std::sort(out.begin(), out.end());
    return out;
}

static bool in_disabled(int pos, const std::vector<std::pair<int,int>>& ranges) {
    for (auto& r : ranges)
        if (r.first <= pos && pos < r.second) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Spacing rules — ported from SpacesRequiredBetween() in token-annotator.cc
// ---------------------------------------------------------------------------

static int spaces_req(const Tok& L, const Tok& R,
                      const FormatOptions& opts, bool in_dim) {
    auto lf=L.ftt; const auto& lx=L.text; const auto& ll=L.lo;
    auto rf=R.ftt; const auto& rx=R.text;

    if (lf==FTT::IncludeDirective||rf==FTT::IncludeDirective) return 0;
    if (rf==FTT::EolComment||rf==FTT::CommentBlock) return 2;
    if (lf==FTT::OpenGroup||rf==FTT::CloseGroup) return 0;
    if (lf==FTT::UnaryOperator) return 0;
    if (lf==FTT::Hierarchy&&lx=="::") return 0;
    if (rf==FTT::Comma) return 0;
    if (lf==FTT::Comma) return 1;
    if (rf==FTT::Semicolon) return (lf==FTT::Colon)?1:0;
    if (lf==FTT::Semicolon) return 1;
    if (lf==FTT::AtSign) return 0;
    if (rf==FTT::AtSign) return 1;
    if (lf==FTT::UnaryOperator&&rx=="{") return 0;
    if (lf==FTT::BinaryOperator||rf==FTT::BinaryOperator) {
        if (rf==FTT::BinaryOperator&&in_dim&&opts.compact_indexing_and_selections) return 0;
        if (lf==FTT::BinaryOperator&&in_dim) return 0;
        return 1;
    }
    if (lf==FTT::Hierarchy||rf==FTT::Hierarchy) return 0;
    if (rx=="'"||lx=="'") return 0;
    if (rx=="(") {
        if (lf==FTT::Hash) return 0;
        if (lx==")") return 1;
        if (lf==FTT::Identifier) return 0;
        if (lf==FTT::Keyword) return 1;
        return 0;
    }
    if (lf==FTT::Colon) return in_dim?0:1;
    if (rf==FTT::Colon) {
        if (ll=="default") return 0;
        if (in_dim) return 0;
        if (lf==FTT::Identifier||lf==FTT::NumericLiteral||lf==FTT::CloseGroup) return 0;
        return 1;
    }
    if (lx=="}") return 1;
    if (rx=="{") return (lf==FTT::Keyword)?1:0;
    if (rx=="[") {
        if (lx=="]") return 0;
        if (lf==FTT::Keyword&&has(TYPE_KW,ll)) return 1;
        return 0;
    }
    auto nm=[](const Tok& t){
        return t.ftt==FTT::NumericLiteral||t.ftt==FTT::Identifier||t.ftt==FTT::Keyword;
    };
    if (nm(L)&&nm(R)) return 1;
    if (lf==FTT::Keyword) return 1;
    if (lf==FTT::UnaryOperator||rf==FTT::UnaryOperator) return 0;
    if (lf==FTT::Hash) return 0;
    if (rf==FTT::Hash) return 1;
    if (rf==FTT::Keyword) return 1;
    if (lx==")") return (rf==FTT::Colon)?0:1;
    if (lx=="]") return 1;
    return 1;
}

// ---------------------------------------------------------------------------
// Break decisions — ported from BreakDecisionBetween()
// ---------------------------------------------------------------------------

static SD break_dec(const Tok& L, const Tok& R,
                    const FormatOptions& opts, bool in_dim) {
    auto lf=L.ftt; const auto& lx=L.text; const auto& ll=L.lo;
    auto rf=R.ftt; const auto& rx=R.text; const auto& rl=R.lo;

    if (in_dim&&lf!=FTT::Colon&&lx!="["&&lx!="]"
              &&rf!=FTT::Colon&&rx!="["&&rx!="]")
        return SD::Preserve;
    if (rf==FTT::IncludeDirective||lf==FTT::IncludeDirective) return SD::MustWrap;
    if (lf==FTT::EolComment) return SD::MustWrap;
    if (lf==FTT::UnaryOperator) return SD::MustAppend;
    if (has(INDENT_CLOSE,rl)) return SD::MustWrap;
    if (rl=="else") {
        if (ll=="end") return opts.statement.wrap_end_else_clauses ? SD::MustWrap : SD::MustAppend;
        if (lx=="}") return SD::MustAppend;
        return SD::MustWrap;
    }
    if (ll=="else"&&rl=="begin") return SD::MustAppend;
    if (lx==")"&&rl=="begin") return SD::MustAppend;
    if (lf==FTT::Hash) return SD::MustAppend;
    return SD::Undecided;
}

// ---------------------------------------------------------------------------
// Keyword case transform
// ---------------------------------------------------------------------------

static std::string kw_case(const std::string& t, const std::string& mode) {
    if (mode=="lower") { auto r=t; for(auto&c:r) c=(char)std::tolower((unsigned char)c); return r; }
    if (mode=="upper") { auto r=t; for(auto&c:r) c=(char)std::toupper((unsigned char)c); return r; }
    return t;
}

// ---------------------------------------------------------------------------
// Split by top-level comma (depth-0)
// ---------------------------------------------------------------------------

static std::vector<std::string> split_top_level(const std::string& text) {
    std::vector<std::string> parts;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c=='('||c=='['||c=='{') ++depth;
        else if (c==')'||c==']'||c=='}') --depth;
        else if (c==',' && depth==0) {
            parts.push_back(text.substr(start, i-start));
            start = i+1;
        }
    }
    parts.push_back(text.substr(start));
    return parts;
}

// ---------------------------------------------------------------------------
// Port-declaration alignment pass
// ---------------------------------------------------------------------------

static const std::unordered_set<std::string> PORT_DIRS = {"input","output","inout"};

struct PortParsed {
    bool valid{false};
    std::string indent, direction, dtype, qualifier, dim;
    std::vector<std::pair<std::string,std::string>> names; // (name, trailing)
    std::string terminator, comment;
};

static PortParsed parse_port(const std::string& raw) {
    PortParsed r;
    std::string s = raw;
    while (!s.empty()&&(s.back()==' '||s.back()=='\t')) s.pop_back();
    size_t p=0; while(p<s.size()&&(s[p]==' '||s[p]=='\t')) ++p;
    r.indent = s.substr(0,p);
    std::string code = s.substr(p);

    auto cp=code.find("//");
    if (cp!=std::string::npos) {
        r.comment = "  "+code.substr(cp); code=code.substr(0,cp);
        while(!code.empty()&&(code.back()==' '||code.back()=='\t')) code.pop_back();
    }
    if (!code.empty()&&(code.back()==','||code.back()==';')) {
        r.terminator=std::string(1,code.back()); code.pop_back();
        while(!code.empty()&&(code.back()==' '||code.back()=='\t')) code.pop_back();
    }

    std::vector<std::string> toks;
    { std::istringstream ss(code); std::string w; while(ss>>w) toks.push_back(w); }
    if (toks.empty()) return r;

    static const std::unordered_set<std::string> BTYPES = {
        "logic","wire","reg","bit","byte","shortint","int","longint",
        "integer","real","realtime","shortreal","time","string","chandle","event","var",
    };
    static const std::unordered_set<std::string> NET_TYPES = {
        "var","wire","uwire","tri","tri0","tri1","wand","triand","wor","trior","trireg",
        "supply0","supply1",
    };
    static const std::unordered_set<std::string> DATA_TYPES = {
        "logic","reg","bit","byte","shortint","int","longint","integer","time",
    };
    static const std::unordered_set<std::string> QUALS = {"signed","unsigned"};

    std::string d0 = lower(toks[0]);
    if (!has(PORT_DIRS,d0)) return r;
    r.direction = toks[0];
    size_t idx=1;

    if (idx<toks.size()) {
        const std::string& cand = toks[idx];
        std::string cl=lower(cand);
        // Candidate must be a pure identifier (no commas or special chars)
        static const std::regex PURE_ID(R"(^[A-Za-z_]\w*(::\w+)?$)");
        bool pure_id = std::regex_match(cand, PURE_ID);
        if (pure_id && cand[0]!='[' && !has(QUALS,cl)) {
            bool is_builtin  = has(BTYPES,cl);
            bool is_usertype = pure_id && idx+1<toks.size();
            if (is_builtin||is_usertype) {
                r.dtype=toks[idx++];
                if (has(NET_TYPES,cl) && idx<toks.size()) {
                    std::string ncl=lower(toks[idx]);
                    if (has(DATA_TYPES,ncl)) {
                        r.dtype += " " + toks[idx++];
                    }
                }
            }
        }
    }
    if (idx<toks.size()&&has(QUALS,lower(toks[idx]))) r.qualifier=toks[idx++];
    if (idx<toks.size()&&!toks[idx].empty()&&toks[idx][0]=='[') {
        int depth=0;
        while(idx<toks.size()) {
            auto& t=toks[idx]; r.dim+=t;
            for(char ch:t){ if(ch=='[')++depth; else if(ch==']')--depth; }
            ++idx; if(depth<=0) break;
        }
    }
    if (idx>=toks.size()) return r;

    // Remaining tokens are comma-separated names (possibly with unpacked dims/defaults)
    // Rebuild remaining as string and split by top-level comma
    std::string remaining;
    for (size_t k=idx; k<toks.size(); ++k) {
        if (k>idx) remaining+=' ';
        remaining+=toks[k];
    }
    auto raw_names = split_top_level(remaining);
    if (raw_names.empty()) return r;

    static const std::regex ID_TRAIL_RE(R"(^([A-Za-z_]\w*)\s*(.*))");
    for (auto& rn : raw_names) {
        // trim
        size_t a=0; while(a<rn.size()&&(rn[a]==' '||rn[a]=='\t')) ++a;
        size_t b=rn.size(); while(b>a&&(rn[b-1]==' '||rn[b-1]=='\t')) --b;
        std::string nm = rn.substr(a,b-a);
        std::smatch m;
        if (std::regex_match(nm, m, ID_TRAIL_RE)) {
            r.names.push_back({m[1].str(), m[2].str()});
        } else {
            r.names.push_back({nm, ""});
        }
    }
    r.valid = !r.names.empty();
    return r;
}

static std::string align_port_pass(const std::string& text, const FormatOptions& opts) {
    static const std::regex DIR_RE(R"(^\s*(?:input|output|inout)\b)",std::regex::icase);

    std::vector<std::string> lines;
    { std::istringstream ss(text); std::string l; while(std::getline(ss,l)) lines.push_back(l); }

    std::vector<std::string> out;
    size_t i=0;
    while(i<lines.size()) {
        if (!std::regex_search(lines[i],DIR_RE)) { out.push_back(lines[i]); ++i; continue; }
        std::vector<std::pair<std::string,PortParsed>> blk;
        size_t j=i;
        while(j<lines.size()&&std::regex_search(lines[j],DIR_RE))
            blk.push_back({lines[j],parse_port(lines[j++])});

        int md=0, ms2_content=0, mdim=0; int np=0;
        size_t max_slots=0;
        for(auto&[orig,pp]:blk) {
            if(!pp.valid) continue; ++np;
            md=std::max(md,(int)pp.direction.size());
            std::string s2=pp.dtype+(pp.qualifier.empty()?"":" "+pp.qualifier);
            ms2_content=std::max(ms2_content,(int)s2.size());
            mdim=std::max(mdim,(int)pp.dim.size());
            max_slots=std::max(max_slots,pp.names.size());
        }
        if(!np) { for(auto&[o,_]:blk) out.push_back(o); i=j; continue; }

        int s1=std::max(opts.port_declaration.section1_min_width, md+1);
        int s2=ms2_content>0?std::max(opts.port_declaration.section2_min_width, ms2_content+1):0;
        int s3=mdim>0?std::max(opts.port_declaration.section3_min_width, mdim+1):0;
        int s5_min = opts.port_declaration.section5_min_width;

        // Per-slot id and trailing widths
        std::vector<int> id_widths(max_slots,0), trail_widths(max_slots,0);
        for(size_t slot=0;slot<max_slots;++slot) {
            int max_id=0, max_tr=0;
            for(auto&[orig,pp]:blk) {
                if(!pp.valid) continue;
                if(slot<pp.names.size()){
                    max_id=std::max(max_id,(int)pp.names[slot].first.size());
                    max_tr=std::max(max_tr,(int)pp.names[slot].second.size());
                }
            }
            id_widths[slot] = std::max(opts.port_declaration.section4_min_width, max_id+1);
            trail_widths[slot] = std::max(s5_min, max_tr);
        }

        auto pad=[](std::string s,int w)->std::string{ s.resize(std::max((int)s.size(),w),' '); return s; };

        for(auto&[orig,pp]:blk) {
            if(!pp.valid) { out.push_back(orig); continue; }
            int line_s1 = s1;
            int line_s2 = s2;
            int line_s3 = s3;
            std::vector<int> line_id_widths = id_widths;
            std::vector<int> line_trail_widths = trail_widths;
            if (opts.port_declaration.align_adaptive) {
                const auto& pd = opts.port_declaration;
                std::string tp=pp.dtype+(pp.qualifier.empty()?"":" "+pp.qualifier);
                // Cumulative minimum target end columns (fixed reference points).
                // Each section pads to max(target_end, actual_end).
                // Overflow on one line is absorbed by subsequent sections' slack,
                // so downstream sections stay aligned as long as overflow is small.
                int t1 = pd.section1_min_width;
                int t2 = t1 + (s2 > 0 ? pd.section2_min_width : 0);
                int t3 = t2 + (s3 > 0 ? pd.section3_min_width : 0);

                int e1 = std::max(t1, (int)pp.direction.size()+1);
                line_s1 = e1;

                int e2 = e1;
                if (s2 > 0) {
                    int c2 = tp.empty() ? 0 : (int)tp.size()+1;
                    e2 = std::max(t2, e1 + c2);
                    line_s2 = e2 - e1;
                }

                if (s3 > 0) {
                    int c3 = pp.dim.empty() ? 0 : (int)pp.dim.size()+1;
                    int e3 = std::max(t3, e2 + c3);
                    line_s3 = e3 - e2;
                }

                line_id_widths.clear();
                line_trail_widths.clear();
                for (const auto& [nm, tr] : pp.names) {
                    line_id_widths.push_back(std::max(pd.section4_min_width,
                                                      (int)nm.size()+1));
                    line_trail_widths.push_back(std::max(s5_min, (int)tr.size()));
                }
            }
            std::string line=pp.indent;
            line+=pad(pp.direction,line_s1);
            if(line_s2>0) {
                std::string tp=pp.dtype+(pp.qualifier.empty()?"":" "+pp.qualifier);
                line+=pad(tp,line_s2);
            }
            if(line_s3>0) line+=pad(pp.dim,line_s3);

            // Emit per-slot names — mirrors Python _reassemble_port_line
            size_t nslots = pp.names.size();
            for(size_t slot=0;slot<nslots;++slot) {
                bool is_last = (slot==nslots-1);
                const auto& nm = pp.names[slot].first;
                const auto& tr = pp.names[slot].second;
                // Pad name to slot id width
                if(slot<line_id_widths.size())
                    line += pad(nm, line_id_widths[slot]);
                else
                    line += nm;

                if(!is_last) {
                    // Non-last slot: pad trailing then ", "
                    if(!tr.empty() && s5_min>0 && slot<line_trail_widths.size() && line_trail_widths[slot]>1)
                        line += pad(tr, line_trail_widths[slot]) + ", ";
                    else if(slot<line_trail_widths.size())
                        line += pad(tr, line_trail_widths[slot]) + ", ";
                    else
                        line += tr + ", ";
                } else {
                    // Last slot — matches Python _reassemble_port_line last-slot logic
                    std::string term = pp.terminator.empty() ? ";" : pp.terminator;
                    if(!tr.empty() && s5_min>0 && slot<line_trail_widths.size() && line_trail_widths[slot]>1) {
                        line += pad(tr, line_trail_widths[slot]) + term;
                    } else {
                        if(slot<line_trail_widths.size())
                            line += pad(tr, line_trail_widths[slot]);
                        else
                            line += tr;
                        line += term;
                        // rstrip trailing spaces before terminator (done below)
                    }
                }
            }
            if(!pp.comment.empty()) line+=pp.comment;
            while(!line.empty()&&line.back()==' ') line.pop_back();
            out.push_back(line);
        }
        i=j;
    }
    std::string r; for(size_t k=0;k<out.size();++k){if(k)r+='\n'; r+=out[k];} return r;
}

// ---------------------------------------------------------------------------
// Statement assignment alignment pass
// ---------------------------------------------------------------------------

static bool is_var_line(const std::string& line); // forward declaration

static std::string align_assign_pass(const std::string& text, const FormatOptions& opts) {
    static const std::regex BLK(R"( ((?:[+\-*/%&|^]|<<|>>|<<<|>>>)?=)(?!=) )");
    static const std::regex NBLK(R"( <= )");

    auto find_op=[&](const std::string& line)->std::pair<int,std::string>{
        size_t cp=line.find("//");
        std::string code=(cp!=std::string::npos)?line.substr(0,cp):line;
        std::smatch m1,m2;
        bool h1=std::regex_search(code,m1,BLK);
        bool h2=std::regex_search(code,m2,NBLK);
        if(h1&&h2) return (m2.position()<m1.position())?
            std::make_pair((int)m2.position(),std::string("<=")):
            std::make_pair((int)m1.position(),m1[1].str());
        if(h2) return {(int)m2.position(),"<="};
        if(h1) return {(int)m1.position(),m1[1].str()};
        return {-1,""};
    };

    std::vector<std::string> lines;
    { std::istringstream ss(text); std::string l; while(std::getline(ss,l)) lines.push_back(l); }

    std::vector<std::string> out;
    size_t i=0;
    while(i<lines.size()) {
        auto [p0,op0]=find_op(lines[i]);
        if(p0<0||is_var_line(lines[i])) { out.push_back(lines[i]); ++i; continue; }
        size_t ind=0;
        while(ind<lines[i].size()&&(lines[i][ind]==' '||lines[i][ind]=='\t')) ++ind;

        struct E { std::string line; int pos; std::string op; int lw; };
        std::vector<E> grp;
        size_t j=i;
        while(j<lines.size()) {
            const auto& lj=lines[j];
            if(lj.empty()) break;
            if(is_var_line(lj)) break;
            size_t ij=0; while(ij<lj.size()&&(lj[ij]==' '||lj[ij]=='\t')) ++ij;
            if(ij!=ind) break;
            auto[pj,oj]=find_op(lj);
            if(pj<0) break;
            grp.push_back({lj,pj,oj,(int)(pj-(int)ij)});
            ++j;
        }
        if(grp.empty()){out.push_back(lines[i]);++i;continue;}

        int mx=opts.statement.lhs_min_width;
        for(auto&e:grp) mx=std::max(mx,e.lw);
        int col=mx+1;
        for(auto&e:grp) {
            int sp=std::max(1,col-e.lw);
            std::string lhs=e.line.substr(0,e.pos);
            size_t rs=(size_t)(e.pos+1+(int)e.op.size()+1);
            std::string rhs=(rs<e.line.size())?e.line.substr(rs):"";
            out.push_back(lhs+std::string(sp,' ')+e.op+" "+rhs);
        }
        i=j;
    }
    std::string r; for(size_t k=0;k<out.size();++k){if(k)r+='\n'; r+=out[k];} return r;
}

// ---------------------------------------------------------------------------
// Variable declaration alignment pass — ported from _align_variable_declarations_pass
// ---------------------------------------------------------------------------

static const std::unordered_set<std::string> VAR_BUILTIN_TYPES = {
    "wire","logic","reg","bit","byte","int","integer","time",
    "shortint","longint","signed","unsigned",
};
static const std::unordered_set<std::string> VAR_PREFIX_KW = {
    "static","automatic","const","var",
};
static const std::unordered_set<std::string> VAR_EXCLUDED = {
    "input","output","inout","ref",
};

struct VarParsed {
    std::string indent, type_kw, qualifier, dim;
    std::vector<std::pair<std::string,std::string>> declarators; // (name, trailing)
    std::string comment;
};

static VarParsed* parse_var_line(const std::string& line) {
    std::string stripped = line;
    while(!stripped.empty()&&(stripped.back()==' '||stripped.back()=='\t')) stripped.pop_back();
    size_t ip=0; while(ip<stripped.size()&&(stripped[ip]==' '||stripped[ip]=='\t')) ++ip;
    std::string indent = stripped.substr(0,ip);
    std::string code = stripped.substr(ip);

    // Strip trailing // comment
    std::string comment;
    auto cm = code.rfind("//");
    if(cm!=std::string::npos) {
        // Make sure // is not inside a string
        comment = "  " + code.substr(cm);
        code = code.substr(0,cm);
        while(!code.empty()&&(code.back()==' '||code.back()=='\t')) code.pop_back();
    }

    // Must end with ;
    if(code.empty()||code.back()!=';') return nullptr;
    code.pop_back();
    while(!code.empty()&&(code.back()==' '||code.back()=='\t')) code.pop_back();

    // Split into tokens, expanding compact "identifier[...]" tokens
    std::vector<std::string> raw_toks;
    { std::istringstream ss(code); std::string w; while(ss>>w) raw_toks.push_back(w); }
    if(raw_toks.empty()) return nullptr;
    // Expand compact "ident[...]" -> "ident" + "[...]"
    static const std::regex COMPACT_DIM_RE(R"(^([A-Za-z_]\w*(?:::\w+)?)(\[.+)$)");
    std::vector<std::string> toks;
    for(auto& t : raw_toks) {
        std::smatch cm;
        if(std::regex_match(t, cm, COMPACT_DIM_RE)) {
            toks.push_back(cm[1].str());
            toks.push_back(cm[2].str());
        } else {
            toks.push_back(t);
        }
    }

    std::string first = lower(toks[0]);
    if(has(VAR_EXCLUDED, first)) return nullptr;

    size_t idx=0;
    std::vector<std::string> type_parts;
    while(idx<toks.size()&&has(VAR_PREFIX_KW, lower(toks[idx]))) {
        type_parts.push_back(toks[idx++]);
    }
    if(idx>=toks.size()) return nullptr;

    first = lower(toks[idx]);
    if(has(VAR_BUILTIN_TYPES, first)) {
        type_parts.push_back(toks[idx++]);
    } else {
        // User-defined type: must be an identifier not an SV keyword
        if(!std::isalpha((unsigned char)toks[idx][0])&&toks[idx][0]!='_') return nullptr;
        if(has(SV_KW, first)) return nullptr;
        if(idx+1>=toks.size()) return nullptr;
        // Next must look like dimension, qualifier, or identifier
        const std::string& nxt = toks[idx+1];
        std::string nxtl = lower(nxt);
        static const std::unordered_set<std::string> QUALS2 = {"signed","unsigned"};
        bool ok = nxt[0]=='[' || std::isalpha((unsigned char)nxt[0]) || nxt[0]=='_'
                  || has(QUALS2,nxtl);
        if(!ok) return nullptr;
        type_parts.push_back(toks[idx++]);
    }

    std::string type_kw;
    for(size_t k=0;k<type_parts.size();++k){ if(k) type_kw+=' '; type_kw+=type_parts[k]; }

    // Optional qualifier
    std::string qualifier;
    static const std::unordered_set<std::string> QUALS = {"signed","unsigned"};
    if(idx<toks.size()&&has(QUALS,lower(toks[idx]))) qualifier=toks[idx++];

    // Optional packed dim
    std::string dim;
    if(idx<toks.size()&&!toks[idx].empty()&&toks[idx][0]=='[') {
        int depth=0;
        while(idx<toks.size()) {
            dim+=toks[idx];
            for(char c:toks[idx]){ if(c=='[')++depth; else if(c==']')--depth; }
            ++idx; if(depth<=0) break;
        }
    }
    if(idx>=toks.size()) return nullptr;

    // Remaining: comma-separated declarators
    std::string remaining;
    for(size_t k=idx;k<toks.size();++k){ if(k>idx) remaining+=' '; remaining+=toks[k]; }

    auto raw_names = split_top_level(remaining);
    if(raw_names.empty()) return nullptr;

    // Validate: each name must start with [A-Za-z_]
    static const std::regex ID_START(R"(^[A-Za-z_])");
    for(auto& rn:raw_names){
        size_t a=0; while(a<rn.size()&&(rn[a]==' '||rn[a]=='\t')) ++a;
        if(a>=rn.size()) continue;
        if(!std::regex_search(rn.substr(a,1), ID_START)) return nullptr;
    }

    static const std::regex DECL_RE(R"(^([A-Za-z_]\w*)\s*(.*))");
    auto* vp = new VarParsed{indent, type_kw, qualifier, dim, {}, comment};
    for(auto& rn:raw_names){
        size_t a=0; while(a<rn.size()&&(rn[a]==' '||rn[a]=='\t')) ++a;
        size_t b=rn.size(); while(b>a&&(rn[b-1]==' '||rn[b-1]=='\t')) --b;
        std::string nm=rn.substr(a,b-a);
        std::smatch m;
        if(std::regex_match(nm,m,DECL_RE)) {
            vp->declarators.push_back({m[1].str(), m[2].str()});
        } else {
            vp->declarators.push_back({nm,""});
        }
    }
    if(vp->declarators.empty()){ delete vp; return nullptr; }
    // Reject if any declarator looks like a function/instance call (has '(' in name or trailing)
    for(auto& decl:vp->declarators){
        if(decl.first.find('(')!=std::string::npos ||
           decl.second.find('(')!=std::string::npos) {
            delete vp; return nullptr;
        }
    }
    return vp;
}

static bool is_var_line(const std::string& line) {
    static const std::regex VAR_RE(
        R"(^\s*(?:(?:static|automatic|const|var)\s+)*(?:wire|logic|reg|bit|byte|int|integer|time|shortint|longint|signed|unsigned)\b)",
        std::regex::icase);
    return std::regex_search(line, VAR_RE);
}

static std::string align_var_pass(const std::string& text, const FormatOptions& opts) {
    const auto& vo = opts.var_declaration;

    std::vector<std::string> lines;
    { std::istringstream ss(text); std::string l; while(std::getline(ss,l)) lines.push_back(l); }

    std::vector<std::string> out;
    size_t i=0;
    while(i<lines.size()) {
        const std::string& line = lines[i];

        // Check if var line or parseable
        bool is_var = is_var_line(line);
        VarParsed* single_parsed = nullptr;
        if(!is_var) {
            single_parsed = parse_var_line(line);
            if(!single_parsed) { out.push_back(line); ++i; continue; }
            delete single_parsed;
        }

        // Collect block
        struct BlkEntry { std::string orig; VarParsed* parsed; };
        std::vector<BlkEntry> block;
        size_t j=i;
        while(j<lines.size()) {
            const std::string& cur = lines[j];
            if(is_var_line(cur)) {
                block.push_back({cur, parse_var_line(cur)});
                ++j; continue;
            }
            VarParsed* pp = parse_var_line(cur);
            if(pp) {
                block.push_back({cur, pp});
                ++j; continue;
            }
            // Comment/blank lines pass through without breaking block
            std::string stripped=cur;
            size_t sp=0; while(sp<stripped.size()&&(stripped[sp]==' '||stripped[sp]=='\t')) ++sp;
            std::string trimmed=stripped.substr(sp);
            if(trimmed.empty()||trimmed.substr(0,2)=="//"||(trimmed.size()>=2&&trimmed[0]=='/'&&trimmed[1]=='*')) {
                block.push_back({cur, nullptr});
                ++j; continue;
            }
            break;
        }

        // Count parseable entries
        int np=0;
        for(auto&e:block) if(e.parsed) ++np;

        if(np<=0) {
            for(auto&e:block) { out.push_back(e.orig); if(e.parsed) delete e.parsed; }
            i=j; continue;
        }

        // Compute section widths
        int max_s1=0;
        for(auto&e:block){
            if(!e.parsed) continue;
            std::string s1=e.parsed->type_kw+(e.parsed->qualifier.empty()?"":" "+e.parsed->qualifier);
            max_s1=std::max(max_s1,(int)s1.size());
        }
        int s1_w = std::max(vo.section1_min_width, max_s1+1);

        int max_dim=0;
        for(auto&e:block) if(e.parsed) max_dim=std::max(max_dim,(int)e.parsed->dim.size());
        int s2_w = max_dim>0 ? std::max(vo.section2_min_width, max_dim+1) : 0;

        size_t max_slots=0;
        for(auto&e:block) if(e.parsed) max_slots=std::max(max_slots,e.parsed->declarators.size());

        std::vector<int> id_widths(max_slots,0), trail_widths(max_slots,0);
        for(size_t slot=0;slot<max_slots;++slot){
            int mx_id=0,mx_tr=0;
            for(auto&e:block){
                if(!e.parsed) continue;
                if(slot<e.parsed->declarators.size()){
                    mx_id=std::max(mx_id,(int)e.parsed->declarators[slot].first.size());
                    mx_tr=std::max(mx_tr,(int)e.parsed->declarators[slot].second.size());
                }
            }
            id_widths[slot]=std::max(vo.section3_min_width, mx_id+1);
            trail_widths[slot]=std::max(vo.section4_min_width, mx_tr);
        }

        auto pad_to=[](std::string s,int w)->std::string{
            if((int)s.size()<w) s.resize(w,' ');
            return s;
        };

        for(auto&e:block){
            if(!e.parsed){out.push_back(e.orig); continue;}
            const auto& vp=*e.parsed;
            int line_s1_w = s1_w;
            int line_s2_w = s2_w;
            std::vector<int> line_id_widths = id_widths;
            std::vector<int> line_trail_widths = trail_widths;
            if (vo.align_adaptive) {
                std::string s1part = vp.type_kw + (vp.qualifier.empty()?"":" "+vp.qualifier);
                // Cumulative minimum target end columns — same model as port_declaration.
                int t1 = vo.section1_min_width;
                int t2 = t1 + (s2_w > 0 ? vo.section2_min_width : 0);

                int e1 = std::max(t1, (int)s1part.size()+1);
                line_s1_w = e1;

                int e2 = e1;
                if (s2_w > 0) {
                    int c2 = vp.dim.empty() ? 0 : (int)vp.dim.size()+1;
                    e2 = std::max(t2, e1 + c2);
                    line_s2_w = e2 - e1;
                }

                line_id_widths.clear();
                line_trail_widths.clear();
                for (const auto& [nm, tr] : vp.declarators) {
                    line_id_widths.push_back(std::max(vo.section3_min_width, (int)nm.size()+1));
                    line_trail_widths.push_back(std::max(vo.section4_min_width, (int)tr.size()));
                }
            }
            std::string ln = vp.indent;
            std::string s1part = vp.type_kw + (vp.qualifier.empty()?"":" "+vp.qualifier);
            ln += pad_to(s1part, line_s1_w);
            if(line_s2_w>0) ln += pad_to(vp.dim, line_s2_w);
            size_t nd=vp.declarators.size();
            for(size_t k=0;k<nd;++k){
                bool is_last=(k==nd-1);
                const auto& nm=vp.declarators[k].first;
                const auto& tr=vp.declarators[k].second;
                if(!is_last){
                    ln += pad_to(nm, line_id_widths[k]);
                    if(!tr.empty()&&vo.section4_min_width>0&&k<line_trail_widths.size()&&line_trail_widths[k]>1){
                        ln += pad_to(tr, line_trail_widths[k]) + ", ";
                    } else if(k<line_trail_widths.size()){
                        ln += pad_to(tr, line_trail_widths[k]) + ", ";
                    } else {
                        ln += tr + ", ";
                    }
                } else {
                    // Last slot — mirrors Python _reassemble_var_line last-slot logic
                    // Pad name to slot width (same as Python: line = line + name.ljust(id_widths[k]))
                    if(k<line_id_widths.size())
                        ln += pad_to(nm, line_id_widths[k]);
                    else
                        ln += nm;
                    if(!tr.empty() && vo.section4_min_width>0 && k<line_trail_widths.size() && line_trail_widths[k]>1) {
                        // trailing content exists + section4 configured: pad trailing then ";"
                        ln += pad_to(tr, line_trail_widths[k]) + ";";
                    } else {
                        // Python else branch: trailing.ljust(w) + trailing + ";"
                        // For empty trailing this gives: spaces(w) + "" + ";" = padding + ";"
                        if(k<line_trail_widths.size())
                            ln += pad_to(tr, line_trail_widths[k]);
                        ln += tr + ";";
                    }
                }
            }
            if(!vp.comment.empty()) ln += vp.comment;
            while(!ln.empty()&&ln.back()==' ') ln.pop_back();
            out.push_back(ln);
            delete e.parsed;
        }
        i=j;
    }
    std::string r; for(size_t k=0;k<out.size();++k){if(k)r+='\n'; r+=out[k];} return r;
}

// ---------------------------------------------------------------------------
// Module instantiation expansion pass
// ---------------------------------------------------------------------------

// Collect lines from start until ')' at depth 0 followed by ';'
static bool collect_instance(const std::vector<std::string>& lines, size_t start,
                              size_t& end_i, std::string& flat) {
    std::vector<std::string> parts;
    int depth=0;
    size_t j=start;
    while(j<lines.size()){
        std::string stripped=lines[j];
        size_t sp=0; while(sp<stripped.size()&&(stripped[sp]==' '||stripped[sp]=='\t')) ++sp;
        parts.push_back(stripped.substr(sp));
        for(char ch:lines[j]){
            if(ch=='(') ++depth;
            else if(ch==')') --depth;
            else if(ch==';'&&depth==0){
                end_i=j+1;
                flat="";
                for(size_t k=0;k<parts.size();++k){ if(k) flat+=' '; flat+=parts[k]; }
                return true;
            }
        }
        ++j;
    }
    return false;
}

// Extract content of outermost (...) immediately before ;
static bool extract_port_list(const std::string& flat, std::string& port_list) {
    auto semi=flat.rfind(';');
    if(semi==std::string::npos) return false;
    int j=(int)semi-1;
    while(j>=0&&(flat[j]==' '||flat[j]=='\t')) --j;
    if(j<0||flat[j]!=')') return false;
    int close=j;
    int depth=1;
    --j;
    while(j>=0&&depth>0){
        if(flat[j]==')') ++depth;
        else if(flat[j]=='(') --depth;
        --j;
    }
    port_list=flat.substr(j+2,close-(j+2));
    // trim
    size_t a=0; while(a<port_list.size()&&(port_list[a]==' '||port_list[a]=='\t'||port_list[a]=='\n')) ++a;
    size_t b=port_list.size(); while(b>a&&(port_list[b-1]==' '||port_list[b-1]=='\t'||port_list[b-1]=='\n')) --b;
    port_list=port_list.substr(a,b-a);
    return true;
}

// Parse named port connections .name(signal), ...
// Returns false if positional
static bool parse_named_ports(const std::string& port_list,
                               std::vector<std::pair<std::string,std::string>>& ports) {
    size_t i=0,n=port_list.size();
    while(i<n){
        while(i<n&&(port_list[i]==' '||port_list[i]=='\t'||port_list[i]=='\n'||port_list[i]==',')) ++i;
        if(i>=n) break;
        if(port_list[i]!='.') return false; // positional
        ++i;
        size_t j=i;
        while(j<n&&(std::isalnum((unsigned char)port_list[j])||port_list[j]=='_')) ++j;
        std::string port_name=port_list.substr(i,j-i);
        i=j;
        while(i<n&&(port_list[i]==' '||port_list[i]=='\t')) ++i;
        if(i>=n||port_list[i]!='(') return false;
        ++i;
        int depth=1;
        size_t sig_start=i;
        while(i<n&&depth>0){
            if(port_list[i]=='(') ++depth;
            else if(port_list[i]==')') --depth;
            ++i;
        }
        std::string sig=port_list.substr(sig_start,i-1-sig_start);
        // trim sig
        size_t a=0; while(a<sig.size()&&(sig[a]==' '||sig[a]=='\t')) ++a;
        size_t b=sig.size(); while(b>a&&(sig[b-1]==' '||sig[b-1]=='\t')) --b;
        sig=sig.substr(a,b-a);
        ports.push_back({port_name,sig});
    }
    return !ports.empty();
}

// Split flat into (module_type, param_block, inst_name)
static bool split_inst_parts(const std::string& flat, std::string& module_type,
                               std::string& param_block, std::string& inst_name) {
    static const std::regex MTYPE_RE(R"(^(\w+)\s*)");
    std::smatch m;
    if(!std::regex_search(flat,m,MTYPE_RE)) return false;
    module_type=m[1].str();
    std::string rest=flat.substr(m.position()+m.length());
    param_block="";
    if(!rest.empty()&&rest[0]=='#'){
        auto ki=rest.find('(');
        if(ki==std::string::npos) return false;
        size_t k=ki+1;
        int depth=1;
        while(k<rest.size()&&depth>0){
            if(rest[k]=='(') ++depth;
            else if(rest[k]==')') --depth;
            ++k;
        }
        param_block=rest.substr(0,k);
        // trim
        while(!param_block.empty()&&(param_block.back()==' '||param_block.back()=='\t')) param_block.pop_back();
        rest=rest.substr(k);
        while(!rest.empty()&&(rest[0]==' '||rest[0]=='\t')) rest=rest.substr(1);
    }
    static const std::regex INAME_RE(R"(^(\w+))");
    std::smatch m2;
    if(!std::regex_search(rest,m2,INAME_RE)) return false;
    inst_name=m2[1].str();
    return true;
}

static std::string expand_instances_pass(const std::string& text, const FormatOptions& opts) {
    static const std::regex INST_RE(R"(^(\s*)(\w+)\s+(\w+)\s*\()");
    static const std::regex INST_PARAM_RE(R"(^(\s*)(\w+)\s*#\s*\()");

    const std::string port_indent(opts.instance.port_indent_level * opts.indent_size, ' ');
    int m_before = opts.instance.instance_port_name_width;
    int m_inside = opts.instance.instance_port_between_paren_width;
    bool adaptive = opts.instance.align_adaptive;

    std::vector<std::string> lines;
    { std::istringstream ss(text); std::string l; while(std::getline(ss,l)) lines.push_back(l); }

    std::vector<std::string> out;
    size_t i=0;
    while(i<lines.size()){
        const std::string& line=lines[i];
        std::smatch m,mp;
        bool is_inst = std::regex_search(line,m,INST_RE)&&m.position()==0;
        bool is_param = !is_inst && std::regex_search(line,mp,INST_PARAM_RE)&&mp.position()==0;

        if(!is_inst&&!is_param){ out.push_back(line); ++i; continue; }

        std::string indent, module_type, inst_name, param_block;

        if(is_inst){
            if(has(SV_KW,lower(m[2].str()))||has(SV_KW,lower(m[3].str()))){
                out.push_back(line); ++i; continue;
            }
            indent=m[1].str();
            module_type=m[2].str();
            inst_name=m[3].str();
        } else {
            if(has(SV_KW,lower(mp[2].str()))){
                out.push_back(line); ++i; continue;
            }
            indent=mp[1].str();
            // Collect first to parse parts
            size_t end_early; std::string flat_early;
            if(!collect_instance(lines,i,end_early,flat_early)){
                out.push_back(line); ++i; continue;
            }
            if(!split_inst_parts(flat_early,module_type,param_block,inst_name)){
                for(size_t k=i;k<end_early;++k) out.push_back(lines[k]);
                i=end_early; continue;
            }
            // Use collected for rest of processing
            size_t end_i; std::string flat;
            if(!collect_instance(lines,i,end_i,flat)){
                for(size_t k=i;k<end_early;++k) out.push_back(lines[k]);
                i=end_early; continue;
            }
            std::string port_list;
            if(!extract_port_list(flat,port_list)){
                for(size_t k=i;k<end_i;++k) out.push_back(lines[k]);
                i=end_i; continue;
            }
            std::vector<std::pair<std::string,std::string>> ports;
            if(!parse_named_ports(port_list,ports)){
                for(size_t k=i;k<end_i;++k) out.push_back(lines[k]);
                i=end_i; continue;
            }
            int max_port=0,max_sig=0;
            for(auto&[p,s]:ports){
                max_port=std::max(max_port,(int)p.size());
                max_sig=std::max(max_sig,(int)s.size());
            }
            int eff_before=std::max(1,m_before-max_port);
            int eff_inside=std::max(0,m_inside-max_sig);

            std::string hdr=indent+module_type;
            if(!param_block.empty()) hdr+=" "+param_block;
            hdr+=" "+inst_name+" (";
            out.push_back(hdr);
            for(size_t k=0;k<ports.size();++k){
                auto&[port,sig]=ports[k];
                std::string comma=(k+1==ports.size())?"":",";
                std::string pline;
                if(adaptive){
                    int sb=std::max(1,m_before-(int)port.size());
                    int si=std::max(0,m_inside-(int)sig.size());
                    pline=indent+port_indent+"."+port+std::string(sb,' ')+"("+sig+std::string(si,' ')+")"+comma;
                } else {
                    std::string pname=port; pname.resize(std::max((int)pname.size(),max_port),' ');
                    std::string sname=sig; sname.resize(std::max((int)sname.size(),max_sig),' ');
                    pline=indent+port_indent+"."+pname+std::string(eff_before,' ')+"("+sname+std::string(eff_inside,' ')+")"+comma;
                }
                while(!pline.empty()&&pline.back()==' ') pline.pop_back();
                out.push_back(pline);
            }
            out.push_back(indent+");");
            i=end_i;
            continue;
        }

        // Simple instance (non-param) path
        size_t end_i; std::string flat;
        if(!collect_instance(lines,i,end_i,flat)){
            out.push_back(line); ++i; continue;
        }
        std::string port_list;
        if(!extract_port_list(flat,port_list)){
            for(size_t k=i;k<end_i;++k) out.push_back(lines[k]);
            i=end_i; continue;
        }
        std::vector<std::pair<std::string,std::string>> ports;
        if(!parse_named_ports(port_list,ports)){
            for(size_t k=i;k<end_i;++k) out.push_back(lines[k]);
            i=end_i; continue;
        }
        int max_port=0,max_sig=0;
        for(auto&[p,s]:ports){
            max_port=std::max(max_port,(int)p.size());
            max_sig=std::max(max_sig,(int)s.size());
        }
        int eff_before=std::max(1,m_before-max_port);
        int eff_inside=std::max(0,m_inside-max_sig);

        std::string hdr=indent+module_type+" "+inst_name+" (";
        out.push_back(hdr);
        for(size_t k=0;k<ports.size();++k){
            auto&[port,sig]=ports[k];
            std::string comma=(k+1==ports.size())?"":",";
            std::string pline;
            if(adaptive){
                int sb=std::max(1,m_before-(int)port.size());
                int si=std::max(0,m_inside-(int)sig.size());
                pline=indent+port_indent+"."+port+std::string(sb,' ')+"("+sig+std::string(si,' ')+")"+comma;
            } else {
                std::string pname=port; pname.resize(std::max((int)pname.size(),max_port),' ');
                std::string sname=sig; sname.resize(std::max((int)sname.size(),max_sig),' ');
                pline=indent+port_indent+"."+pname+std::string(eff_before,' ')+"("+sname+std::string(eff_inside,' ')+")"+comma;
            }
            while(!pline.empty()&&pline.back()==' ') pline.pop_back();
            out.push_back(pline);
        }
        out.push_back(indent+");");
        i=end_i;
    }
    std::string r; for(size_t k=0;k<out.size();++k){if(k)r+='\n'; r+=out[k];} return r;
}

// ---------------------------------------------------------------------------
// Function/task call formatting pass
// ---------------------------------------------------------------------------

static std::string trim_copy(const std::string& s) {
    size_t a=0; while(a<s.size()&&(s[a]==' '||s[a]=='\t')) ++a;
    size_t b=s.size(); while(b>a&&(s[b-1]==' '||s[b-1]=='\t')) --b;
    return s.substr(a,b-a);
}

static bool find_simple_call(const std::string& line, size_t& name_start,
                             size_t& name_end, size_t& open, size_t& close) {
    static const std::unordered_set<std::string> SKIP = {
        "if","for","foreach","while","repeat","wait","case","casex","casez",
        "module","macromodule","function","task","covergroup","class","property",
        "sequence","assert","assume","cover"
    };
    for(size_t i=0;i<line.size();++i) {
        unsigned char ch=(unsigned char)line[i];
        if(!(std::isalpha(ch)||line[i]=='_'||line[i]=='$')) continue;
        size_t s=i++;
        while(i<line.size()&&(std::isalnum((unsigned char)line[i])||line[i]=='_'||line[i]=='$')) ++i;
        size_t e=i;
        size_t j=i;
        while(j<line.size()&&(line[j]==' '||line[j]=='\t')) ++j;
        if(j>=line.size()||line[j]!='(') continue;
        if(s>0 && line[s-1]=='.') continue;
        std::string name=line.substr(s,e-s);
        if(has(SKIP,lower(name))) continue;
        int depth=1;
        size_t k=j+1;
        for(;k<line.size();++k) {
            if(line[k]=='(') ++depth;
            else if(line[k]==')' && --depth==0) break;
        }
        if(k>=line.size()) return false;
        name_start=s; name_end=e; open=j; close=k;
        return true;
    }
    return false;
}

static std::string render_call_single(const std::string& prefix, const std::string& name,
                                      const std::vector<std::string>& args,
                                      const std::string& suffix,
                                      const FunctionOptions& fo) {
    std::string r = prefix + name + (fo.space_before_paren ? " " : "") + "(";
    if(fo.space_inside_paren && !args.empty()) r += " ";
    for(size_t i=0;i<args.size();++i) {
        if(i) r += ", ";
        r += args[i];
    }
    if(fo.space_inside_paren && !args.empty()) r += " ";
    r += ")" + suffix;
    return r;
}

static std::string format_function_calls_pass(const std::string& text, const FormatOptions& opts) {
    const auto& fo = opts.function;
    auto disabled = find_disabled(text);
    std::vector<std::string> lines;
    { std::istringstream ss(text); std::string l; while(std::getline(ss,l)) lines.push_back(l); }

    std::vector<std::string> out;
    int pos = 0;
    for(const auto& line:lines) {
        int line_start = pos;
        pos += (int)line.size() + 1; // +1 for the newline consumed by getline
        if(line.find('\n')!=std::string::npos) { out.push_back(line); continue; }
        if(in_disabled(line_start, disabled)) { out.push_back(line); continue; }
        size_t ns=0,ne=0,op=0,cl=0;
        if(!find_simple_call(line, ns, ne, op, cl)) { out.push_back(line); continue; }
        std::string args_text = line.substr(op+1, cl-op-1);
        auto raw_args = split_top_level(args_text);
        std::vector<std::string> args;
        for(auto& a:raw_args) {
            auto t=trim_copy(a);
            if(!t.empty()) args.push_back(t);
        }

        std::string prefix = line.substr(0, ns);
        std::string name = line.substr(ns, ne-ns);
        std::string suffix = line.substr(cl+1);
        std::string prefix_trimmed = trim_copy(prefix);
        std::string prefix_lower = lower(prefix_trimmed);
        if (std::regex_match(prefix_trimmed, std::regex(R"([A-Za-z_$][\w$]*)"))
                || prefix_lower.find("function") != std::string::npos
                || prefix_lower.find("task") != std::string::npos
                || prefix_lower.find("module") != std::string::npos
                || prefix_lower.find("class") != std::string::npos) {
            out.push_back(line);
            continue;
        }
        std::string single = render_call_single(prefix, name, args, suffix, fo);

        bool do_break = false;
        if(fo.break_policy=="always") {
            do_break = !args.empty();
        } else if(fo.break_policy=="auto") {
            do_break = ((int)single.size() > fo.line_length)
                    || (fo.arg_count >= 0 && (int)args.size() >= fo.arg_count);
        }
        if(!do_break || fo.break_policy=="never") {
            out.push_back(single);
            continue;
        }

        std::string open_text = prefix + name + (fo.space_before_paren ? " " : "") + "(";
        if(fo.layout=="hanging") {
            std::string hang(open_text.size(), ' ');
            std::string r = open_text;
            for(size_t i=0;i<args.size();++i) {
                if(i) r += "\n" + hang;
                r += args[i];
                if(i+1<args.size()) r += ",";
            }
            r += ")" + suffix;
            out.push_back(r);
        } else {
            size_t base_len=0;
            while(base_len<prefix.size()&&(prefix[base_len]==' '||prefix[base_len]=='\t')) ++base_len;
            std::string base_indent = prefix.substr(0, base_len);
            std::string arg_indent = base_indent + std::string(std::max(0, opts.indent_size), ' ');
            std::string r = open_text + "\n";
            for(size_t i=0;i<args.size();++i) {
                r += arg_indent + args[i];
                if(i+1<args.size()) r += ",";
                r += "\n";
            }
            r += base_indent + ")" + suffix;
            out.push_back(r);
        }
    }
    std::string r; for(size_t k=0;k<out.size();++k){if(k)r+='\n'; r+=out[k];} return r;
}

// ---------------------------------------------------------------------------
// Module port-list formatting pass
// ---------------------------------------------------------------------------

static std::string format_portlist_pass(const std::string& text, const FormatOptions& opts) {
    // Matches single-line module headers: module foo (...);
    // Captures: (prefix_up_to_open_paren, ports_string, ");")
    static const std::regex MODULE_HDR_RE(
        R"(^([ \t]*(?:module|macromodule)\b(?:[^(\n]|#\([^)]*\))*\()([^)\n]+?)(\)\s*;)\s*$)",
        std::regex::icase | std::regex::multiline
    );

    const std::string indent_unit(opts.indent_size, ' ');

    std::string result = text;
    std::string out;
    size_t search_pos = 0;
    std::sregex_iterator it(result.begin(), result.end(), MODULE_HDR_RE);
    std::sregex_iterator end_it;

    // Collect all matches and replacements
    std::vector<std::tuple<size_t,size_t,std::string>> replacements;

    for(; it!=end_it; ++it){
        const std::smatch& ms = *it;
        std::string prefix    = ms[1].str();
        std::string ports_str = ms[2].str();
        std::string suffix    = ms[3].str();
        size_t mstart = ms.position();
        size_t mlen   = ms.length();

        auto ports = split_top_level(ports_str);
        // Trim each port
        std::vector<std::string> trimmed_ports;
        for(auto& p:ports){
            size_t a=0; while(a<p.size()&&(p[a]==' '||p[a]=='\t')) ++a;
            size_t b=p.size(); while(b>a&&(p[b-1]==' '||p[b-1]=='\t')) --b;
            if(b>a) trimmed_ports.push_back(p.substr(a,b-a));
        }
        if(trimmed_ports.empty()) continue;

        // Leading whitespace
        std::string leading_ws;
        for(size_t k=0;k<prefix.size()&&(prefix[k]==' '||prefix[k]=='\t');++k) leading_ws+=prefix[k];
        std::string port_indent = leading_ws + indent_unit;

        // Check ANSI vs non-ANSI
        static const std::unordered_set<std::string> ANSI_DIR={"input","output","inout","ref"};
        bool is_ansi=false;
        for(auto& p:trimmed_ports){
            std::istringstream ss(p); std::string first; ss>>first;
            if(!first.empty()&&has(ANSI_DIR,lower(first))){ is_ansi=true; break; }
        }

        std::string new_text;
        if(is_ansi){
            // One port per line, then apply port_declaration alignment
            std::string port_lines;
            for(size_t k=0;k<trimmed_ports.size();++k){
                std::string comma=(k+1<trimmed_ports.size())?",":"";
                port_lines += port_indent + trimmed_ports[k] + comma + "\n";
            }
            // Remove trailing newline for alignment pass input
            if(!port_lines.empty()&&port_lines.back()=='\n') port_lines.pop_back();
            if(opts.port_declaration.align)
                port_lines = align_port_pass(port_lines, opts);
            new_text = prefix + "\n" + port_lines + "\n" + leading_ws + suffix;
        } else {
            // Check all are simple identifiers
            static const std::regex SIMPLE_ID(R"(^[A-Za-z_$][\w$]*$)");
            bool all_simple=true;
            for(auto& p:trimmed_ports){
                if(!std::regex_match(p,SIMPLE_ID)){ all_simple=false; break; }
            }
            if(!all_simple) continue; // leave unchanged

            std::string port_block;
            if(opts.port.non_ansi_port_per_line_enabled && opts.port.non_ansi_port_per_line>0){
                int n=opts.port.non_ansi_port_per_line;
                for(size_t gi=0;gi<trimmed_ports.size();gi+=(size_t)n){
                    size_t end_g=std::min(gi+(size_t)n,trimmed_ports.size());
                    std::string comma=(end_g<trimmed_ports.size())?",":"";
                    std::string grp_line=port_indent;
                    for(size_t k=gi;k<end_g;++k){
                        if(k>gi) grp_line+=", ";
                        grp_line+=trimmed_ports[k];
                    }
                    grp_line+=comma;
                    port_block+=grp_line+"\n";
                }
            } else if(opts.port.non_ansi_port_max_line_length_enabled && opts.port.non_ansi_port_max_line_length>0){
                int max_len=opts.port.non_ansi_port_max_line_length;
                std::vector<std::string> current;
                for(size_t pi=0;pi<trimmed_ports.size();++pi){
                    std::string candidate=port_indent;
                    for(size_t k=0;k<current.size();++k){ if(k) candidate+=", "; candidate+=current[k]; }
                    candidate+=", "+trimmed_ports[pi];
                    if(!current.empty()&&(int)candidate.size()>max_len){
                        std::string row=port_indent;
                        for(size_t k=0;k<current.size();++k){ if(k) row+=", "; row+=current[k]; }
                        row+=",";
                        port_block+=row+"\n";
                        current={trimmed_ports[pi]};
                    } else {
                        current.push_back(trimmed_ports[pi]);
                    }
                }
                if(!current.empty()){
                    std::string row=port_indent;
                    for(size_t k=0;k<current.size();++k){ if(k) row+=", "; row+=current[k]; }
                    port_block+=row+"\n";
                }
            } else {
                for(size_t k=0;k<trimmed_ports.size();++k){
                    std::string comma=(k+1<trimmed_ports.size())?",":"";
                    port_block+=port_indent+trimmed_ports[k]+comma+"\n";
                }
            }
            if(!port_block.empty()&&port_block.back()=='\n') port_block.pop_back();
            new_text = prefix + "\n" + port_block + "\n" + leading_ws + suffix;
        }
        replacements.push_back({mstart, mlen, new_text});
    }

    // Apply replacements in reverse order
    std::sort(replacements.begin(), replacements.end(),
              [](const auto& a, const auto& b){ return std::get<0>(a) > std::get<0>(b); });
    for(auto&[pos,len,rep]:replacements){
        result.replace(pos,len,rep);
    }
    return result;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// format_source — main entry point
// ---------------------------------------------------------------------------

std::string format_source(const std::string& source, const FormatOptions& opts) {
    const std::string indent_unit(opts.indent_size, ' ');
    auto disabled = find_disabled(source);
    auto tokens   = tokenize(source);

    std::string out;
    out.reserve(source.size() + source.size()/4);

    int  indent_level = 0;
    std::vector<int> indent_stack;
    bool at_bol     = true;
    int  dim_depth  = 0;
    int  paren_depth= 0;
    int  do_depth   = 0;
    bool pending_nl = false;
    int  blank_pend = 0;
    bool in_pp_cond = false;
    bool after_dis  = false;
    bool struct_pend= false;
    std::vector<std::string> brace_stk;

    const Tok* prev = nullptr;

    auto flush_nl = [&]() {
        if (pending_nl) { out+='\n'; at_bol=true; pending_nl=false; }
        if (blank_pend>0) {
            if (!at_bol) { out+='\n'; at_bol=true; }
            for (int k=0;k<blank_pend;++k) { out+='\n'; at_bol=true; }
            blank_pend=0;
        }
    };
    auto emit = [&](const std::string& text) {
        if (at_bol) {
            for (int k=0;k<indent_level;++k) out+=indent_unit;
            at_bol=false;
        }
        out+=text;
    };

    for (size_t i=0; i<tokens.size(); ++i) {
        const Tok& tok=tokens[i];

        // Disabled region — pass through verbatim
        if (in_disabled(tok.pos, disabled)) {
            flush_nl();
            out+=tok.text;
            at_bol=!tok.text.empty()&&tok.text.back()=='\n';
            after_dis=!at_bol;
            continue; // don't update prev
        }

        // Whitespace
        if (tok.ftt==FTT::Whitespace) {
            int nl=(int)std::count(tok.text.begin(),tok.text.end(),'\n');
            if (after_dis&&nl>=1) pending_nl=true;
            after_dis=false;
            if (nl>1) {
                int extra=std::min(nl-1, opts.blank_lines_between_items);
                blank_pend=std::max(blank_pend, extra);
            }
            continue;
        }

        // Spacing / break decision
        bool in_dim=dim_depth>0;
        int  spaces=0;
        SD   dec=SD::Undecided;
        if (prev) {
            spaces=spaces_req(*prev,tok,opts,in_dim);
            dec=break_dec(*prev,tok,opts,in_dim);
        }

        // Special: "end while" — kMustAppend only inside do...while
        if (prev&&prev->lo=="end"&&tok.ftt==FTT::Keyword&&tok.lo=="while") {
            if (do_depth>0) { dec=SD::MustAppend; --do_depth; }
            else            { dec=SD::Undecided; }
        }

        if (dec==SD::MustWrap) {
            pending_nl=false;
            if (!at_bol) { out+='\n'; at_bol=true; }
            for (int k=0;k<blank_pend;++k) out+='\n';
            blank_pend=0;
        } else if (dec==SD::MustAppend) {
            if (pending_nl) { pending_nl=false; blank_pend=0; }
            if (!at_bol&&spaces>0) out+=std::string(spaces,' ');
        } else {
            flush_nl();
            if (!at_bol&&spaces>0) out+=std::string(spaces,' ');
        }

        // Indent-close: decrement BEFORE emit
        if (tok.ftt==FTT::Keyword&&has(INDENT_CLOSE,tok.lo)) {
            int delta=indent_stack.empty()?1:indent_stack.back();
            if (!indent_stack.empty()) indent_stack.pop_back();
            indent_level=std::max(0,indent_level-delta);
        } else if (tok.ftt==FTT::CloseGroup&&tok.text=="}"
                   &&!brace_stk.empty()&&brace_stk.back()=="struct") {
            int delta=indent_stack.empty()?1:indent_stack.back();
            if (!indent_stack.empty()) indent_stack.pop_back();
            indent_level=std::max(0,indent_level-delta);
        }

        // Emit token
        if (tok.ftt==FTT::Keyword) emit(kw_case(tok.text,opts.keyword_case));
        else                        emit(tok.text);

        // Track depths
        if      (tok.text=="[")             ++dim_depth;
        else if (tok.text=="]"&&dim_depth>0)--dim_depth;
        else if (tok.text=="(")             ++paren_depth;
        else if (tok.text==")"&&paren_depth>0)--paren_depth;
        else if (tok.ftt==FTT::Semicolon)   dim_depth=0;

        // Post-emit: indent-open, pending_nl, pp-cond tracking
        if (tok.ftt==FTT::Keyword) {
            if (tok.lo=="do") ++do_depth;
            if (has(INDENT_OPEN,tok.lo)) {
                int delta=(tok.lo=="module"||tok.lo=="macromodule")
                          ? opts.default_indent_level_inside_module_block : 1;
                indent_level+=delta;
                indent_stack.push_back(delta);
                if (has(BLOCK_OPEN,tok.lo)) pending_nl=true;
            } else if (has(INDENT_CLOSE,tok.lo)) {
                pending_nl=true;
            } else if (tok.lo=="struct"||tok.lo=="union") {
                struct_pend=true;
            }
        } else if (tok.ftt==FTT::OpenGroup&&tok.text=="{") {
            if (struct_pend) {
                brace_stk.push_back("struct");
                pending_nl=true; indent_level+=1; indent_stack.push_back(1);
            } else {
                brace_stk.push_back("other");
            }
            struct_pend=false;
        } else if (tok.ftt==FTT::CloseGroup&&tok.text=="}") {
            if (!brace_stk.empty()) brace_stk.pop_back();
        } else if (tok.ftt==FTT::Semicolon) {
            if (paren_depth==0) pending_nl=true;
        } else if (tok.ftt==FTT::EolComment||tok.ftt==FTT::IncludeDirective) {
            pending_nl=true;
        } else if (tok.ftt==FTT::CommentBlock) {
            if (i+1<tokens.size()&&tokens[i+1].ftt==FTT::Whitespace
                    &&tokens[i+1].text.find('\n')!=std::string::npos)
                pending_nl=true;
        } else if (tok.ftt==FTT::Identifier) {
            if      (has(PP_COND_BARE,tok.lo)) { pending_nl=true; in_pp_cond=false; }
            else if (has(PP_COND_WITH,tok.lo))   in_pp_cond=true;
            else if (in_pp_cond)               { pending_nl=true; in_pp_cond=false; }
        } else if (in_pp_cond) {
            pending_nl=true; in_pp_cond=false;
        }

        prev=&tok;
    }

    if (!at_bol) out+='\n';

    // Collapse extra trailing newlines to one
    while (out.size()>=2 && out[out.size()-1]=='\n' && out[out.size()-2]=='\n')
        out.pop_back();

    // Alignment passes (order matches Python format_source)
    if (opts.statement.align)
        out = align_assign_pass(out, opts);
    if (opts.port_declaration.align)
        out = align_port_pass(out, opts);
    if (opts.var_declaration.align)
        out = align_var_pass(out, opts);
    if (opts.instance.align)
        out = expand_instances_pass(out, opts);
    out = format_function_calls_pass(out, opts);
    // Always run port list pass
    out = format_portlist_pass(out, opts);

    // Ensure exactly one trailing newline (mirrors Python: result.rstrip('\n') + '\n')
    while (!out.empty() && out.back()=='\n') out.pop_back();
    out += '\n';

    // Safe-mode: abort if non-whitespace changed.
    if (opts.safe_mode) {
        auto strip=[](const std::string& s){
            std::string r; r.reserve(s.size());
            for(char c:s) if(!std::isspace((unsigned char)c)) r+=c;
            return r;
        };
        if (strip(source)!=strip(out))
            throw SafeModeError(
                "Formatter safe-mode: non-whitespace content changed — formatting aborted");
    }

    // trailing_newline option
    if (!opts.trailing_newline && !out.empty() && out.back()=='\n')
        out.pop_back();

    return out;
}
