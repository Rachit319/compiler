#include "executor.h"

executor::executor(ast_node *_tree, object *_parent) {
    tree = _tree;
    parent = _parent;
}

object *executor::init() {
    has_return = false;
    has_continue = false;
    has_break = false;
    object *res = run(tree);
    if (has_return && parent->f_return == o_none)
        err("none function returned non-none object");
    if (!has_return && parent->f_return != o_none)
        err("non-none function returned none");
    if (has_return && return_val->type != parent->f_return)
        err("function return type does not match returned object type");
    if (has_continue)
        err("continue called outside loop");
    if (has_break)
        err("break called outside loop");
    return has_return ? return_val : res;
}

namespace {
bool truthy(object *o) {
    if (o == nullptr)
        return false;
    switch (o->type) {
        case o_none:
            return false;
        case o_bool:
            return std::get<bool>(o->store);
        case o_num:
            return std::get<double>(o->store) != 0.0;
        case o_str:
            return !std::get<std::string>(o->store).empty();
        case o_arr:
            return !std::get<std::vector<object *>>(o->store).empty();
        case o_queue:
            return !std::get<std::queue<object *>>(o->store).empty();
        case o_stack:
            return !std::get<std::stack<object *>>(o->store).empty();
        case o_set:
            return !std::get<std::unordered_set<object *, obj_hash, obj_equals>>(o->store).empty();
        case o_map:
            return !std::get<std::unordered_map<object *, object *, obj_hash, obj_equals>>(o->store).empty();
        case o_fn:
            return true;
        default:
            return false;
    }
}
} // namespace

object *executor::run(ast_node *u) {
    if (u == nullptr)
        return new object();

    // Sequence execution.
    if (u->val.type == t_group && u->val.val == "GROUP") {
        object *last = new object();
        for (size_t i = 0; i < u->children.size(); ++i) {
            ast_node *curr = &u->children[i];

            // Handle if/elsif/else chaining at the GROUP level so we can skip the right branches.
            if (curr->val.val == "if") {
                bool executed = false;

                // Evaluate "if" then any following "elsif".
                size_t j = i;
                for (; j < u->children.size(); ++j) {
                    ast_node *branch = &u->children[j];
                    if (branch->val.val != "if" && branch->val.val != "elsif")
                        break;

                    object *cond_obj = run(&branch->children[0]);
                    if (!executed && truthy(cond_obj)) {
                        last = run(&branch->children[1]);
                        executed = true;
                    }

                    if (has_return || has_break || has_continue)
                        return last;
                }

                // Optional "else" directly after the elsif-chain (always skipped if present).
                size_t chain_end = j;
                if (chain_end < u->children.size() && u->children[chain_end].val.val == "else") {
                    if (!executed) {
                        last = run(&u->children[chain_end].children[0]);
                        executed = true;
                        if (has_return || has_break || has_continue)
                            return last;
                    }
                    ++chain_end;
                }

                i = chain_end - 1; // loop will ++i
                continue;
            }

            last = run(curr);
            if (has_return || has_break || has_continue)
                return last;
        }
        return last;
    }

    // Control flow / statements.
    if (u->val.type == t_builtin) {
        if (u->val.val == "break") {
            has_break = true;
            return new object();
        }
        if (u->val.val == "continue") {
            has_continue = true;
            return new object();
        }
        if (u->val.val == "return") {
            if (u->children.size() != 1)
                err("return must have exactly one expression");
            has_return = true;
            return_val = run(&u->children[0]);
            return return_val;
        }
        if (u->val.val == "out" || u->val.val == "outl") {
            if (u->children.size() != 1)
                err(u->val.val + " must have exactly one expression");
            object *v = run(&u->children[0]);
            if (u->val.val == "out")
                std::cout << v->str();
            else
                std::cout << v->str() << std::endl;
            return new object();
        }
        if (u->val.val == "in") {
            if (u->children.size() != 1 || u->children[0].val.type != t_symbol)
                err("in must target a symbol");
            std::string id = u->children[0].val.val;
            object *dst = memory::get(id);
            if (dst->type == o_num) {
                double x;
                std::cin >> x;
                object tmp(o_num);
                tmp.set(x);
                dst->equal(&tmp);
            } else if (dst->type == o_bool) {
                std::string s;
                std::cin >> s;
                object tmp(o_bool);
                tmp.set(s == "true" || s == "1");
                dst->equal(&tmp);
            } else if (dst->type == o_str) {
                std::string s;
                std::cin >> std::ws;
                std::getline(std::cin, s);
                object tmp(o_str);
                tmp.set(s);
                dst->equal(&tmp);
            } else {
                err("in() not supported for this variable type");
            }
            return new object();
        }

        // Variable declarations: num x, bool b, str s, arr a, ...
        if (token::vars.find(u->val.val) != token::vars.end() && u->children.size() == 1 &&
            u->children[0].val.type == t_symbol) {
            std::vector<token> decl;
            decl.push_back(u->val);
            decl.push_back(u->children[0].val);
            interpreter::declare_obj(decl, false /* to_global */);
            return new object();
        }

        // Block statements.
        if (u->val.val == "if" || u->val.val == "elsif") {
            // These should normally be executed by the GROUP-level chain handler.
            object *cond_obj = run(&u->children[0]);
            if (truthy(cond_obj))
                return run(&u->children[1]);
            return new object();
        }
        if (u->val.val == "else") {
            // Else without its associated if/elsif is likely a parse/usage error.
            if (u->children.size() != 1)
                err("else block format invalid");
            return run(&u->children[0]);
        }

        if (u->val.val == "while") {
            object *last = new object();
            while (truthy(run(&u->children[0]))) {
                last = run(&u->children[1]);
                if (has_return)
                    return last;
                if (has_break) {
                    has_break = false;
                    return last;
                }
                if (has_continue) {
                    has_continue = false;
                    continue;
                }
            }
            return last;
        }

        if (u->val.val == "for") {
            // Expected syntax: for <symbol> of <iterable> start ... end
            if (u->children.size() != 2)
                err("for block format invalid");

            ast_node *spec = &u->children[0]; // usually an "of" node
            if (spec->val.val != "of" || spec->children.size() != 2 || spec->children[0].val.type != t_symbol)
                err("for loop spec must be: <symbol> of <expr>");

            std::string loop_id = spec->children[0].val.val;

            object *iterable = run(&spec->children[1]);
            // Support iterating over arrays produced by range(n).
            std::vector<object *> elems;
            if (iterable->type == o_arr)
                elems = std::get<std::vector<object *>>(iterable->store);
            else
                err("for() currently only supports iteration over arr");

            object *loop_var = nullptr;
            if (memory::has(loop_id)) {
                loop_var = memory::get(loop_id);
            } else {
                object *tmp = new object(o_num);
                tmp->set(0.0);
                memory::add(loop_id, tmp, false);
                loop_var = tmp;
            }

            object *last = new object();
            for (object *elem : elems) {
                loop_var->equal(elem);
                last = run(&u->children[1]);
                if (has_return)
                    return last;
                if (has_break) {
                    has_break = false;
                    break;
                }
                if (has_continue) {
                    has_continue = false;
                    continue;
                }
            }
            return last;
        }
    }

    // Dot operator: member/method call (a.at(0), a.push(x), x.floor(), ...)
    if (u->val.type == t_builtin && u->val.val == ".") {
        if (u->children.size() != 2)
            err("member access '.' expects exactly 2 operands");

        object *recv = run(&u->children[0]);
        ast_node *rhs = &u->children[1];
        if (rhs->val.type != t_symbol)
            err("member name must be a symbol");

        std::string method = rhs->val.val;
        std::vector<object *> args;
        for (size_t i = 0; i < rhs->children.size(); ++i)
            args.push_back(run(&rhs->children[i]));

        // Dispatch to the appropriate object method.
        if (method == "len" && args.empty())
            return recv->len();
        if (method == "empty" && args.empty())
            return recv->empty();
        if (method == "reverse" && args.empty())
            return recv->reverse();
        if (method == "at" && args.size() == 1)
            return recv->at(args[0]);
        if (method == "push" && args.size() == 1)
            return recv->push(args[0]);
        if (method == "pop" && args.empty())
            return recv->pop();
        if (method == "find" && args.size() == 1)
            return recv->find(args[0]);
        if (method == "next" && args.empty())
            return recv->next();
        if (method == "last" && args.empty())
            return recv->last();
        if (method == "clear" && args.empty())
            return recv->clear();
        if (method == "sort" && args.empty())
            return recv->sort();
        if (method == "fill" && args.size() == 3)
            return recv->fill(args[0], args[1], args[2]);
        if (method == "sub") {
            if (args.empty())
                return recv->sub();
            if (args.size() == 1)
                return recv->sub(args[0]);
            if (args.size() == 2)
                return recv->sub(args[0], args[1]);
            if (args.size() == 3)
                return recv->sub(args[0], args[1], args[2]);
        }
        if (method == "floor" && args.empty())
            return recv->floor();
        if (method == "ceil" && args.empty())
            return recv->ceil();
        if (method == "round" && args.size() == 1)
            return recv->round(args[0]);
        if (method == "to_bool" && args.empty())
            return recv->to_bool();

        err("unknown member method: " + method);
        return new object();
    }

    // Literals.
    if (u->val.type == t_num) {
        object *ret = new object(o_num);
        ret->set(std::stod(u->val.val));
        return ret;
    }
    if (u->val.type == t_str) {
        object *ret = new object(o_str);
        ret->set(u->val.val);
        return ret;
    }

    // Symbols: either variables, function calls, or built-in calls.
    if (u->val.type == t_symbol) {
        std::string name = u->val.val;

        // Built-in "range(n)" for loops.
        if (name == "range") {
            if (u->children.size() != 1)
                err("range() expects exactly one argument");
            object *n_obj = run(&u->children[0]);
            if (n_obj->type != o_num || !n_obj->is_int())
                err("range() argument must be an integer num");
            int n = (int) std::get<double>(n_obj->store);
            if (n < 0)
                err("range() argument must be non-negative");

            object *arr = new object(o_arr);
            std::vector<object *> elems;
            elems.reserve((size_t)n);
            for (int i = 0; i < n; ++i) {
                object *x = new object(o_num);
                x->set((double)i);
                elems.push_back(x);
            }
            arr->set(elems);
            return arr;
        }

        // Built-in math functions: floor/ceil/round/rand.
        if (token::methods.find(name) != token::methods.end()) {
            if (name == "rand" && u->children.empty())
                return object::rand();

            if (name == "floor" && u->children.size() == 1) {
                object *x = run(&u->children[0]);
                return x->floor();
            }
            if (name == "ceil" && u->children.size() == 1) {
                object *x = run(&u->children[0]);
                return x->ceil();
            }
            if (name == "round" && u->children.size() == 2) {
                object *x = run(&u->children[0]);
                object *prec = run(&u->children[1]);
                return x->round(prec);
            }

            err("invalid argument count for builtin: " + name);
        }

        // User-defined functions or variables.
        object *ref = memory::get(name);
        if (ref->type == o_fn) {
            if (ref->f_params.size() != u->children.size())
                err("function parameter count mismatch for: " + name);

            std::vector<object *> args;
            args.reserve(u->children.size());
            for (size_t i = 0; i < u->children.size(); ++i)
                args.push_back(run(&u->children[i]));

            memory::push();
            for (size_t i = 0; i < ref->f_params.size(); ++i) {
                const f_param &p = ref->f_params[i];
                object *param_var = new object(p.type);
                param_var->equal(args[i]);
                memory::add(p.symbol, param_var, false);
            }

            executor callee(ref->f_body, ref);
            object *res = callee.init();
            memory::pop();
            return res;
        }

        // Variable reference must not look like a call.
        if (!u->children.empty())
            err("cannot call a non-function symbol: " + name);
        return ref;
    }

    // Operators / expressions.
    if (u->val.type == t_builtin) {
        std::string op = u->val.val;
        if (u->children.size() == 2) {
            object *lhs = run(&u->children[0]);
            object *rhs = run(&u->children[1]);

            // Assignment.
            if (op == "=") {
                // LHS must be a variable symbol.
                if (u->children[0].val.type != t_symbol)
                    err("assignment LHS must be a symbol");
                lhs->equal(rhs);
                return new object();
            }
            if (op == "+=")
                return lhs->add_equal(rhs);
            if (op == "-=")
                return lhs->subtract_equal(rhs);
            if (op == "*=")
                return lhs->multiply_equal(rhs);
            if (op == "**=")
                return lhs->power_equal(rhs);
            if (op == "/=")
                return lhs->divide_equal(rhs);
            if (op == "//=")
                return lhs->truncate_divide_equal(rhs);
            if (op == "%=")
                return lhs->modulo_equal(rhs);
            if (op == "^=")
                return lhs->b_xor_equal(rhs);
            if (op == "|=")
                return lhs->b_or_equal(rhs);
            if (op == "&=")
                return lhs->b_and_equal(rhs);
            if (op == ">>=")
                return lhs->b_right_shift_equal(rhs);
            if (op == "<<=")
                return lhs->b_left_shift_equal(rhs);

            // Arithmetic.
            if (op == "+")
                return lhs->add(rhs);
            if (op == "-")
                return lhs->subtract(rhs);
            if (op == "*")
                return lhs->multiply(rhs);
            if (op == "**")
                return lhs->power(rhs);
            if (op == "/")
                return lhs->divide(rhs);
            if (op == "//")
                return lhs->truncate_divide(rhs);
            if (op == "%")
                return lhs->modulo(rhs);
            if (op == "^")
                return lhs->b_xor(rhs);
            if (op == "|")
                return lhs->b_or(rhs);
            if (op == "&")
                return lhs->b_and(rhs);
            if (op == ">>")
                return lhs->b_right_shift(rhs);
            if (op == "<<")
                return lhs->b_left_shift(rhs);

            // Comparisons.
            if (op == "==")
                return lhs->equals(rhs);
            if (op == "!=")
                return lhs->not_equals(rhs);
            if (op == ">")
                return lhs->greater_than(rhs);
            if (op == "<")
                return lhs->less_than(rhs);
            if (op == ">=")
                return lhs->greater_than_equal_to(rhs);
            if (op == "<=")
                return lhs->less_than_equal_to(rhs);

            if (op == "and")
                return lhs->_and(rhs);
            if (op == "or")
                return lhs->_or(rhs);

            err("unrecognized binary operator: " + op);
        } else if (u->children.size() == 1) {
            object *v = run(&u->children[0]);
            if (u->val.val == "not")
                return v->_not();
            if (u->val.val == "-") {
                // Unary minus is represented as 0 - x by the AST generator.
                object zero(o_num);
                return zero.subtract(v);
            }

            err("unrecognized unary operator: " + u->val.val);
        }
    }

    // Unhandled node type/value.
    err("unhandled AST node");
    return new object();
}
