#include "midori/mdsl.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Midori {

namespace {

// One syntax-directed pass: each parse function checks its piece and returns the HLSL for it, while appending its
// instructions for the interpreter, with no syntax tree in between. Instructions come out in the order a stack machine
// runs them: an operator after both of its operands. The target's own compiler optimizes; this only has to reject
// what is wrong, in the user's source.

template<class... Parts>
std::string Cat(const Parts&... parts) {
    std::string s;
    (s.append(parts), ...);
    return s;
}

constexpr std::array<std::string_view, 13> kTypeNames{
    "error",      "void",       "bool",        "float",       "vec2",  "vec3",    "vec4",
    "StampInput", "PaintInput", "StampOutput", "PaintOutput", "curve", "texture",
};
// The input structs are never values in HLSL: their fields are the m_ statics. stamp() is not translated to HLSL.
constexpr std::array<std::string_view, 13> kHlslTypes{
    "", "void", "bool", "float", "float2", "float3", "float4", "", "", "", "Paint", "", "",
};

constexpr std::array<std::string_view, 9> kKeywords{"param", "range",  "if",   "else", "for",
                                                    "in",    "return", "true", "false"};

constexpr int kLoopIterationsMax = 64;
// The interpreter's fixed storage, in values of 4 floats, and its calls in progress.
constexpr std::uint32_t kStackMax = 64;
constexpr std::uint32_t kLocalsMax = 256;
constexpr std::uint32_t kCallDepthMax = 16;

using Lane = std::array<float, 4>;

bool IsNumeric(MdslType type) noexcept {
    return MdslComponents(type) > 0;
}

MdslType Vec(std::uint32_t components) noexcept {
    return static_cast<MdslType>(static_cast<std::uint32_t>(MdslType::Float) + components - 1);
}

std::string_view TypeName(MdslType type) noexcept {
    return kTypeNames[std::to_underlying(type)];
}

std::string_view HlslType(MdslType type) noexcept {
    return kHlslTypes[std::to_underlying(type)];
}

MdslType TypeFromName(std::string_view name) noexcept {
    for (std::size_t i = 1; i < kTypeNames.size(); ++i) {
        if (kTypeNames[i] == name) {
            return static_cast<MdslType>(i);
        }
    }
    return MdslType::Error;
}

bool IsReserved(std::string_view name) noexcept {
    return TypeFromName(name) != MdslType::Error || std::ranges::find(kKeywords, name) != kKeywords.end();
}

// Where something can be used, one bit each: paint() runs on the GPU, stamp() on the CPU.
constexpr std::uint8_t kStagePaint = 1;
constexpr std::uint8_t kStageStamp = 2;
constexpr std::uint8_t kStageAll = kStagePaint | kStageStamp;

// Component-wise, scalar arguments splatted to the widest vector argument.
struct Intrinsic {
    std::string_view name;
    std::string_view hlsl;
    MdslOpCode op;
    std::uint8_t arity;
    bool scalar; // returns float
    bool sized;  // the op needs the argument's component count
    std::uint8_t stages{kStageAll};
};

constexpr Intrinsic kIntrinsics[]{
    {"abs", "abs", MdslOpCode::Abs, 1, false, false},
    {"floor", "floor", MdslOpCode::Floor, 1, false, false},
    {"ceil", "ceil", MdslOpCode::Ceil, 1, false, false},
    {"fract", "frac", MdslOpCode::Fract, 1, false, false},
    {"sqrt", "sqrt", MdslOpCode::Sqrt, 1, false, false},
    {"exp", "exp", MdslOpCode::Exp, 1, false, false},
    {"log", "log", MdslOpCode::Log, 1, false, false},
    {"sin", "sin", MdslOpCode::Sin, 1, false, false},
    {"cos", "cos", MdslOpCode::Cos, 1, false, false},
    {"normalize", "normalize", MdslOpCode::Normalize, 1, false, true},
    {"pow", "pow", MdslOpCode::Pow, 2, false, false},
    {"min", "min", MdslOpCode::Min, 2, false, false},
    {"max", "max", MdslOpCode::Max, 2, false, false},
    {"step", "step", MdslOpCode::Step, 2, false, false},
    {"atan2", "atan2", MdslOpCode::Atan2, 2, false, false},
    {"clamp", "clamp", MdslOpCode::Clamp, 3, false, false},
    {"mix", "lerp", MdslOpCode::Mix, 3, false, false},
    {"smoothstep", "smoothstep", MdslOpCode::Smoothstep, 3, false, false},
    {"length", "length", MdslOpCode::Length, 1, true, true},
    {"distance", "distance", MdslOpCode::Distance, 2, true, true},
    {"dot", "dot", MdslOpCode::Dot, 2, true, true},
};

const Intrinsic* FindIntrinsic(std::string_view name) noexcept {
    for (const Intrinsic& intrinsic : kIntrinsics) {
        if (intrinsic.name == name) {
            return &intrinsic;
        }
    }
    return nullptr;
}

struct Input {
    std::string_view name;
    MdslType type;
    std::uint8_t stages;
};

// An input's index is its slot in the interpreter, and the order BrushPaint takes them in.
constexpr Input kInputs[]{
    {"local", MdslType::Vec2, kStagePaint},  {"radius", MdslType::Float, kStagePaint},
    {"color", MdslType::Vec4, kStagePaint},  {"pressure", MdslType::Float, kStageAll},
    {"tilt", MdslType::Vec2, kStageAll},     {"position", MdslType::Vec2, kStagePaint},
    {"velocity", MdslType::Vec2, kStageAll}, {"direction", MdslType::Vec2, kStageAll},
    {"random", MdslType::Float, kStageAll},  {"along", MdslType::Float, kStageAll},
    {"time", MdslType::Float, kStageAll},
};

struct Field {
    std::string_view name;
    MdslType type;
};

// 4 floats: the interpreter returns them as one value.
constexpr Field kStampOutputFields[]{
    {"radius", MdslType::Float}, {"spacing", MdslType::Float}, {"offset", MdslType::Vec2}};
constexpr Field kPaintOutputFields[]{
    {"color", MdslType::Vec3}, {"amount", MdslType::Float}, {"opacity", MdslType::Float}};

bool IsInputStruct(MdslType type) noexcept {
    return type == MdslType::StampInput || type == MdslType::PaintInput;
}

bool IsOutputStruct(MdslType type) noexcept {
    return type == MdslType::StampOutput || type == MdslType::PaintOutput;
}

// An input struct's fields are the kInputs of its stage.
std::uint8_t InputStage(MdslType type) noexcept {
    return type == MdslType::StampInput ? kStageStamp : kStagePaint;
}

std::span<const Field> OutputFields(MdslType type) noexcept {
    if (type == MdslType::StampOutput) {
        return kStampOutputFields;
    }
    return kPaintOutputFields;
}

struct Entry {
    std::string_view name;
    MdslType input;
    MdslType output;
};

constexpr Entry kEntries[]{
    {.name = "stamp", .input = MdslType::StampInput, .output = MdslType::StampOutput},
    {.name = "paint", .input = MdslType::PaintInput, .output = MdslType::PaintOutput},
};

const Entry* FindEntry(std::string_view name) noexcept {
    const auto* const found = std::ranges::find(kEntries, name, &Entry::name);
    return found == std::end(kEntries) ? nullptr : &*found;
}

constexpr std::uint32_t kInputPressure = 3;
constexpr std::uint32_t kInputTilt = 4;
constexpr std::uint32_t kInputVelocity = 6;
constexpr std::uint32_t kInputDirection = 7;
constexpr std::uint32_t kInputRandom = 8;
constexpr std::uint32_t kInputAlong = 9;
constexpr std::uint32_t kInputTime = 10;

// Written once in mdsl, translated ahead of the user's code.
constexpr std::string_view kPrelude = R"(
float hash(vec2 p) {
    vec2 q = fract(p * vec2(123.34, 456.21));
    q += dot(q, q + 45.32);
    return fract(q.x * q.y);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p) {
    float sum = 0.0;
    float amp = 0.5;
    vec2 q = p;
    for i in 0..5 {
        sum += amp * noise(q);
        q = q * 2.03 + vec2(17.0, 9.2);
        amp *= 0.5;
    }
    return sum;
}

float cells(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float best = 8.0;
    for y in 0..3 {
        for x in 0..3 {
            vec2 o = vec2(x - 1.0, y - 1.0);
            vec2 h = vec2(hash(i + o), hash(i + o + 7.31));
            best = min(best, length(o + h - f));
        }
    }
    return best;
}
)";

enum struct Tok : std::uint8_t {
    End,
    Invalid,
    Ident,
    Number,
    String,
    LParen,
    RParen,
    LBrace,
    RBrace,
    Comma,
    Semi,
    Dot,
    DotDot,
    Plus,
    Minus,
    Star,
    Slash,
    Bang,
    Less,
    Greater,
    LessEq,
    GreaterEq,
    EqEq,
    NotEq,
    AndAnd,
    OrOr,
    Assign,
    PlusEq,
    MinusEq,
    StarEq,
    SlashEq,
};

// Longest first.
constexpr std::pair<std::string_view, Tok> kOperators[]{
    {"..", Tok::DotDot},  {"<=", Tok::LessEq}, {">=", Tok::GreaterEq}, {"==", Tok::EqEq},    {"!=", Tok::NotEq},
    {"&&", Tok::AndAnd},  {"||", Tok::OrOr},   {"+=", Tok::PlusEq},    {"-=", Tok::MinusEq}, {"*=", Tok::StarEq},
    {"/=", Tok::SlashEq}, {"(", Tok::LParen},  {")", Tok::RParen},     {"{", Tok::LBrace},   {"}", Tok::RBrace},
    {",", Tok::Comma},    {";", Tok::Semi},    {".", Tok::Dot},        {"+", Tok::Plus},     {"-", Tok::Minus},
    {"*", Tok::Star},     {"/", Tok::Slash},   {"!", Tok::Bang},       {"<", Tok::Less},     {">", Tok::Greater},
    {"=", Tok::Assign},
};

struct Token {
    std::string_view text;
    std::uint32_t line{};
    std::uint32_t column{};
    Tok kind{Tok::End};
};

struct Lexer {
    std::string_view source;
    std::size_t pos{};
    std::uint32_t line{1};
    std::uint32_t column{1};
};

bool IsDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool IsIdentStart(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool IsIdentChar(char c) noexcept {
    return IsIdentStart(c) || IsDigit(c);
}

char LexerPeek(const Lexer& lexer, std::size_t ahead) noexcept {
    return lexer.pos + ahead < lexer.source.size() ? lexer.source[lexer.pos + ahead] : '\0';
}

void LexerAdvance(Lexer& lexer, std::size_t count) noexcept {
    for (; count > 0 && lexer.pos < lexer.source.size(); --count, ++lexer.pos) {
        if (lexer.source[lexer.pos] == '\n') {
            ++lexer.line;
            lexer.column = 1;
        } else {
            ++lexer.column;
        }
    }
}

void LexerSkipTrivia(Lexer& lexer) noexcept {
    for (;;) {
        const char c = LexerPeek(lexer, 0);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            LexerAdvance(lexer, 1);
        } else if (c == '/' && LexerPeek(lexer, 1) == '/') {
            while (LexerPeek(lexer, 0) != '\n' && LexerPeek(lexer, 0) != '\0') {
                LexerAdvance(lexer, 1);
            }
        } else if (c == '/' && LexerPeek(lexer, 1) == '*') {
            LexerAdvance(lexer, 2);
            while (LexerPeek(lexer, 0) != '\0' && !(LexerPeek(lexer, 0) == '*' && LexerPeek(lexer, 1) == '/')) {
                LexerAdvance(lexer, 1);
            }
            LexerAdvance(lexer, 2);
        } else {
            return;
        }
    }
}

Token LexerNext(Lexer& lexer) noexcept {
    LexerSkipTrivia(lexer);
    Token token{.text = {}, .line = lexer.line, .column = lexer.column, .kind = Tok::End};
    if (lexer.pos >= lexer.source.size()) {
        return token;
    }
    const char c = LexerPeek(lexer, 0);
    std::size_t count = 0;
    if (IsIdentStart(c)) {
        while (IsIdentChar(LexerPeek(lexer, count))) {
            ++count;
        }
        token.kind = Tok::Ident;
    } else if (IsDigit(c) || (c == '.' && IsDigit(LexerPeek(lexer, 1)))) {
        while (IsDigit(LexerPeek(lexer, count))) {
            ++count;
        }
        // "0..8" stays 0 .. 8.
        if (LexerPeek(lexer, count) == '.' && LexerPeek(lexer, count + 1) != '.') {
            ++count;
            while (IsDigit(LexerPeek(lexer, count))) {
                ++count;
            }
        }
        token.kind = Tok::Number;
    } else if (c == '"') {
        // On one line, without escapes.
        count = 1;
        while (LexerPeek(lexer, count) != '"' && LexerPeek(lexer, count) != '\n' && LexerPeek(lexer, count) != '\0') {
            ++count;
        }
        token.kind = LexerPeek(lexer, count) == '"' ? Tok::String : Tok::Invalid;
        ++count;
    } else {
        token.kind = Tok::Invalid;
        count = 1;
        for (const auto& [spelling, kind] : kOperators) {
            if (lexer.source.substr(lexer.pos, spelling.size()) == spelling) {
                token.kind = kind;
                count = spelling.size();
                break;
            }
        }
    }
    token.text = lexer.source.substr(lexer.pos, count);
    LexerAdvance(lexer, count);
    return token;
}

// Locale-independent: the lexer guarantees [digits][.digits].
float ParseNumber(std::string_view text) noexcept {
    double value = 0.0;
    double scale = 1.0;
    bool fraction = false;
    for (const char c : text) {
        if (c == '.') {
            fraction = true;
            continue;
        }
        value = (value * 10.0) + (c - '0');
        if (fraction) {
            scale *= 10.0;
        }
    }
    return static_cast<float>(value / scale);
}

std::string Literal(std::string_view text) {
    std::string out;
    if (text.front() == '.') {
        out += '0';
    }
    out += text;
    if (!text.contains('.')) {
        out += ".0";
    } else if (text.back() == '.') {
        out += '0';
    }
    out += 'f';
    return out;
}

struct Value {
    std::string code;
    MdslType type{MdslType::Error};
};

enum struct SymbolKind : std::uint8_t {
    Param,
    Local,
    InputStruct,  // a function's input: its fields read the stage's inputs, it takes no storage
    OutputStruct, // an entry's output: a local per field
    Curve,        // called like a function
    Texture,      // likewise
};

struct Symbol {
    std::string_view name;
    std::string code;
    std::uint32_t index; // Param: its offset; Local: its slot; OutputStruct: its first field's; Curve, Texture: of
                         // their kind
    MdslType type;
    SymbolKind kind;
};

struct Function {
    std::string_view name;
    std::vector<MdslType> params;
    std::uint32_t maxStack;  // values on the stack while it runs, what it calls included
    std::uint32_t frame;     // locals, what it calls included
    std::uint32_t callDepth; // 1 plus what it calls
    std::uint32_t inputs;    // the kInputs it reads, what it calls included, one bit each
    std::uint8_t stages;     // where the intrinsics it calls, and what it calls, can run
    MdslType ret;
    bool prelude;
};

// The first error stops translation: it cuts the token stream, so every loop exits.
struct Translator {
    std::vector<Symbol> globals;
    std::vector<Symbol> locals;
    std::vector<Function> functions;
    std::vector<MdslParam> params;
    std::vector<MdslCurve> curves;
    std::vector<MdslTexture> textures;
    std::string code; // the translated functions, in definition order
    MdslProgram program;
    MdslError error;
    Lexer lexer;
    Token token;
    // Of the function being defined.
    std::uint32_t depth{};
    std::uint32_t maxStack{};
    std::uint32_t slots{};
    std::uint32_t calleeFrame{};
    std::uint32_t calleeDepth{};
    std::uint32_t inputs{};
    std::uint8_t stages{};
    std::uint32_t paramFloats{};
    std::uint32_t indent{};
    MdslType ret{};
    bool emitting{}; // paint() runs on the GPU only, so it gets no instructions
    bool user{};
    bool failed{};
};

void Emit(Translator& t, MdslOpCode code, int delta, std::uint32_t arg = 0, std::uint32_t count = 0,
          std::uint32_t pattern = 0) {
    t.depth = static_cast<std::uint32_t>(static_cast<int>(t.depth) + delta);
    t.maxStack = std::max(t.maxStack, t.depth);
    if (t.emitting) {
        t.program.ops.push_back(MdslOp{.arg = arg,
                                       .code = code,
                                       .count = static_cast<std::uint8_t>(count),
                                       .pattern = static_cast<std::uint8_t>(pattern)});
    }
}

void EmitConstant(Translator& t, float value) {
    const auto index = static_cast<std::uint32_t>(t.program.constants.size());
    t.program.constants.push_back(Lane{value, value, value, value});
    Emit(t, MdslOpCode::Const, 1, index);
}

std::uint32_t Here(const Translator& t) noexcept {
    return static_cast<std::uint32_t>(t.program.ops.size());
}

// The index of the jump to patch once its target is known.
std::uint32_t EmitJump(Translator& t, MdslOpCode code, int delta) {
    Emit(t, code, delta);
    return Here(t) - 1;
}

void PatchJump(Translator& t, std::uint32_t jump) noexcept {
    if (t.emitting) {
        t.program.ops[jump].arg = Here(t);
    }
}

void Bump(Translator& t) noexcept {
    t.token = LexerNext(t.lexer);
}

bool Accept(Translator& t, Tok kind) noexcept {
    if (t.token.kind != kind) {
        return false;
    }
    Bump(t);
    return true;
}

bool AcceptWord(Translator& t, std::string_view word) noexcept {
    if (t.token.kind != Tok::Ident || t.token.text != word) {
        return false;
    }
    Bump(t);
    return true;
}

void Fail(Translator& t, const Token& at, std::string message) noexcept {
    if (t.failed) {
        return;
    }
    t.failed = true;
    t.error = MdslError{.message = std::move(message), .line = t.user ? at.line : 0, .column = t.user ? at.column : 0};
    t.lexer = Lexer{};
    t.token = Token{};
}

Value Bad(Translator& t, const Token& at, std::string message) noexcept {
    Fail(t, at, std::move(message));
    return Value{};
}

void Expect(Translator& t, Tok kind, std::string_view what) noexcept {
    if (Accept(t, kind)) {
        return;
    }
    Fail(t, t.token,
         t.token.kind == Tok::End ? Cat("expected ", what, " at end of input")
                                  : Cat("expected ", what, " before '", t.token.text, "'"));
}

const Symbol* Find(const Translator& t, std::string_view name) noexcept {
    for (auto it = t.locals.rbegin(); it != t.locals.rend(); ++it) {
        if (it->name == name) {
            return &*it;
        }
    }
    for (const Symbol& symbol : t.globals) {
        if (symbol.name == name) {
            return &symbol;
        }
    }
    return nullptr;
}

const Function* FindFunction(const Translator& t, std::string_view name) noexcept {
    for (const Function& function : t.functions) {
        if (function.name == name) {
            return &function;
        }
    }
    return nullptr;
}

// Prefixes keep every user name clear of the target's keywords and the engine's own names.
void Claim(Translator& t, const Token& name) noexcept {
    if (t.failed) {
        return;
    }
    if (IsReserved(name.text)) {
        Fail(t, name, Cat("'", name.text, "' is a reserved word"));
        return;
    }
    if (name.text.front() == '_' || name.text.contains("__")) {
        Fail(t, name, "names can't start with '_' or contain '__'");
        return;
    }
    if (Find(t, name.text) != nullptr) {
        Fail(t, name, Cat("'", name.text, "' is already defined"));
    }
}

// Its slot.
std::uint32_t DeclareLocal(Translator& t, const Token& name, MdslType type) noexcept {
    Claim(t, name);
    const std::uint32_t slot = t.slots++;
    if (!t.failed) {
        t.locals.push_back(Symbol{
            .name = name.text, .code = Cat("u_", name.text), .index = slot, .type = type, .kind = SymbolKind::Local});
    }
    return slot;
}

void Line(const Translator& t, std::string& out, std::string_view text) {
    out.append(4 * std::size_t{t.indent}, ' ');
    out += text;
    out += '\n';
}

float SignedNumber(Translator& t) noexcept {
    const bool negative = Accept(t, Tok::Minus);
    const Token number = t.token;
    Expect(t, Tok::Number, "a number");
    if (t.failed) {
        return 0.0f;
    }
    const float value = ParseNumber(number.text);
    return negative ? -value : value;
}

int Integer(Translator& t) noexcept {
    const Token number = t.token;
    Expect(t, Tok::Number, "an integer");
    if (t.failed) {
        return 0;
    }
    if (number.text.contains('.') || number.text.size() > 4) {
        Fail(t, number, "expected a small integer");
        return 0;
    }
    return static_cast<int>(ParseNumber(number.text));
}

// `1.0` or `vec3(1, 0.5, 0)`: a default is a literal, not an expression.
void Constant(Translator& t, std::array<float, 4>& out, MdslType type) noexcept {
    const std::uint32_t components = MdslComponents(type);
    if (t.token.kind != Tok::Ident) {
        std::fill_n(out.begin(), components, SignedNumber(t));
        return;
    }
    const Token constructor = t.token;
    if (TypeFromName(constructor.text) != type) {
        Fail(t, constructor, Cat("expected a ", TypeName(type), " value"));
        return;
    }
    Bump(t);
    Expect(t, Tok::LParen, "'('");
    std::uint32_t count = 0;
    do {
        const float value = SignedNumber(t);
        if (count < components) {
            out[count] = value;
        }
        ++count;
    } while (Accept(t, Tok::Comma));
    Expect(t, Tok::RParen, "')'");
    if (count == 1) {
        std::fill_n(out.begin(), components, out[0]);
    } else if (count != components) {
        Fail(t, constructor, Cat(TypeName(type), " needs ", std::to_string(components), " values"));
    }
}

// `curve(0, 0, 0.5, 0.8, 1, 1)`: x, y pairs from x 0 to x 1, in increasing x, every y within 0 to 1.
void CurveConstant(Translator& t, Curve& out) noexcept {
    const Token constructor = t.token;
    if (!AcceptWord(t, "curve")) {
        Fail(t, constructor, "expected a curve value");
        return;
    }
    Expect(t, Tok::LParen, "'('");
    std::array<float, std::size_t{2} * kCurvePointsMax> values{};
    std::uint32_t count = 0;
    do {
        const Token at = t.token;
        const float value = SignedNumber(t);
        if (count == values.size()) {
            Fail(t, at, Cat("a curve has at most ", std::to_string(kCurvePointsMax), " points"));
            return;
        }
        values[count++] = value;
    } while (Accept(t, Tok::Comma));
    Expect(t, Tok::RParen, "')'");
    if (t.failed) {
        return;
    }
    if (count < 4 || count % 2 != 0) {
        Fail(t, constructor, "a curve takes x, y pairs, two points at least");
        return;
    }
    out.count = count / 2;
    for (std::uint32_t i = 0; i < out.count; ++i) {
        out.points[i] = CurvePoint{.x = values[std::size_t{2} * i], .y = values[(std::size_t{2} * i) + 1]};
        if (out.points[i].y < 0.0f || out.points[i].y > 1.0f) {
            Fail(t, constructor, "a curve's y values are within 0 to 1");
            return;
        }
        if (i > 0 && out.points[i].x <= out.points[i - 1].x) {
            Fail(t, constructor, "a curve's x values increase");
            return;
        }
    }
    if (out.points[0].x != 0.0f || out.points[out.count - 1].x != 1.0f) {
        Fail(t, constructor, "a curve goes from x 0 to x 1");
    }
}

// `param curve name = curve(...);` or `param texture name = "file.png";`.
void SamplerParam(Translator& t, MdslType type, const Token& name) noexcept {
    const bool curve = type == MdslType::Curve;
    const std::uint32_t max = curve ? kMdslCurvesMax : kMdslTexturesMax;
    const std::size_t count = curve ? t.curves.size() : t.textures.size();
    if (count == max) {
        Fail(t, name, Cat("a brush has at most ", std::to_string(max), curve ? " curves" : " textures"));
        return;
    }
    if (FindIntrinsic(name.text) != nullptr || FindFunction(t, name.text) != nullptr) {
        Fail(t, name, Cat("'", name.text, "' is already a function"));
        return;
    }
    if (curve) {
        MdslCurve param{.name = std::string(name.text), .value = {}};
        if (Accept(t, Tok::Assign)) {
            CurveConstant(t, param.value);
        }
        t.curves.push_back(std::move(param));
    } else {
        MdslTexture param{.name = std::string(name.text), .path = {}};
        if (Accept(t, Tok::Assign)) {
            const Token path = t.token;
            Expect(t, Tok::String, "a file name in quotes");
            if (!t.failed) {
                param.path = std::string(path.text.substr(1, path.text.size() - 2));
            }
        }
        t.textures.push_back(std::move(param));
    }
    Expect(t, Tok::Semi, "';'");
    t.globals.push_back(Symbol{.name = name.text,
                               .code = {},
                               .index = static_cast<std::uint32_t>(count),
                               .type = type,
                               .kind = curve ? SymbolKind::Curve : SymbolKind::Texture});
}

void Param(Translator& t) noexcept {
    const Token typeToken = t.token;
    const MdslType type = TypeFromName(typeToken.text);
    const bool sampler = type == MdslType::Curve || type == MdslType::Texture;
    if (!IsNumeric(type) && !sampler) {
        Fail(t, typeToken, "expected float, vec2, vec3, vec4, curve or texture");
        return;
    }
    Bump(t);
    const Token name = t.token;
    Expect(t, Tok::Ident, "a parameter name");
    Claim(t, name);
    if (t.failed) {
        return;
    }
    if (sampler) {
        SamplerParam(t, type, name);
        return;
    }
    if (t.paramFloats + MdslComponents(type) > kMdslParamFloatsMax) {
        Fail(t, name, Cat("a brush has at most ", std::to_string(kMdslParamFloatsMax), " param values"));
        return;
    }
    MdslParam param{.name = std::string(name.text), .offset = t.paramFloats, .type = type};
    if (Accept(t, Tok::Assign)) {
        Constant(t, param.value, type);
    }
    if (AcceptWord(t, "range")) {
        Expect(t, Tok::LParen, "'('");
        param.min = SignedNumber(t);
        Expect(t, Tok::Comma, "','");
        const Token max = t.token;
        param.max = SignedNumber(t);
        Expect(t, Tok::RParen, "')'");
        if (!t.failed && param.max < param.min) {
            Fail(t, max, "a range ends below where it starts");
            return;
        }
    }
    Expect(t, Tok::Semi, "';'");
    t.globals.push_back(Symbol{.name = name.text,
                               .code = Cat("u_", name.text),
                               .index = t.paramFloats,
                               .type = type,
                               .kind = SymbolKind::Param});
    t.paramFloats += MdslComponents(type);
    t.params.push_back(std::move(param));
}

Value Expression(Translator& t, int minPrecedence = 1);
bool Block(Translator& t, std::string& out, MdslType ret, std::string_view prologue = {});

int Precedence(Tok kind) noexcept {
    switch (kind) {
        case Tok::OrOr:
            return 1;
        case Tok::AndAnd:
            return 2;
        case Tok::EqEq:
        case Tok::NotEq:
            return 3;
        case Tok::Less:
        case Tok::Greater:
        case Tok::LessEq:
        case Tok::GreaterEq:
            return 4;
        case Tok::Plus:
        case Tok::Minus:
            return 5;
        case Tok::Star:
        case Tok::Slash:
            return 6;
        default:
            return 0;
    }
}

MdslOpCode BinaryOp(Tok kind) noexcept {
    switch (kind) {
        case Tok::OrOr:
            return MdslOpCode::Or;
        case Tok::AndAnd:
            return MdslOpCode::And;
        case Tok::EqEq:
            return MdslOpCode::Equal;
        case Tok::NotEq:
            return MdslOpCode::NotEqual;
        case Tok::Less:
            return MdslOpCode::Less;
        case Tok::Greater:
            return MdslOpCode::Greater;
        case Tok::LessEq:
            return MdslOpCode::LessEq;
        case Tok::GreaterEq:
            return MdslOpCode::GreaterEq;
        case Tok::Plus:
        case Tok::PlusEq:
            return MdslOpCode::Add;
        case Tok::Minus:
        case Tok::MinusEq:
            return MdslOpCode::Sub;
        case Tok::Star:
        case Tok::StarEq:
            return MdslOpCode::Mul;
        default:
            return MdslOpCode::Div;
    }
}

// Scalars mix with vectors; vectors only with their own size.
MdslType Arithmetic(MdslType a, MdslType b) noexcept {
    if (!IsNumeric(a) || !IsNumeric(b)) {
        return MdslType::Error;
    }
    if (a == b || b == MdslType::Float) {
        return a;
    }
    if (a == MdslType::Float) {
        return b;
    }
    return MdslType::Error;
}

Value Binary(Translator& t, const Token& op, const Value& a, const Value& b) {
    if (t.failed) {
        return Value{};
    }
    MdslType type = MdslType::Error;
    switch (op.kind) {
        case Tok::OrOr:
        case Tok::AndAnd:
            if (a.type == MdslType::Bool && b.type == MdslType::Bool) {
                type = MdslType::Bool;
            }
            break;
        case Tok::EqEq:
        case Tok::NotEq:
            if (a.type == b.type && (a.type == MdslType::Float || a.type == MdslType::Bool)) {
                type = MdslType::Bool;
            }
            break;
        case Tok::Less:
        case Tok::Greater:
        case Tok::LessEq:
        case Tok::GreaterEq:
            if (a.type == MdslType::Float && b.type == MdslType::Float) {
                type = MdslType::Bool;
            }
            break;
        default:
            type = Arithmetic(a.type, b.type);
            break;
    }
    if (type == MdslType::Error) {
        return Bad(t, op, Cat("cannot apply '", op.text, "' to ", TypeName(a.type), " and ", TypeName(b.type)));
    }
    Emit(t, BinaryOp(op.kind), -1);
    return Value{.code = Cat("(", a.code, " ", op.text, " ", b.code, ")"), .type = type};
}

// Normalized to xyzw: rgba and xyzw may not be mixed. `pattern` gets the components picked, 2 bits each.
Value Swizzle(Translator& t, const Value& value, bool lvalue, std::uint32_t& pattern) {
    const Token swizzle = t.token;
    Expect(t, Tok::Ident, "a swizzle");
    if (t.failed) {
        return Value{};
    }
    const std::uint32_t components = MdslComponents(value.type);
    if (components < 2) {
        return Bad(t, swizzle, Cat(TypeName(value.type), " has no components"));
    }
    if (swizzle.text.size() > 4) {
        return Bad(t, swizzle, "a swizzle has at most 4 components");
    }
    const std::string_view set =
        std::string_view("rgba").contains(swizzle.text.front()) ? std::string_view("rgba") : std::string_view("xyzw");
    std::string xyzw;
    std::uint32_t used = 0;
    pattern = 0;
    for (std::size_t i = 0; i < swizzle.text.size(); ++i) {
        const std::size_t index = set.find(swizzle.text[i]);
        if (index == std::string_view::npos || index >= components) {
            return Bad(t, swizzle, Cat("invalid swizzle '", swizzle.text, "' on ", TypeName(value.type)));
        }
        if (lvalue && (used & (1U << index)) != 0) {
            return Bad(t, swizzle, "a component can only be written once");
        }
        used |= 1U << index;
        pattern |= static_cast<std::uint32_t>(index) << (2 * i);
        xyzw += "xyzw"[index];
    }
    const auto count = static_cast<std::uint32_t>(swizzle.text.size());
    if (!lvalue) {
        Emit(t, MdslOpCode::Swizzle, 0, 0, count, pattern);
    }
    return Value{.code = Cat(value.code, ".", xyzw), .type = Vec(count)};
}

// HLSL has no float3(x). The interpreter needs nothing: a float already fills every component.
std::string Splat(const Value& value, MdslType to) {
    return value.type == to ? value.code : Cat("((", HlslType(to), ")", value.code, ")");
}

// The sizes of up to 4 values, 2 bits each, one less than the size.
std::uint32_t SizePattern(const std::vector<Value>& args) noexcept {
    std::uint32_t pattern = 0;
    for (std::size_t i = 0; i < args.size() && i < 4; ++i) {
        pattern |= (std::max(MdslComponents(args[i].type), 1U) - 1) << (2 * i);
    }
    return pattern;
}

Value Construct(Translator& t, const Token& at, MdslType type, const std::vector<Value>& args) {
    const int popped = 1 - static_cast<int>(args.size());
    std::uint32_t total = 0;
    for (const Value& arg : args) {
        if (!IsNumeric(arg.type)) {
            return Bad(t, at, Cat(TypeName(type), "() takes numbers, got ", TypeName(arg.type)));
        }
        total += MdslComponents(arg.type);
    }
    if (args.size() == 1 && args[0].type == MdslType::Float) {
        return Value{.code = Splat(args[0], type), .type = type};
    }
    if (total != MdslComponents(type)) {
        return Bad(t, at,
                   Cat(TypeName(type), "() needs ", std::to_string(MdslComponents(type)), " components, got ",
                       std::to_string(total)));
    }
    Emit(t, MdslOpCode::Construct, popped, 0, static_cast<std::uint32_t>(args.size()), SizePattern(args));
    std::string code = Cat(HlslType(type), "(");
    for (std::size_t i = 0; i < args.size(); ++i) {
        code += Cat(i != 0 ? ", " : "", args[i].code);
    }
    return Value{.code = code + ")", .type = type};
}

Value CallIntrinsic(Translator& t, const Token& at, const Intrinsic& intrinsic, const std::vector<Value>& args) {
    if (args.size() != intrinsic.arity) {
        return Bad(t, at, Cat(intrinsic.name, "() takes ", std::to_string(intrinsic.arity), " argument(s)"));
    }
    std::uint32_t components = 1;
    for (const Value& arg : args) {
        if (!IsNumeric(arg.type)) {
            return Bad(t, at, Cat(intrinsic.name, "() takes numbers, got ", TypeName(arg.type)));
        }
        components = std::max(components, MdslComponents(arg.type));
    }
    for (const Value& arg : args) {
        if (MdslComponents(arg.type) != 1 && MdslComponents(arg.type) != components) {
            return Bad(t, at, Cat("mixed vector sizes in ", intrinsic.name, "()"));
        }
    }
    t.stages = static_cast<std::uint8_t>(t.stages & intrinsic.stages);
    if ((intrinsic.stages & kStageStamp) != 0) {
        Emit(t, intrinsic.op, 1 - static_cast<int>(args.size()), 0, intrinsic.sized ? components : 0);
    } else {
        t.depth -= static_cast<std::uint32_t>(args.size()) - 1;
    }
    const MdslType type = Vec(components);
    std::string code = Cat(intrinsic.hlsl, "(");
    for (std::size_t i = 0; i < args.size(); ++i) {
        code += Cat(i != 0 ? ", " : "", Splat(args[i], type));
    }
    return Value{.code = code + ")", .type = intrinsic.scalar ? MdslType::Float : type};
}

Value CallFunction(Translator& t, const Token& at, const Function& function, const std::vector<Value>& args) {
    bool match = args.size() == function.params.size();
    for (std::size_t i = 0; match && i < args.size(); ++i) {
        match = args[i].type == function.params[i];
    }
    if (!match) {
        std::string signature = Cat(TypeName(function.ret), " ", function.name, "(");
        for (std::size_t i = 0; i < function.params.size(); ++i) {
            signature += Cat(i != 0 ? ", " : "", TypeName(function.params[i]));
        }
        return Bad(t, at, Cat("arguments don't match ", signature, ")"));
    }
    // Its stack starts above what is left of the caller's once the arguments moved into its locals. An input struct is
    // the same everywhere, so it passes nothing.
    const auto argCount = static_cast<std::uint32_t>(
        std::ranges::count_if(args, [](const Value& arg) { return !IsInputStruct(arg.type); }));
    t.maxStack = std::max(t.maxStack, t.depth - argCount + function.maxStack);
    t.calleeFrame = std::max(t.calleeFrame, function.frame);
    t.calleeDepth = std::max(t.calleeDepth, function.callDepth);
    t.inputs |= function.inputs;
    t.stages = static_cast<std::uint8_t>(t.stages & function.stages);
    Emit(t, MdslOpCode::Call, 1 - static_cast<int>(argCount),
         static_cast<std::uint32_t>(&function - t.functions.data()));
    std::string code = Cat("f_", function.name, "(");
    bool first = true;
    for (const Value& arg : args) {
        if (!IsInputStruct(arg.type)) {
            code += Cat(first ? "" : ", ", arg.code);
            first = false;
        }
    }
    return Value{.code = code + ")", .type = function.ret};
}

// A curve takes any number type, component-wise; a texture vec2 coordinates and an optional float LOD bias, on the GPU
// only.
Value CallSampler(Translator& t, const Token& at, const Symbol& symbol, const std::vector<Value>& args) {
    const std::string index = std::to_string(symbol.index);
    if (symbol.kind == SymbolKind::Curve) {
        if (args.size() != 1 || !IsNumeric(args[0].type)) {
            return Bad(t, at, Cat(symbol.name, "() takes a float, vec2, vec3 or vec4"));
        }
        Emit(t, MdslOpCode::Curve, 0, symbol.index, MdslComponents(args[0].type));
        return Value{.code = Cat("s_curve(", index, ", ", args[0].code, ")"), .type = args[0].type};
    }
    if ((args.size() != 1 && args.size() != 2) || args[0].type != MdslType::Vec2
        || (args.size() == 2 && args[1].type != MdslType::Float)) {
        return Bad(t, at, Cat(symbol.name, "() takes vec2 coordinates and an optional float LOD bias"));
    }
    t.depth -= static_cast<std::uint32_t>(args.size()) - 1;
    t.stages = static_cast<std::uint8_t>(t.stages & kStagePaint);
    const std::string bias = args.size() == 2 ? args[1].code : "0.0";
    return Value{.code = Cat("BrushTexture(", index, ", ", args[0].code, ", m_radius, ", bias, ")"),
                 .type = MdslType::Vec4};
}

Value Call(Translator& t, const Token& callee) {
    Bump(t); // '('
    std::vector<Value> args;
    if (t.token.kind != Tok::RParen) {
        do {
            args.push_back(Expression(t));
        } while (Accept(t, Tok::Comma));
    }
    Expect(t, Tok::RParen, "')'");
    if (t.failed) {
        return Value{};
    }
    if (const MdslType type = TypeFromName(callee.text); IsNumeric(type)) {
        return Construct(t, callee, type, args);
    }
    if (const MdslType type = TypeFromName(callee.text); IsOutputStruct(type)) {
        return Bad(t, callee,
                   Cat("a ", TypeName(type), " is declared, then its fields set: ", TypeName(type), " output; output.",
                       OutputFields(type).front().name, " = ...;"));
    }
    if (const Symbol* symbol = Find(t, callee.text);
        symbol != nullptr && (symbol->kind == SymbolKind::Curve || symbol->kind == SymbolKind::Texture)) {
        return CallSampler(t, callee, *symbol, args);
    }
    if (const Intrinsic* intrinsic = FindIntrinsic(callee.text)) {
        return CallIntrinsic(t, callee, *intrinsic, args);
    }
    if (const Function* function = FindFunction(t, callee.text)) {
        return CallFunction(t, callee, *function, args);
    }
    return Bad(t, callee, Cat("unknown function '", callee.text, "'"));
}

void PushSymbol(Translator& t, const Symbol& symbol) {
    switch (symbol.kind) {
        case SymbolKind::Param:
            Emit(t, MdslOpCode::Param, 1, symbol.index, MdslComponents(symbol.type));
            break;
        case SymbolKind::Local:
            Emit(t, MdslOpCode::Load, 1, symbol.index);
            break;
        case SymbolKind::InputStruct:
        case SymbolKind::OutputStruct:
        case SymbolKind::Curve:
        case SymbolKind::Texture:
            break;
    }
}

// `input.local` reads the stage's input, `output.amount` the field. A whole input only passes to a function that takes
// one; a whole output is returned.
Value StructValue(Translator& t, const Symbol& symbol) {
    if (!Accept(t, Tok::Dot)) {
        if (IsOutputStruct(symbol.type)) {
            const std::span<const Field> fields = OutputFields(symbol.type);
            std::vector<Value> parts;
            for (std::uint32_t i = 0; i < fields.size(); ++i) {
                Emit(t, MdslOpCode::Load, 1, symbol.index + i);
                parts.push_back(Value{.code = {}, .type = fields[i].type});
            }
            Emit(t, MdslOpCode::Construct, 1 - static_cast<int>(fields.size()), 0,
                 static_cast<std::uint32_t>(fields.size()), SizePattern(parts));
        }
        return Value{.code = symbol.code, .type = symbol.type};
    }
    const Token field = t.token;
    Expect(t, Tok::Ident, "a field");
    if (t.failed) {
        return Value{};
    }
    if (IsInputStruct(symbol.type)) {
        for (std::uint32_t i = 0; i < std::size(kInputs); ++i) {
            if ((kInputs[i].stages & InputStage(symbol.type)) != 0 && kInputs[i].name == field.text) {
                t.inputs |= 1U << i;
                Emit(t, MdslOpCode::Input, 1, i);
                return Value{.code = Cat("m_", kInputs[i].name), .type = kInputs[i].type};
            }
        }
    } else {
        const std::span<const Field> fields = OutputFields(symbol.type);
        for (std::uint32_t i = 0; i < fields.size(); ++i) {
            if (fields[i].name == field.text) {
                Emit(t, MdslOpCode::Load, 1, symbol.index + i);
                return Value{.code = Cat(symbol.code, ".", fields[i].name), .type = fields[i].type};
            }
        }
    }
    return Bad(t, field, Cat(TypeName(symbol.type), " has no field '", field.text, "'"));
}

Value Primary(Translator& t) {
    const Token token = t.token;
    switch (token.kind) {
        case Tok::Number:
            Bump(t);
            EmitConstant(t, ParseNumber(token.text));
            return Value{.code = Literal(token.text), .type = MdslType::Float};
        case Tok::LParen: {
            Bump(t);
            Value value = Expression(t);
            Expect(t, Tok::RParen, "')'");
            return value;
        }
        case Tok::Ident:
            break;
        default:
            return Bad(t, token,
                       token.kind == Tok::End ? std::string("unexpected end of input")
                                              : Cat("unexpected '", token.text, "'"));
    }
    Bump(t);
    if (token.text == "true" || token.text == "false") {
        EmitConstant(t, token.text == "true" ? 1.0f : 0.0f);
        return Value{.code = std::string(token.text), .type = MdslType::Bool};
    }
    if (t.token.kind == Tok::LParen) {
        return Call(t, token);
    }
    if (const Symbol* symbol = Find(t, token.text)) {
        if (symbol->kind == SymbolKind::Curve || symbol->kind == SymbolKind::Texture) {
            return Bad(t, token, Cat("'", token.text, "' is called like a function: ", token.text, "(...)"));
        }
        if (symbol->kind == SymbolKind::InputStruct || symbol->kind == SymbolKind::OutputStruct) {
            return StructValue(t, *symbol);
        }
        PushSymbol(t, *symbol);
        return Value{.code = symbol->code, .type = symbol->type};
    }
    return Bad(t, token, Cat("unknown name '", token.text, "'"));
}

Value Unary(Translator& t) {
    const Token op = t.token;
    if (Accept(t, Tok::Minus)) {
        const Value value = Unary(t);
        if (t.failed) {
            return Value{};
        }
        if (!IsNumeric(value.type)) {
            return Bad(t, op, Cat("cannot negate ", TypeName(value.type)));
        }
        Emit(t, MdslOpCode::Neg, 0);
        return Value{.code = Cat("(-", value.code, ")"), .type = value.type};
    }
    if (Accept(t, Tok::Bang)) {
        const Value value = Unary(t);
        if (t.failed) {
            return Value{};
        }
        if (value.type != MdslType::Bool) {
            return Bad(t, op, Cat("'!' needs a bool, got ", TypeName(value.type)));
        }
        Emit(t, MdslOpCode::Not, 0);
        return Value{.code = Cat("(!", value.code, ")"), .type = MdslType::Bool};
    }
    Value value = Primary(t);
    while (Accept(t, Tok::Dot)) {
        std::uint32_t pattern = 0;
        value = Swizzle(t, value, false, pattern);
    }
    return value;
}

Value Expression(Translator& t, int minPrecedence) {
    Value lhs = Unary(t);
    for (;;) {
        const Token op = t.token;
        const int precedence = Precedence(op.kind);
        if (precedence < minPrecedence) {
            return lhs;
        }
        Bump(t);
        const Value rhs = Expression(t, precedence + 1);
        lhs = Binary(t, op, lhs, rhs);
    }
}

bool IfStatement(Translator& t, std::string& out, MdslType ret, const Token& at) {
    Expect(t, Tok::LParen, "'('");
    const Value condition = Expression(t);
    Expect(t, Tok::RParen, "')'");
    if (!t.failed && condition.type != MdslType::Bool) {
        Fail(t, at, Cat("'if' needs a bool, got ", TypeName(condition.type)));
    }
    Line(t, out, Cat("if (", condition.code, ")"));
    const std::uint32_t skipThen = EmitJump(t, MdslOpCode::JumpIfFalse, -1);
    const bool thenReturns = Block(t, out, ret);
    if (!AcceptWord(t, "else")) {
        PatchJump(t, skipThen);
        return false;
    }
    const std::uint32_t skipElse = EmitJump(t, MdslOpCode::Jump, 0);
    PatchJump(t, skipThen);
    Line(t, out, "else");
    const Token next = t.token;
    const bool elseReturns = AcceptWord(t, "if") ? IfStatement(t, out, ret, next) : Block(t, out, ret);
    PatchJump(t, skipElse);
    return thenReturns && elseReturns;
}

// `for i in 0..8 { }`: constant bounds only, so every loop ends and a user's brush cannot hang the GPU. The loop
// variable is a copy of a hidden counter, as in the HLSL, so writing to it does not change the iterations.
void ForStatement(Translator& t, std::string& out, MdslType ret) {
    const Token var = t.token;
    Expect(t, Tok::Ident, "a loop variable");
    if (!t.failed && !AcceptWord(t, "in")) {
        Fail(t, t.token, "expected 'in'");
        return;
    }
    const int from = Integer(t);
    Expect(t, Tok::DotDot, "'..'");
    const Token end = t.token;
    const int to = Integer(t);
    if (t.failed) {
        return;
    }
    if (to <= from || to - from > kLoopIterationsMax) {
        Fail(t, end, Cat("a loop runs 1 to ", std::to_string(kLoopIterationsMax), " times"));
        return;
    }
    const std::string counter = Cat("n_", var.text);
    Line(t, out,
         Cat("for (int ", counter, " = ", std::to_string(from), "; ", counter, " < ", std::to_string(to), "; ++",
             counter, ")"));
    const std::uint32_t counterSlot = t.slots++;
    EmitConstant(t, static_cast<float>(from));
    Emit(t, MdslOpCode::Store, -1, counterSlot);
    const std::uint32_t top = Here(t);
    Emit(t, MdslOpCode::Load, 1, counterSlot);
    EmitConstant(t, static_cast<float>(to));
    Emit(t, MdslOpCode::Less, -1);
    const std::uint32_t exit = EmitJump(t, MdslOpCode::JumpIfFalse, -1);

    const std::size_t scope = t.locals.size();
    const std::uint32_t varSlot = DeclareLocal(t, var, MdslType::Float);
    Emit(t, MdslOpCode::Load, 1, counterSlot);
    Emit(t, MdslOpCode::Store, -1, varSlot);
    Block(t, out, ret, Cat("float u_", var.text, " = float(", counter, ");"));
    t.locals.resize(scope);

    Emit(t, MdslOpCode::Load, 1, counterSlot);
    EmitConstant(t, 1.0f);
    Emit(t, MdslOpCode::Add, -1);
    Emit(t, MdslOpCode::Store, -1, counterSlot);
    Emit(t, MdslOpCode::Jump, 0, top);
    PatchJump(t, exit);
}

// `PaintOutput output;`, its fields at 0, in the entry that returns it.
void OutputDeclaration(Translator& t, std::string& out, const Token& typeToken, MdslType type) {
    const Token var = t.token;
    Expect(t, Tok::Ident, "a variable name");
    Expect(t, Tok::Semi, "';' (its fields start at 0)");
    if (t.failed) {
        return;
    }
    if (type != t.ret) {
        Fail(t, typeToken, Cat("a ", TypeName(type), " can only be declared in the function that returns one"));
        return;
    }
    Claim(t, var);
    const std::span<const Field> fields = OutputFields(type);
    const std::uint32_t slot = t.slots;
    t.slots += static_cast<std::uint32_t>(fields.size());
    if (t.failed) {
        return;
    }
    t.locals.push_back(Symbol{
        .name = var.text, .code = Cat("u_", var.text), .index = slot, .type = type, .kind = SymbolKind::OutputStruct});
    for (std::uint32_t i = 0; i < fields.size(); ++i) {
        EmitConstant(t, 0.0f);
        Emit(t, MdslOpCode::Store, -1, slot + i);
    }
    Line(t, out, Cat(HlslType(type), " u_", var.text, " = (", HlslType(type), ")0;"));
}

void Declaration(Translator& t, std::string& out) {
    const Token typeToken = t.token;
    const MdslType type = TypeFromName(typeToken.text);
    Bump(t);
    if (IsOutputStruct(type)) {
        OutputDeclaration(t, out, typeToken, type);
        return;
    }
    if (!IsNumeric(type) && type != MdslType::Bool) {
        Fail(t, typeToken, Cat("cannot declare a local ", TypeName(type)));
        return;
    }
    const Token var = t.token;
    Expect(t, Tok::Ident, "a variable name");
    Expect(t, Tok::Assign, "'=' (variables must be initialized)");
    const Value value = Expression(t);
    Expect(t, Tok::Semi, "';'");
    if (t.failed) {
        return;
    }
    if (value.type != type) {
        Fail(t, var, Cat("'", var.text, "' is ", TypeName(type), " but gets ", TypeName(value.type)));
        return;
    }
    const std::uint32_t slot = DeclareLocal(t, var, type);
    Emit(t, MdslOpCode::Store, -1, slot);
    Line(t, out, Cat(HlslType(type), " u_", var.text, " = ", value.code, ";"));
}

void Assignment(Translator& t, std::string& out) {
    const Token var = t.token;
    Expect(t, Tok::Ident, "a statement");
    if (t.failed) {
        return;
    }
    const Symbol* symbol = Find(t, var.text);
    if (symbol == nullptr) {
        Fail(t, var, Cat("unknown name '", var.text, "'"));
        return;
    }
    std::uint32_t slot = symbol->index;
    Value target{.code = symbol->code, .type = symbol->type};
    if (symbol->kind == SymbolKind::OutputStruct) {
        const MdslType type = symbol->type;
        Expect(t, Tok::Dot, Cat("'.' and a field: a ", TypeName(type), " is set field by field"));
        const Token field = t.token;
        Expect(t, Tok::Ident, "a field");
        if (t.failed) {
            return;
        }
        const std::span<const Field> fields = OutputFields(type);
        const auto found = std::ranges::find(fields, field.text, &Field::name);
        if (found == fields.end()) {
            Fail(t, field, Cat(TypeName(type), " has no field '", field.text, "'"));
            return;
        }
        slot += static_cast<std::uint32_t>(found - fields.begin());
        target = Value{.code = Cat(target.code, ".", found->name), .type = found->type};
    } else if (symbol->kind != SymbolKind::Local) {
        Fail(t, var, Cat("'", var.text, "' is read-only"));
        return;
    }
    std::uint32_t pattern = 0;
    const bool swizzled = Accept(t, Tok::Dot);
    if (swizzled) {
        target = Swizzle(t, target, true, pattern);
    }
    const Token op = t.token;
    if (op.kind != Tok::Assign && op.kind != Tok::PlusEq && op.kind != Tok::MinusEq && op.kind != Tok::StarEq
        && op.kind != Tok::SlashEq) {
        Fail(t, op, "expected '=', '+=', '-=', '*=' or '/='");
        return;
    }
    Bump(t);
    const std::uint32_t count = MdslComponents(target.type);
    if (op.kind != Tok::Assign) {
        Emit(t, MdslOpCode::Load, 1, slot);
        if (swizzled) {
            Emit(t, MdslOpCode::Swizzle, 0, 0, count, pattern);
        }
    }
    const Value value = Expression(t);
    Expect(t, Tok::Semi, "';'");
    if (t.failed) {
        return;
    }
    const bool assignable =
        op.kind == Tok::Assign ? value.type == target.type : Arithmetic(target.type, value.type) == target.type;
    if (!assignable) {
        Fail(t, op, Cat("cannot assign ", TypeName(value.type), " to ", TypeName(target.type)));
        return;
    }
    if (op.kind != Tok::Assign) {
        Emit(t, BinaryOp(op.kind), -1);
    }
    if (swizzled) {
        Emit(t, MdslOpCode::StoreMask, -1, slot, count, pattern);
    } else {
        Emit(t, MdslOpCode::Store, -1, slot);
    }
    Line(t, out, Cat(target.code, " ", op.text, " ", value.code, ";"));
}

// True when it returns on every path.
bool Statement(Translator& t, std::string& out, MdslType ret) {
    const Token token = t.token;
    if (token.kind == Tok::LBrace) {
        return Block(t, out, ret);
    }
    if (AcceptWord(t, "return")) {
        const Value value = Expression(t);
        Expect(t, Tok::Semi, "';'");
        if (!t.failed && value.type != ret) {
            Fail(t, token, Cat("returning ", TypeName(value.type), " from a function that returns ", TypeName(ret)));
        }
        Emit(t, MdslOpCode::Return, -1);
        Line(t, out, Cat("return ", value.code, ";"));
        return true;
    }
    if (AcceptWord(t, "if")) {
        return IfStatement(t, out, ret, token);
    }
    if (AcceptWord(t, "for")) {
        ForStatement(t, out, ret);
        return false;
    }
    if (token.kind == Tok::Ident && TypeFromName(token.text) != MdslType::Error) {
        Declaration(t, out);
        return false;
    }
    Assignment(t, out);
    return false;
}

bool Block(Translator& t, std::string& out, MdslType ret, std::string_view prologue) {
    Expect(t, Tok::LBrace, "'{'");
    Line(t, out, "{");
    ++t.indent;
    if (!prologue.empty()) {
        Line(t, out, prologue);
    }
    const std::size_t scope = t.locals.size();
    bool returns = false;
    while (t.token.kind != Tok::RBrace && t.token.kind != Tok::End) {
        if (Statement(t, out, ret)) {
            returns = true;
        }
    }
    Expect(t, Tok::RBrace, "'}'");
    t.locals.resize(scope);
    --t.indent;
    Line(t, out, "}");
    return returns;
}

// stamp() and paint() only use what is known where they run; stamp() also fits the interpreter.
void CheckEntry(Translator& t, const Token& name, const Function& function) {
    const std::uint8_t stage = function.ret == MdslType::StampOutput ? kStageStamp : kStagePaint;
    for (std::uint32_t i = 0; i < std::size(kInputs); ++i) {
        if ((function.inputs & (1U << i)) != 0 && (kInputs[i].stages & stage) == 0) {
            Fail(t, name, Cat(name.text, "() cannot read '", kInputs[i].name, "', or call a function that does"));
            return;
        }
    }
    if ((function.stages & stage) == 0) {
        Fail(t, name, Cat(name.text, "() cannot read textures, which only paint() can"));
        return;
    }
    if (function.ret == MdslType::StampOutput
        && (function.maxStack > kStackMax || function.frame > kLocalsMax || function.callDepth > kCallDepthMax)) {
        Fail(t, name, "stamp() and what it calls need more variables or nested calls than the CPU side holds");
    }
}

void DefineFunction(Translator& t, MdslType ret) {
    const Token name = t.token;
    Expect(t, Tok::Ident, "a function name");
    if (t.failed) {
        return;
    }
    const Entry* entry = FindEntry(name.text);
    if (entry != nullptr ? ret != entry->output : !IsNumeric(ret)) {
        Fail(t, name,
             "stamp() returns StampOutput, paint() PaintOutput, and other functions float, vec2, vec3 or vec4");
        return;
    }
    if (IsReserved(name.text) || FindIntrinsic(name.text) != nullptr) {
        Fail(t, name, Cat("'", name.text, "' is a built-in"));
        return;
    }
    if (const Symbol* symbol = Find(t, name.text);
        symbol != nullptr && (symbol->kind == SymbolKind::Curve || symbol->kind == SymbolKind::Texture)) {
        Fail(t, name, Cat("'", name.text, "' is already a param"));
        return;
    }
    if (const Function* defined = FindFunction(t, name.text)) {
        Fail(t, name, Cat("'", name.text, "' is already defined", defined->prelude ? " by the prelude" : ""));
        return;
    }
    t.indent = 0;
    t.depth = 0;
    t.maxStack = 0;
    t.slots = 0;
    t.calleeFrame = 0;
    t.calleeDepth = 0;
    t.inputs = 0;
    t.stages = kStageAll;
    t.ret = ret;
    t.emitting = ret != MdslType::PaintOutput;
    const std::uint32_t start = Here(t);
    Function function{.name = name.text,
                      .params = {},
                      .maxStack = 0,
                      .frame = 0,
                      .callDepth = 0,
                      .inputs = 0,
                      .stages = kStageAll,
                      .ret = ret,
                      .prelude = !t.user};
    std::string head = Cat(HlslType(ret), " f_", name.text, "(");
    std::uint32_t slotParams = 0; // the interpreter's: an input struct takes none
    Expect(t, Tok::LParen, "'('");
    if (t.token.kind != Tok::RParen) {
        do {
            const Token typeToken = t.token;
            const MdslType type = TypeFromName(typeToken.text);
            if (!IsNumeric(type) && !IsInputStruct(type)) {
                Fail(t, typeToken, "expected a parameter type");
                return;
            }
            Bump(t);
            const Token param = t.token;
            Expect(t, Tok::Ident, "a parameter name");
            if (IsInputStruct(type)) {
                Claim(t, param);
                if (!t.failed) {
                    t.locals.push_back(Symbol{
                        .name = param.text, .code = {}, .index = 0, .type = type, .kind = SymbolKind::InputStruct});
                }
            } else {
                DeclareLocal(t, param, type);
                head += Cat(slotParams == 0 ? "" : ", ", HlslType(type), " u_", param.text);
                ++slotParams;
            }
            function.params.push_back(type);
        } while (Accept(t, Tok::Comma));
    }
    Expect(t, Tok::RParen, "')'");
    head += ")";
    if (!t.failed && entry != nullptr && (function.params.size() != 1 || function.params[0] != entry->input)) {
        Fail(t, name, Cat(name.text, "() takes (", TypeName(entry->input), " input)"));
        return;
    }

    std::string body;
    const bool returns = Block(t, body, ret);
    t.locals.clear();
    if (t.failed) {
        return;
    }
    if (!returns) {
        Fail(t, name, Cat("'", name.text, "' must return a value on every path"));
        return;
    }
    function.maxStack = t.maxStack;
    function.frame = t.slots + t.calleeFrame;
    function.callDepth = 1 + t.calleeDepth;
    function.inputs = t.inputs;
    function.stages = t.stages;
    if (entry != nullptr) {
        CheckEntry(t, name, function);
    }
    // stamp() runs on the CPU only.
    if (ret != MdslType::StampOutput) {
        Line(t, t.code, head);
        t.code += body;
        t.code += '\n';
    }
    t.program.functions.push_back(MdslFunction{.start = start, .params = slotParams, .slots = t.slots});
    t.functions.push_back(std::move(function));
}

void Unit(Translator& t, std::string_view source, bool user) {
    if (t.failed) {
        return;
    }
    t.user = user;
    t.lexer = Lexer{.source = source};
    Bump(t);
    while (t.token.kind != Tok::End) {
        if (AcceptWord(t, "param")) {
            Param(t);
            continue;
        }
        const Token token = t.token;
        const MdslType ret = TypeFromName(token.text);
        if (token.kind != Tok::Ident || ret == MdslType::Error) {
            Fail(t, token, "expected 'param' or a function");
            return;
        }
        Bump(t);
        DefineFunction(t, ret);
    }
}

// The param statics from `params`.
std::string ParamsHlsl(const Translator& t) {
    std::string s;
    for (const MdslParam& param : t.params) {
        const std::uint32_t components = MdslComponents(param.type);
        std::string values;
        for (std::uint32_t i = 0; i < components; ++i) {
            values += Cat(i != 0 ? ", " : "", "params[", std::to_string(param.offset + i), "]");
        }
        s += components == 1 ? Cat("    u_", param.name, " = ", values, ";\n")
                             : Cat("    u_", param.name, " = ", HlslType(param.type), "(", values, ");\n");
    }
    return s;
}

// The entry `name` returning `type`, setting the inputs of `stage` from its parameters, then the params.
std::string EntryHlsl(const Translator& t, std::string_view type, std::string_view name, std::uint8_t stage,
                      std::string_view entry) {
    std::string s = Cat(type, " ", name, "(");
    for (const Input& input : kInputs) {
        if ((input.stages & stage) != 0) {
            s += Cat(HlslType(input.type), " i_", input.name, ", ");
        }
    }
    s += Cat("float params[", std::to_string(kMdslParamFloatsMax), "]) {\n");
    for (const Input& input : kInputs) {
        if ((input.stages & stage) != 0) {
            s += Cat("    m_", input.name, " = i_", input.name, ";\n");
        }
    }
    return Cat(s, ParamsHlsl(t), "    return ", entry, ";\n}\n");
}

// Paint and the inputs are plain statics: a compute thread runs one texel of one stamp at a time.
std::string EmitHlsl(const Translator& t) {
    std::string s = "struct Paint {\n"
                    "    float3 color;\n"
                    "    float amount;\n"
                    "    float opacity;\n"
                    "};\n\n"
                    "Paint PaintMake(float3 p_color, float p_amount, float p_opacity) {\n"
                    "    Paint paint;\n"
                    "    paint.color = p_color;\n"
                    "    paint.amount = p_amount;\n"
                    "    paint.opacity = p_opacity;\n"
                    "    return paint;\n"
                    "}\n\n";
    for (const Input& input : kInputs) {
        s += Cat("static ", HlslType(input.type), " m_", input.name, ";\n");
    }
    for (const MdslParam& param : t.params) {
        s += Cat("static ", HlslType(param.type), " u_", param.name, ";\n");
    }
    // BrushCurve and BrushTexture are the template's.
    s += "float s_curve(uint c, float x) {\n    return BrushCurve(c, x);\n}\n";
    for (const std::string_view n : {"2", "3", "4"}) {
        s += Cat("float", n, " s_curve(uint c, float", n, " x) {\n    float", n, " y = 0.0;\n    for (uint i = 0; i < ",
                 n, "; ++i) {\n        y[i] = BrushCurve(c, x[i]);\n    }\n    return y;\n}\n");
    }
    return Cat(s, "\n", t.code, EntryHlsl(t, "Paint", "BrushPaint", kStagePaint, "f_paint()"));
}

Lane Splatted(float value) noexcept {
    return Lane{value, value, value, value};
}

template<class Fn>
Lane EachLane(const Lane& a, Fn fn) noexcept {
    return Lane{fn(a[0]), fn(a[1]), fn(a[2]), fn(a[3])};
}

template<class Fn>
Lane EachLane(const Lane& a, const Lane& b, Fn fn) noexcept {
    return Lane{fn(a[0], b[0]), fn(a[1], b[1]), fn(a[2], b[2]), fn(a[3], b[3])};
}

template<class Fn>
Lane EachLane(const Lane& a, const Lane& b, const Lane& c, Fn fn) noexcept {
    return Lane{fn(a[0], b[0], c[0]), fn(a[1], b[1], c[1]), fn(a[2], b[2], c[2]), fn(a[3], b[3], c[3])};
}

float Dot(const Lane& a, const Lane& b, std::uint32_t count) noexcept {
    float sum = 0.0f;
    for (std::uint32_t i = 0; i < count; ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

} // namespace

std::expected<MdslBrush, MdslError> MdslTranslate(std::string_view source) noexcept {
    Translator t;
    Unit(t, kPrelude, false);
    Unit(t, source, true);
    const Token start{.text = {}, .line = 1, .column = 1, .kind = Tok::End};
    const Function* stamp = FindFunction(t, "stamp");
    const Function* paint = FindFunction(t, "paint");
    if (!t.failed && stamp == nullptr) {
        Fail(t, start, "missing StampOutput stamp(StampInput input)");
    }
    if (!t.failed && paint == nullptr) {
        Fail(t, start, "missing PaintOutput paint(PaintInput input)");
    }
    if (t.failed) {
        return std::unexpected(std::move(t.error));
    }
    t.program.entry = static_cast<std::uint32_t>(stamp - t.functions.data());
    return MdslBrush{.hlsl = EmitHlsl(t),
                     .params = std::move(t.params),
                     .curves = std::move(t.curves),
                     .textures = std::move(t.textures),
                     .stamp = std::move(t.program)};
}

// The translator checked the program's stack, locals and calls against the fixed storage here, and every loop is
// bounded, so it runs to its Return.
MdslStamp MdslStampRun(const MdslProgram& program, const MdslStampInput& input) noexcept {
    struct Frame {
        std::uint32_t pc;
        std::uint32_t base;
        std::uint32_t slots;
        std::uint32_t stackBase;
    };
    std::array<Lane, kStackMax> stack;
    std::array<Lane, kLocalsMax> locals{};
    std::array<Frame, kCallDepthMax> frames;
    std::array<Lane, std::size(kInputs)> inputs{};
    const MdslDab& dab = input.dab;
    inputs[kInputPressure] = Splatted(dab.pressure);
    inputs[kInputTilt] = Lane{dab.tiltX, dab.tiltY, 0.0f, 0.0f};
    inputs[kInputVelocity] = Lane{dab.velocityX, dab.velocityY, 0.0f, 0.0f};
    inputs[kInputDirection] = Lane{dab.directionX, dab.directionY, 0.0f, 0.0f};
    inputs[kInputRandom] = Splatted(dab.random);
    inputs[kInputAlong] = Splatted(dab.along);
    inputs[kInputTime] = Splatted(dab.time);

    const MdslFunction& entry = program.functions[program.entry];
    std::uint32_t pc = entry.start;
    std::uint32_t base = 0;
    std::uint32_t slots = entry.slots;
    std::uint32_t sp = 0;
    std::uint32_t fp = 0;
    const auto pop = [&]() noexcept -> Lane {
        return stack[--sp];
    };
    const auto push = [&](const Lane& value) noexcept {
        stack[sp++] = value;
    };
    for (;;) {
        const MdslOp op = program.ops[pc++];
        switch (op.code) {
            case MdslOpCode::Const:
                push(program.constants[op.arg]);
                break;
            case MdslOpCode::Input:
                push(inputs[op.arg]);
                break;
            case MdslOpCode::Param: {
                Lane value = Splatted(input.params[op.arg]);
                for (std::uint32_t i = 1; i < op.count; ++i) {
                    value[i] = input.params[op.arg + i];
                }
                if (op.count > 1) {
                    std::fill(value.begin() + op.count, value.end(), 0.0f);
                }
                push(value);
                break;
            }
            case MdslOpCode::Load:
                push(locals[base + op.arg]);
                break;
            case MdslOpCode::Store:
                locals[base + op.arg] = pop();
                break;
            case MdslOpCode::StoreMask: {
                const Lane value = pop();
                for (std::uint32_t i = 0; i < op.count; ++i) {
                    locals[base + op.arg][(op.pattern >> (2 * i)) & 3] = value[i];
                }
                break;
            }
            case MdslOpCode::Swizzle: {
                const Lane value = pop();
                Lane picked{};
                for (std::uint32_t i = 0; i < op.count; ++i) {
                    picked[i] = value[(op.pattern >> (2 * i)) & 3];
                }
                push(op.count == 1 ? Splatted(picked[0]) : picked);
                break;
            }
            case MdslOpCode::Construct: {
                std::array<Lane, 4> args;
                for (std::uint32_t i = op.count; i-- > 0;) {
                    args[i] = pop();
                }
                Lane value{};
                std::uint32_t at = 0;
                for (std::uint32_t i = 0; i < op.count; ++i) {
                    const std::uint32_t size = ((op.pattern >> (2 * i)) & 3) + 1;
                    for (std::uint32_t c = 0; c < size && at < 4; ++c) {
                        value[at++] = args[i][c];
                    }
                }
                push(value);
                break;
            }
            case MdslOpCode::Call: {
                const MdslFunction& function = program.functions[op.arg];
                const std::uint32_t calleeBase = base + slots;
                for (std::uint32_t i = function.params; i-- > 0;) {
                    locals[calleeBase + i] = pop();
                }
                frames[fp++] = Frame{.pc = pc, .base = base, .slots = slots, .stackBase = sp};
                pc = function.start;
                base = calleeBase;
                slots = function.slots;
                break;
            }
            case MdslOpCode::Return: {
                const Lane value = pop();
                if (fp == 0) {
                    return MdslStamp{.radius = value[0], .spacing = value[1], .offsetX = value[2], .offsetY = value[3]};
                }
                const Frame frame = frames[--fp];
                pc = frame.pc;
                base = frame.base;
                slots = frame.slots;
                sp = frame.stackBase;
                push(value);
                break;
            }
            case MdslOpCode::Jump:
                pc = op.arg;
                break;
            case MdslOpCode::JumpIfFalse:
                if (pop()[0] == 0.0f) {
                    pc = op.arg;
                }
                break;
            case MdslOpCode::Neg:
                push(EachLane(pop(), [](float a) { return -a; }));
                break;
            case MdslOpCode::Not:
                push(Splatted(pop()[0] == 0.0f ? 1.0f : 0.0f));
                break;
            case MdslOpCode::Min:
            case MdslOpCode::Max:
            case MdslOpCode::Pow:
            case MdslOpCode::Step:
            case MdslOpCode::Atan2:
            case MdslOpCode::Add:
            case MdslOpCode::Sub:
            case MdslOpCode::Mul:
            case MdslOpCode::Div: {
                const Lane b = pop();
                const Lane a = pop();
                switch (op.code) {
                    case MdslOpCode::Add:
                        push(EachLane(a, b, [](float x, float y) { return x + y; }));
                        break;
                    case MdslOpCode::Sub:
                        push(EachLane(a, b, [](float x, float y) { return x - y; }));
                        break;
                    case MdslOpCode::Mul:
                        push(EachLane(a, b, [](float x, float y) { return x * y; }));
                        break;
                    case MdslOpCode::Div:
                        push(EachLane(a, b, [](float x, float y) { return x / y; }));
                        break;
                    case MdslOpCode::Min:
                        push(EachLane(a, b, [](float x, float y) { return std::min(x, y); }));
                        break;
                    case MdslOpCode::Max:
                        push(EachLane(a, b, [](float x, float y) { return std::max(x, y); }));
                        break;
                    case MdslOpCode::Pow:
                        push(EachLane(a, b, [](float x, float y) { return std::pow(x, y); }));
                        break;
                    case MdslOpCode::Step:
                        push(EachLane(a, b, [](float edge, float x) { return x >= edge ? 1.0f : 0.0f; }));
                        break;
                    default:
                        push(EachLane(a, b, [](float y, float x) { return std::atan2(y, x); }));
                        break;
                }
                break;
            }
            case MdslOpCode::Less:
            case MdslOpCode::Greater:
            case MdslOpCode::LessEq:
            case MdslOpCode::GreaterEq:
            case MdslOpCode::Equal:
            case MdslOpCode::NotEqual:
            case MdslOpCode::And:
            case MdslOpCode::Or: {
                const float b = pop()[0];
                const float a = pop()[0];
                bool result = false;
                switch (op.code) {
                    case MdslOpCode::Less:
                        result = a < b;
                        break;
                    case MdslOpCode::Greater:
                        result = a > b;
                        break;
                    case MdslOpCode::LessEq:
                        result = a <= b;
                        break;
                    case MdslOpCode::GreaterEq:
                        result = a >= b;
                        break;
                    case MdslOpCode::Equal:
                        result = a == b;
                        break;
                    case MdslOpCode::NotEqual:
                        result = a != b;
                        break;
                    case MdslOpCode::And:
                        result = a != 0.0f && b != 0.0f;
                        break;
                    default:
                        result = a != 0.0f || b != 0.0f;
                        break;
                }
                push(Splatted(result ? 1.0f : 0.0f));
                break;
            }
            case MdslOpCode::Abs:
                push(EachLane(pop(), [](float a) { return std::abs(a); }));
                break;
            case MdslOpCode::Floor:
                push(EachLane(pop(), [](float a) { return std::floor(a); }));
                break;
            case MdslOpCode::Ceil:
                push(EachLane(pop(), [](float a) { return std::ceil(a); }));
                break;
            case MdslOpCode::Fract:
                push(EachLane(pop(), [](float a) { return a - std::floor(a); }));
                break;
            case MdslOpCode::Sqrt:
                push(EachLane(pop(), [](float a) { return std::sqrt(a); }));
                break;
            case MdslOpCode::Exp:
                push(EachLane(pop(), [](float a) { return std::exp(a); }));
                break;
            case MdslOpCode::Log:
                push(EachLane(pop(), [](float a) { return std::log(a); }));
                break;
            case MdslOpCode::Sin:
                push(EachLane(pop(), [](float a) { return std::sin(a); }));
                break;
            case MdslOpCode::Cos:
                push(EachLane(pop(), [](float a) { return std::cos(a); }));
                break;
            case MdslOpCode::Normalize: {
                const Lane a = pop();
                const float length = std::sqrt(Dot(a, a, op.count));
                push(EachLane(a, [length](float x) { return x / length; }));
                break;
            }
            case MdslOpCode::Clamp:
            case MdslOpCode::Mix:
            case MdslOpCode::Smoothstep: {
                const Lane c = pop();
                const Lane b = pop();
                const Lane a = pop();
                if (op.code == MdslOpCode::Clamp) {
                    push(EachLane(a, b, c, [](float x, float lo, float hi) { return std::min(std::max(x, lo), hi); }));
                } else if (op.code == MdslOpCode::Mix) {
                    push(EachLane(a, b, c, [](float x, float y, float s) { return x + ((y - x) * s); }));
                } else {
                    push(EachLane(a, b, c, [](float e0, float e1, float x) {
                        const float s = std::min(std::max((x - e0) / (e1 - e0), 0.0f), 1.0f);
                        return s * s * (3.0f - (2.0f * s));
                    }));
                }
                break;
            }
            case MdslOpCode::Length: {
                const Lane a = pop();
                push(Splatted(std::sqrt(Dot(a, a, op.count))));
                break;
            }
            case MdslOpCode::Distance: {
                const Lane b = pop();
                const Lane a = pop();
                const Lane d = EachLane(a, b, [](float x, float y) { return x - y; });
                push(Splatted(std::sqrt(Dot(d, d, op.count))));
                break;
            }
            case MdslOpCode::Dot: {
                const Lane b = pop();
                const Lane a = pop();
                push(Splatted(Dot(a, b, op.count)));
                break;
            }
            case MdslOpCode::Curve: {
                Lane value = pop();
                const auto samples = input.curves.subspan(std::size_t{op.arg} * kCurveSamples).first<kCurveSamples>();
                for (std::uint32_t i = 0; i < op.count; ++i) {
                    value[i] = CurveSample(samples, value[i]);
                }
                push(op.count == 1 ? Splatted(value[0]) : value);
                break;
            }
        }
    }
}

} // namespace Midori
