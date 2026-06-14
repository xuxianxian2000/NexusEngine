#include "nexus/scripting/lua_backend.h"
#include "nexus/scripting/script_engine.h"
#include "nexus/core/log.h"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace nexus::scripting {

// ── Construction / Lifecycle ────────────────────────────────────────────────

LuaBackend::LuaBackend(ScriptEngine& engine)
    : engine_(engine) {}

LuaBackend::~LuaBackend() {
    shutdown();
}

bool LuaBackend::initialize() {
    if (initialized_) return true;
    initialized_ = true;
    NX_INFO("LuaBackend initialized (lightweight evaluator mode)");
    return true;
}

void LuaBackend::shutdown() {
    if (!initialized_) return;
    globals_.clear();
    initialized_ = false;
    NX_INFO("LuaBackend shut down");
}

// ── Public API ──────────────────────────────────────────────────────────────

bool LuaBackend::execute(const std::string& script) {
    if (!initialized_) {
        set_error("LuaBackend not initialized", "<string>", 0);
        return false;
    }

    last_error_.clear();

    std::istringstream stream(script);
    std::string line;
    u32 line_number = 0;

    // Track if/then/end block state.
    // We support single-level if blocks for the common scripting pattern.
    enum class BlockState : u8 { None, WaitingForThen, InTrueBody, InFalseBody };
    BlockState block_state = BlockState::None;
    bool condition_result = false;

    while (std::getline(stream, line)) {
        ++line_number;

        std::string trimmed = trim(line);

        // Skip empty lines.
        if (trimmed.empty()) continue;

        // Skip comments (-- style).
        if (trimmed.size() >= 2 && trimmed[0] == '-' && trimmed[1] == '-') {
            continue;
        }

        // Strip inline comments (-- not inside a string).
        {
            bool in_single_quote = false;
            bool in_double_quote = false;
            for (size_t i = 0; i < trimmed.size(); ++i) {
                char c = trimmed[i];
                if (c == '\'' && !in_double_quote) {
                    in_single_quote = !in_single_quote;
                } else if (c == '"' && !in_single_quote) {
                    in_double_quote = !in_double_quote;
                } else if (c == '-' && !in_single_quote && !in_double_quote &&
                           i + 1 < trimmed.size() && trimmed[i + 1] == '-') {
                    trimmed = trim(trimmed.substr(0, i));
                    break;
                }
            }
        }

        if (trimmed.empty()) continue;

        // Handle "end" keyword.
        if (trimmed == "end") {
            block_state = BlockState::None;
            continue;
        }

        // Handle "else" keyword.
        if (trimmed == "else") {
            if (block_state == BlockState::InTrueBody) {
                block_state = BlockState::InFalseBody;
            } else if (block_state == BlockState::InFalseBody) {
                block_state = BlockState::InTrueBody;
            }
            continue;
        }

        // Handle "while ... do" loop.
        if (trimmed.size() > 6 && trimmed.substr(0, 6) == "while ") {
            auto do_pos = trimmed.rfind(" do");
            if (do_pos != std::string::npos && do_pos + 3 >= trimmed.size() - 1) {
                std::string cond_expr = trim(trimmed.substr(6, do_pos - 6));
                auto body = collect_block(stream, line_number);
                if (!execute_while(cond_expr, body, line_number, "<string>")) {
                    return false;
                }
                continue;
            }
        }

        // Handle "for var = start, stop[, step] do" numeric for loop.
        if (trimmed.size() > 4 && trimmed.substr(0, 4) == "for ") {
            auto do_pos = trimmed.rfind(" do");
            if (do_pos != std::string::npos) {
                std::string for_clause = trim(trimmed.substr(4, do_pos - 4));
                auto eq_pos = for_clause.find('=');
                if (eq_pos != std::string::npos) {
                    std::string var = trim(for_clause.substr(0, eq_pos));
                    std::string range_str = trim(for_clause.substr(eq_pos + 1));
                    auto parts = tokenize_args(range_str);

                    if (parts.size() >= 2) {
                        ScriptValue sv_start = evaluate_expression(trim(parts[0]));
                        ScriptValue sv_stop = evaluate_expression(trim(parts[1]));
                        i32 start_val = sv_start.is_int() ? sv_start.as_int() : static_cast<i32>(sv_start.as_float());
                        i32 stop_val = sv_stop.is_int() ? sv_stop.as_int() : static_cast<i32>(sv_stop.as_float());
                        i32 step_val = 1;
                        if (parts.size() >= 3) {
                            ScriptValue sv_step = evaluate_expression(trim(parts[2]));
                            step_val = sv_step.is_int() ? sv_step.as_int() : static_cast<i32>(sv_step.as_float());
                        }

                        auto body = collect_block(stream, line_number);
                        if (!execute_for(var, start_val, stop_val, step_val,
                                        body, line_number, "<string>")) {
                            return false;
                        }
                        continue;
                    }
                }
            }
        }

        // Handle "if ... then" pattern.
        if (trimmed.substr(0, 3) == "if " && trimmed.size() > 4) {
            // Find "then" at the end.
            auto then_pos = trimmed.rfind("then");
            if (then_pos != std::string::npos &&
                then_pos + 4 >= trimmed.size() - 1) {
                std::string cond_expr =
                    trim(trimmed.substr(3, then_pos - 3));
                ScriptValue cond_val = evaluate_expression(cond_expr);
                condition_result = cond_val.truthy();
                block_state = condition_result ? BlockState::InTrueBody
                                               : BlockState::InFalseBody;
                continue;
            }
        }

        // If we are inside a false branch, skip lines.
        if (block_state == BlockState::InFalseBody) {
            continue;
        }

        // Execute the line.
        if (!execute_line(trimmed, line_number, "<string>")) {
            return false;
        }
    }

    return last_error_.empty();
}

bool LuaBackend::execute_file(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        set_error("Failed to open script file: " + filepath, filepath, 0);
        return false;
    }

    std::stringstream buf;
    buf << file.rdbuf();
    return execute(buf.str());
}

ScriptValue LuaBackend::call(const std::string& func_name,
                              const std::vector<ScriptValue>& args) {
    if (!initialized_) {
        set_error("LuaBackend not initialized", "<call>", 0);
        return ScriptValue::nil();
    }
    return engine_.call_function(func_name, args);
}

void LuaBackend::set_global(const std::string& name,
                             const ScriptValue& value) {
    globals_[name] = value;
}

ScriptValue LuaBackend::get_global(const std::string& name) const {
    auto it = globals_.find(name);
    return it != globals_.end() ? it->second : ScriptValue::nil();
}

// ── Line Execution ─────────────────────────────────────────────────────────

bool LuaBackend::execute_line(const std::string& line, u32 line_number,
                               const std::string& source_name) {
    std::string trimmed = trim(line);

    // Handle "local varname = expr" or "varname = expr".
    bool is_local = false;
    std::string work = trimmed;

    if (work.size() > 6 && work.substr(0, 6) == "local ") {
        is_local = true;
        work = trim(work.substr(6));
    }
    (void)is_local; // local vs global distinction not needed in flat scope

    // Check for assignment: "name = expr".
    // But not "==", and the name must be a valid identifier (possibly dotted).
    auto eq_pos = work.find('=');
    if (eq_pos != std::string::npos && eq_pos > 0 &&
        work[eq_pos - 1] != '!' && work[eq_pos - 1] != '<' &&
        work[eq_pos - 1] != '>' && work[eq_pos - 1] != '~') {
        // Make sure the next char is not '=' (would be "==").
        if (eq_pos + 1 < work.size() && work[eq_pos + 1] == '=') {
            // This is a comparison, not assignment. Fall through to evaluate.
        } else {
            std::string var_name = trim(work.substr(0, eq_pos));
            std::string rhs = trim(work.substr(eq_pos + 1));

            // Validate that var_name looks like an identifier.
            bool valid_ident = !var_name.empty();
            for (size_t i = 0; i < var_name.size() && valid_ident; ++i) {
                char c = var_name[i];
                if (i == 0) {
                    valid_ident = (std::isalpha(static_cast<unsigned char>(c)) != 0) || c == '_';
                } else {
                    valid_ident = (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_';
                }
            }

            if (valid_ident && !rhs.empty()) {
                ScriptValue val = evaluate_expression(rhs);
                globals_[var_name] = val;
                return true;
            }
        }
    }

    // Not an assignment -- evaluate as expression (likely a function call).
    ScriptValue result = evaluate_expression(trimmed);
    // We don't require expressions to produce a value; side-effects are fine.
    (void)result;
    return last_error_.empty();
}

// ── Expression Evaluation ───────────────────────────────────────────────────

ScriptValue LuaBackend::evaluate_expression(const std::string& expr) {
    std::string trimmed = trim(expr);
    if (trimmed.empty()) return ScriptValue::nil();

    // Check for string concatenation (..) -- handle before anything else
    // so that "a .. b .. c" works.
    {
        // Find ".." not inside strings or parentheses.
        i32 paren_depth = 0;
        bool in_sq = false;
        bool in_dq = false;
        for (size_t i = 0; i < trimmed.size(); ++i) {
            char c = trimmed[i];
            if (c == '\'' && !in_dq) in_sq = !in_sq;
            else if (c == '"' && !in_sq) in_dq = !in_dq;
            else if (c == '(' && !in_sq && !in_dq) ++paren_depth;
            else if (c == ')' && !in_sq && !in_dq) --paren_depth;
            else if (c == '.' && !in_sq && !in_dq && paren_depth == 0 &&
                     i + 1 < trimmed.size() && trimmed[i + 1] == '.') {
                // Make sure it's not "..." (varargs -- three dots).
                if (i + 2 < trimmed.size() && trimmed[i + 2] == '.') {
                    continue;
                }
                return evaluate_concatenation(trimmed);
            }
        }
    }

    // Function call: name(args) or Module.name(args).
    if (is_function_call(trimmed)) {
        ParsedCall pc = parse_function_call(trimmed);
        std::string qualified =
            pc.module.empty() ? pc.function : (pc.module + "." + pc.function);
        return engine_.call_function(qualified, pc.args);
    }

    // Otherwise, treat as a literal / variable reference.
    return parse_value(trimmed);
}

ScriptValue LuaBackend::evaluate_concatenation(const std::string& expr) {
    // Split on ".." that is not inside strings or parentheses.
    std::vector<std::string> parts;
    std::string current;
    i32 paren_depth = 0;
    bool in_sq = false;
    bool in_dq = false;

    for (size_t i = 0; i < expr.size(); ++i) {
        char c = expr[i];
        if (c == '\'' && !in_dq) { in_sq = !in_sq; current += c; continue; }
        if (c == '"' && !in_sq) { in_dq = !in_dq; current += c; continue; }
        if (c == '(' && !in_sq && !in_dq) { ++paren_depth; current += c; continue; }
        if (c == ')' && !in_sq && !in_dq) { --paren_depth; current += c; continue; }

        if (c == '.' && !in_sq && !in_dq && paren_depth == 0 &&
            i + 1 < expr.size() && expr[i + 1] == '.' &&
            !(i + 2 < expr.size() && expr[i + 2] == '.')) {
            parts.push_back(trim(current));
            current.clear();
            ++i; // skip second '.'
            continue;
        }
        current += c;
    }
    parts.push_back(trim(current));

    std::string result;
    for (auto& part : parts) {
        ScriptValue val = evaluate_expression(part);
        result += val.to_string();
    }
    return ScriptValue(result);
}

// ── Function Call Parsing ───────────────────────────────────────────────────

LuaBackend::ParsedCall
LuaBackend::parse_function_call(const std::string& expr) {
    ParsedCall result;

    // Find the opening parenthesis (not inside a string).
    size_t paren_start = std::string::npos;
    bool in_sq = false;
    bool in_dq = false;
    for (size_t i = 0; i < expr.size(); ++i) {
        char c = expr[i];
        if (c == '\'' && !in_dq) in_sq = !in_sq;
        else if (c == '"' && !in_sq) in_dq = !in_dq;
        else if (c == '(' && !in_sq && !in_dq) {
            paren_start = i;
            break;
        }
    }

    if (paren_start == std::string::npos) {
        // Should not happen if is_function_call passed, but be safe.
        return result;
    }

    std::string func_part = trim(expr.substr(0, paren_start));

    // Find matching closing paren.
    size_t paren_end = std::string::npos;
    i32 depth = 0;
    in_sq = false;
    in_dq = false;
    for (size_t i = paren_start; i < expr.size(); ++i) {
        char c = expr[i];
        if (c == '\'' && !in_dq) in_sq = !in_sq;
        else if (c == '"' && !in_sq) in_dq = !in_dq;
        else if (c == '(' && !in_sq && !in_dq) ++depth;
        else if (c == ')' && !in_sq && !in_dq) {
            --depth;
            if (depth == 0) { paren_end = i; break; }
        }
    }

    // Extract args string.
    std::string args_str;
    if (paren_end != std::string::npos && paren_end > paren_start + 1) {
        args_str = expr.substr(paren_start + 1, paren_end - paren_start - 1);
    }

    // Split "Module.func" into module and function.
    auto dot_pos = func_part.find('.');
    if (dot_pos != std::string::npos) {
        result.module = func_part.substr(0, dot_pos);
        result.function = func_part.substr(dot_pos + 1);
    } else {
        result.function = func_part;
    }

    // Parse arguments.
    if (!args_str.empty()) {
        auto tokens = tokenize_args(trim(args_str));
        result.args.reserve(tokens.size());
        for (auto& tok : tokens) {
            result.args.push_back(evaluate_expression(trim(tok)));
        }
    }

    return result;
}

// ── Value Parsing ───────────────────────────────────────────────────────────

ScriptValue LuaBackend::parse_value(const std::string& token) {
    std::string t = trim(token);
    if (t.empty() || t == "nil") return ScriptValue::nil();

    // Booleans.
    if (t == "true") return ScriptValue(true);
    if (t == "false") return ScriptValue(false);

    // Table constructors: { ... }
    if (t.front() == '{' && t.back() == '}') {
        return parse_table_constructor(t);
    }

    // String literals (double-quoted).
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
        return ScriptValue(t.substr(1, t.size() - 2));
    }

    // String literals (single-quoted).
    if (t.size() >= 2 && t.front() == '\'' && t.back() == '\'') {
        return ScriptValue(t.substr(1, t.size() - 2));
    }

    // Numbers -- try integer first, then float.
    {
        bool might_be_number = !t.empty() &&
            (std::isdigit(static_cast<unsigned char>(t[0])) ||
             t[0] == '-' || t[0] == '+');
        if (might_be_number) {
            // Try integer.
            try {
                size_t pos = 0;
                i32 ival = std::stoi(t, &pos);
                if (pos == t.size()) {
                    return ScriptValue(ival);
                }
            } catch (const std::exception&) {
                // Not a valid integer literal — fall through to float parse
            }

            // Try float.
            try {
                size_t pos = 0;
                float fval = std::stof(t, &pos);
                if (pos == t.size()) {
                    return ScriptValue(fval);
                }
            } catch (const std::exception&) {
                // Not a valid float literal — fall through to variable lookup
            }
        }
    }

    // Variable reference -- check local globals, then engine globals.
    {
        auto it = globals_.find(t);
        if (it != globals_.end()) return it->second;
    }

    // Check engine-level globals (includes constants like Input.KEY_SPACE).
    {
        ScriptValue val = engine_.get_global(t);
        if (!val.is_nil()) return val;
    }

    // Dotted names: could be a constant like Input.KEY_SPACE registered as a
    // global, or a module-level constant.
    if (t.find('.') != std::string::npos) {
        ScriptValue val = engine_.get_global(t);
        if (!val.is_nil()) return val;
    }

    // Unknown identifier -- return nil rather than error, matching Lua
    // semantics where undefined variables are nil.
    return ScriptValue::nil();
}

// ── Argument Tokenization ───────────────────────────────────────────────────

std::vector<std::string>
LuaBackend::tokenize_args(const std::string& args_str) {
    std::vector<std::string> tokens;
    std::string current;
    i32 paren_depth = 0;
    bool in_sq = false;
    bool in_dq = false;

    for (size_t i = 0; i < args_str.size(); ++i) {
        char c = args_str[i];

        if (c == '\'' && !in_dq) { in_sq = !in_sq; current += c; continue; }
        if (c == '"' && !in_sq) { in_dq = !in_dq; current += c; continue; }

        if (!in_sq && !in_dq) {
            if (c == '(') { ++paren_depth; current += c; continue; }
            if (c == ')') { --paren_depth; current += c; continue; }
            if (c == ',' && paren_depth == 0) {
                tokens.push_back(current);
                current.clear();
                continue;
            }
        }

        current += c;
    }

    if (!current.empty() || !tokens.empty()) {
        tokens.push_back(current);
    }

    return tokens;
}

// ── Block Collection ────────────────────────────────────────────────────

std::vector<std::string>
LuaBackend::collect_block(std::istringstream& stream, u32& line_number) {
    std::vector<std::string> body;
    std::string line;
    i32 depth = 1; // We already consumed the opening keyword

    while (std::getline(stream, line)) {
        ++line_number;
        std::string t = trim(line);
        if (t.empty() || (t.size() >= 2 && t[0] == '-' && t[1] == '-'))
            continue;

        // Track nesting depth
        if (t.substr(0, 6) == "while " || t.substr(0, 4) == "for " ||
            (t.substr(0, 3) == "if " && t.find("then") != std::string::npos) ||
            t.substr(0, 9) == "function ") {
            ++depth;
        }
        if (t == "end") {
            --depth;
            if (depth == 0) return body;
        }

        body.push_back(t);
    }
    return body;
}

bool LuaBackend::execute_while(const std::string& condition,
                                const std::vector<std::string>& body,
                                u32 line_number, const std::string& source) {
    constexpr u32 MAX_ITERATIONS = 100000;
    u32 iterations = 0;

    while (evaluate_expression(condition).truthy()) {
        if (++iterations > MAX_ITERATIONS) {
            set_error("while loop exceeded max iterations", source, line_number);
            return false;
        }

        for (const auto& stmt : body) {
            if (!execute_line(stmt, line_number, source)) return false;
        }
    }
    return true;
}

bool LuaBackend::execute_for(const std::string& var, i32 start, i32 stop,
                              i32 step, const std::vector<std::string>& body,
                              u32 line_number, const std::string& source) {
    if (step == 0) {
        set_error("for loop step cannot be zero", source, line_number);
        return false;
    }

    constexpr u32 MAX_ITERATIONS = 100000;
    u32 iterations = 0;

    // Preserve any pre-existing global with the loop variable's name so the loop
    // doesn't clobber and then delete it (this interpreter has a flat scope).
    auto prior_it = globals_.find(var);
    const bool had_prior = (prior_it != globals_.end());
    ScriptValue prior_value = had_prior ? prior_it->second : ScriptValue{};

    for (i32 i = start; (step > 0) ? (i <= stop) : (i >= stop); i += step) {
        if (++iterations > MAX_ITERATIONS) {
            set_error("for loop exceeded max iterations", source, line_number);
            return false;
        }

        globals_[var] = ScriptValue(i);
        for (const auto& stmt : body) {
            if (!execute_line(stmt, line_number, source)) return false;
        }
    }

    // Restore the prior binding, or remove the loop variable if there was none.
    if (had_prior) {
        globals_[var] = prior_value;
    } else {
        globals_.erase(var);
    }
    return true;
}

// ── Table Constructor ───────────────────────────────────────────────────

ScriptValue LuaBackend::parse_table_constructor(const std::string& expr) {
    // Parse "{key=val, ...}" or "{val1, val2, ...}"
    std::string inner = trim(expr.substr(1, expr.size() - 2));
    if (inner.empty()) return ScriptValue::table();

    auto items = tokenize_args(inner);
    ScriptValue table = ScriptValue::table();
    i32 array_index = 1; // Lua 1-based arrays

    for (auto& item : items) {
        std::string t = trim(item);
        if (t.empty()) continue;

        // Check for "key = value" form
        auto eq_pos = t.find('=');
        if (eq_pos != std::string::npos && eq_pos > 0 &&
            (eq_pos + 1 >= t.size() || t[eq_pos + 1] != '=') &&
            t[eq_pos - 1] != '~' && t[eq_pos - 1] != '<' &&
            t[eq_pos - 1] != '>') {
            std::string key = trim(t.substr(0, eq_pos));
            std::string val_str = trim(t.substr(eq_pos + 1));
            table.set_field(key, evaluate_expression(val_str));
        } else {
            // Array-style: table[1], table[2], ...
            table.set_field(std::to_string(array_index++),
                           evaluate_expression(t));
        }
    }

    return table;
}

// ── Utilities ───────────────────────────────────────────────────────────────

std::string LuaBackend::trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool LuaBackend::is_function_call(const std::string& expr) {
    // A function call has the pattern: identifier(...)
    // We need to find '(' not inside a string literal.
    bool in_sq = false;
    bool in_dq = false;
    bool found_ident = false;

    for (size_t i = 0; i < expr.size(); ++i) {
        char c = expr[i];
        if (c == '\'' && !in_dq) { in_sq = !in_sq; continue; }
        if (c == '"' && !in_sq) { in_dq = !in_dq; continue; }

        if (!in_sq && !in_dq) {
            if (c == '(' && found_ident) return true;
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
                c == '.') {
                found_ident = true;
            } else if (c != ' ' && c != '\t') {
                found_ident = false;
            }
        }
    }

    return false;
}

void LuaBackend::set_error(const std::string& message,
                            const std::string& source, u32 line) {
    if (line > 0) {
        last_error_ = source + ":" + std::to_string(line) + ": " + message;
    } else {
        last_error_ = message;
    }
    NX_ERROR("LuaBackend: {}", last_error_);
}

} // namespace nexus::scripting
