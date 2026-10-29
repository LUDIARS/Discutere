#include "lib/blackbox/condition.hpp"

#include <stdexcept>

#include "base/strings.hpp"

namespace di::lib::blackbox {

namespace {

// 評価時の入れ子の上限 (検証済みの条件は 8 段まで。DB から読んだ条件もここで止める)。
constexpr int kMaxEvalDepth = 64;

bool is_feature_value(const Json& v) { return v.is_number() || v.is_string() || v.is_boolean(); }

/** JS の === (FeatureValue 同士)。 */
bool strict_equal(const Json& a, const Json& b) {
    if (a.is_number() && b.is_number()) return a.get<double>() == b.get<double>();
    if (a.is_string() && b.is_string()) return a.get_ref<const std::string&>() == b.get_ref<const std::string&>();
    if (a.is_boolean() && b.is_boolean()) return a.get<bool>() == b.get<bool>();
    return false;
}

const Json* feature_of(const FeatureMap& features, const Json& cond) {
    if (!features.is_object()) return nullptr;
    auto f = cond.find("feature");
    if (f == cond.end() || !f->is_string()) return nullptr;
    auto it = features.find(f->get_ref<const std::string&>());
    if (it == features.end() || !is_feature_value(*it)) return nullptr;
    return &*it;
}

bool eval_cmp(const Json* actual, const std::string& op, const Json& expected) {
    if (!actual) return false;
    if (op == "==") return strict_equal(*actual, expected);
    if (op == "!=") return !strict_equal(*actual, expected);
    // 大小比較は number 同士のみ意味を持つ。
    if (!actual->is_number() || !expected.is_number()) return false;
    const double a = actual->get<double>();
    const double e = expected.get<double>();
    if (op == ">") return a > e;
    if (op == ">=") return a >= e;
    if (op == "<") return a < e;
    if (op == "<=") return a <= e;
    return false;
}

struct TooDeep {};

bool eval_at(const Condition& cond, const FeatureMap& features, int depth) {
    // 深すぎる条件は not で反転されないよう、評価全体を false にする (evaluate で捕まえる)。
    if (depth > kMaxEvalDepth) throw TooDeep{};
    if (!cond.is_object()) return false;
    const std::string op = json_string(cond, "op");
    if (op == "and" || op == "or") {
        auto it = cond.find("clauses");
        if (it == cond.end() || !it->is_array()) return false;
        if (op == "and") {
            for (const auto& c : *it)
                if (!eval_at(c, features, depth + 1)) return false;
            return true;
        }
        for (const auto& c : *it)
            if (eval_at(c, features, depth + 1)) return true;
        return false;
    }
    if (op == "not") {
        auto it = cond.find("clause");
        if (it == cond.end()) return false;
        return !eval_at(*it, features, depth + 1);
    }
    if (op == "in") {
        const Json* v = feature_of(features, cond);
        if (!v) return false;
        for (const auto& x : json_array(cond, "values"))
            if (strict_equal(x, *v)) return true;
        return false;
    }
    if (op == "cmp") {
        auto value = cond.find("value");
        if (value == cond.end()) return false;
        return eval_cmp(feature_of(features, cond), json_string(cond, "cmp"), *value);
    }
    return false;
}

std::string member_string(const Json& obj, const char* key) {
    auto it = obj.find(key);
    return it == obj.end() ? std::string("undefined") : js_string(*it);
}

} // namespace

std::string js_string(const Json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_number()) return js_number_to_string(value.get<double>());
    if (value.is_null()) return "null";
    if (value.is_array()) {
        std::string out;
        bool first = true;
        for (const auto& v : value) {
            if (!first) out += ",";
            first = false;
            if (!v.is_null()) out += js_string(v);
        }
        return out;
    }
    return "[object Object]";
}

bool evaluate(const Condition& cond, const FeatureMap& features) {
    try {
        return eval_at(cond, features, 0);
    } catch (const TooDeep&) {
        return false;
    }
}

Condition validate_condition(const Json& value, int depth) {
    if (depth > 8) throw std::invalid_argument("condition nested too deep (max 8)");
    // JS: !value || typeof value !== 'object' (配列は object 扱いで次の op 検査に落ちる)
    if (!value.is_object() && !value.is_array()) throw std::invalid_argument("condition must be an object");
    const Json empty = Json::object();
    const Json& c = value.is_object() ? value : empty;
    auto op_it = c.find("op");
    const std::string op = op_it != c.end() && op_it->is_string() ? op_it->get<std::string>() : std::string();
    if (op == "and" || op == "or") {
        auto it = c.find("clauses");
        if (it == c.end() || !it->is_array() || it->empty()) throw std::invalid_argument(op + " needs non-empty clauses");
        Json clauses = Json::array();
        for (const auto& x : *it) clauses.push_back(validate_condition(x, depth + 1));
        return Json{{"op", op}, {"clauses", std::move(clauses)}};
    }
    if (op == "not") {
        auto it = c.find("clause");
        return Json{{"op", "not"}, {"clause", validate_condition(it == c.end() ? Json() : *it, depth + 1)}};
    }
    if (op == "in") {
        auto f = c.find("feature");
        if (f == c.end() || !f->is_string()) throw std::invalid_argument("in.feature must be string");
        auto vs = c.find("values");
        if (vs == c.end() || !vs->is_array() || vs->empty()) throw std::invalid_argument("in.values must be non-empty array");
        Json values = Json::array();
        for (const auto& v : *vs) {
            if (!v.is_string() && !v.is_number()) throw std::invalid_argument("in.values must be string|number");
            values.push_back(v);
        }
        return Json{{"op", "in"}, {"feature", *f}, {"values", std::move(values)}};
    }
    if (op == "cmp") {
        auto f = c.find("feature");
        if (f == c.end() || !f->is_string()) throw std::invalid_argument("cmp.feature must be string");
        auto cmp = c.find("cmp");
        static const char* kOps[] = {">", ">=", "<", "<=", "==", "!="};
        bool ok = false;
        if (cmp != c.end() && cmp->is_string())
            for (const char* o : kOps)
                if (cmp->get_ref<const std::string&>() == o) ok = true;
        if (!ok) throw std::invalid_argument("cmp.cmp must be one of >,>=,<,<=,==,!=");
        auto v = c.find("value");
        if (v == c.end() || !is_feature_value(*v)) throw std::invalid_argument("cmp.value must be number|string|boolean");
        return Json{{"op", "cmp"}, {"feature", *f}, {"cmp", *cmp}, {"value", *v}};
    }
    throw std::invalid_argument("unknown condition op: " + member_string(c, "op"));
}

namespace {
std::string describe_at(const Condition& cond, int depth) {
    if (depth > kMaxEvalDepth || !cond.is_object()) return "?";
    const std::string op = json_string(cond, "op");
    if (op == "and" || op == "or") {
        std::vector<std::string> parts;
        for (const auto& c : json_array(cond, "clauses")) parts.push_back(describe_at(c, depth + 1));
        if (op == "and") return str::join(parts, " かつ ");
        return "(" + str::join(parts, " または ") + ")";
    }
    if (op == "not") {
        auto it = cond.find("clause");
        return "NOT(" + (it == cond.end() ? std::string("?") : describe_at(*it, depth + 1)) + ")";
    }
    if (op == "in") {
        std::vector<std::string> parts;
        for (const auto& v : json_array(cond, "values")) parts.push_back(js_string(v));
        return member_string(cond, "feature") + " ∈ {" + str::join(parts, ", ") + "}";
    }
    if (op == "cmp") return member_string(cond, "feature") + " " + member_string(cond, "cmp") + " " + member_string(cond, "value");
    return "?";
}
} // namespace

std::string describe_condition(const Condition& cond) { return describe_at(cond, 0); }

} // namespace di::lib::blackbox
