/*******************************************************************************
 * Copyright 2022 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/
#include "ssa_transform.hpp"
#include <map>
#include <utility>
#include <vector>
#include "module_globals_resolve.hpp"
#include <compiler/ir/builder.hpp>
#include <compiler/ir/ssa_data.hpp>
#include <compiler/ir/ssa_visitor.hpp>
#include <util/any_map.hpp>

namespace sc {

struct ssa_var_status_t {
    expr current_value;
    size_t defined_scope_idx;
    // the phi node for the variable that is referenced in the current
    // for-loop, which is defined outside of the loop
    std::vector<expr> for_loop_phi;
};

struct var_cmper_t {
    bool operator()(const expr_c &l, const expr_c &r) const {
        if (l->node_type_ != r->node_type_) {
            return static_cast<int>(l->node_type_)
                    < static_cast<int>(r->node_type_);
        }
        if (l->node_type_ == sc_expr_type::var) {
            return l.static_as<var>()->name_ < r.static_as<var>()->name_;
        } else {
            assert(l->node_type_ == sc_expr_type::tensor);
            return l.static_as<tensor>()->name_ < r.static_as<tensor>()->name_;
        }
    }
};

struct ssa_scope_t {
    // old var => ssa_var_status_t. Using ordered map to make unit tests happy
    std::map<expr_c, ssa_var_status_t, var_cmper_t> vars_;
    enum class kind {
        normal,
        for_loop,
        if_then,
        if_else,
    };
    kind kind_;
    int for_depth_;

    ssa_scope_t(int for_depth, kind kind)
        : kind_(kind), for_depth_(for_depth) {}
};

class ssa_transform_impl_t : public ssa_visitor_t {
public:
    using ssa_visitor_t::dispatch;
    using ssa_visitor_t::visit;
    std::vector<ssa_scope_t> scopes_;
    // if the current expr needs to be flatten in dispatch()
    bool need_flatten_ = true;

    ssa_scope_t &push_scope(ssa_scope_t::kind k) {
        int for_depth;
        if (scopes_.empty()) {
            for_depth = 0;
        } else {
            for_depth = scopes_.back().for_depth_;
        }
        if (k == ssa_scope_t::kind::for_loop) { for_depth++; }
        scopes_.emplace_back(for_depth, k);
        return scopes_.back();
    }

    ssa_scope_t pop_scope() {
        auto ret = std::move(scopes_.back());
        scopes_.pop_back();
        return ret;
    }

    // add an old var definition to scopes, returns the new var
    ssa_var_status_t *insert_local_var(
            const expr_c &old_var, const expr &new_val) {
        auto itr = scopes_.back().vars_.insert(std::make_pair(old_var,
                ssa_var_status_t {
                        new_val, scopes_.size() - 1, std::vector<expr>()}));
        return &itr.first->second;
    }

    ssa_var_status_t *get_local_var_nothrow(const expr_c &old_var) {
        for (auto itr = scopes_.rbegin(); itr != scopes_.rend(); ++itr) {
            auto varitr = (*itr).vars_.find(old_var);
            if (varitr != (*itr).vars_.end()) { return &varitr->second; }
        }
        return nullptr;
    }

    ssa_var_status_t *get_local_var(const expr_c &old_var) {
        auto ret = get_local_var_nothrow(old_var);
        COMPILE_ASSERT(ret, "Undefined var:" << old_var);
        return ret;
    }

    ssa_var_status_t *get_local_var_for_update(const expr_c &old_var) {
        if (is_old_var_global(old_var.get())) { return get_local_var(old_var); }
        auto &back = scopes_.back();
        auto varitr = back.vars_.find(old_var);
        if (varitr != back.vars_.end()) { return &varitr->second; }
        return insert_local_var(old_var, expr());
    }

    bool is_old_var_global(const expr_base *old_var) {
        return old_var->node_type_ == sc_expr_type::var && old_var->attr_
                && old_var->attr_->has_key(attr_keys::module_global_offset);
    }

    void set_var_as_global(const expr_c &old_var) {
        if (!old_var->ssa_data_) {
            old_var.remove_const()->ssa_data_
                    = utils::make_unique<ssa_data_t>();
        }
        old_var->ssa_data_->is_global_ = true;
    }

    ssa_data_t *init_ssa_data(expr_base *ex) {
        assert(!ex->ssa_data_);
        ex->ssa_data_ = utils::make_unique<ssa_data_t>();
        return ex->ssa_data_.get();
    }

    expr_c dispatch(expr_c f) override {
        bool old_need_flatten = need_flatten_;
        need_flatten_ = true;
        auto ret = ssa_visitor_t::dispatch(std::move(f));
        if (old_need_flatten && !ret.isa<var>() && !ret.isa<tensor>()) {
            return add_def(ret);
        }
        return ret;
    }

    func_c dispatch(func_c f) override {
        push_scope(ssa_scope_t::kind::normal);
        std::vector<expr> new_params;
        for (auto &p : f->params_) {
            auto newp = p->remake();
            init_ssa_data(newp.get())->is_param_ = true;
            insert_local_var(p, newp);
            new_params.emplace_back(std::move(newp));
        }
        auto body = dispatch(f->body_);
        pop_scope();
        return copy_attr(*f,
                builder::make_func(f->name_, new_params, body.remove_const(),
                        f->ret_type_));
    }

    expr_c visit(tensor_c v) override {
        auto ret = get_local_var(v);
        return ret->current_value;
    }
    expr_c visit(var_c v) override {
        auto ret = get_local_var(v);
        auto &cur_scope = scopes_.back();
        if (!ret->current_value->ssa_data_->is_global_) {
            if (cur_scope.for_depth_
                    > scopes_[ret->defined_scope_idx].for_depth_) {
                // if the variable depends on a value created outside the
                // current for loop
                auto phi = add_def(make_expr<ssa_phi_node>(
                        std::vector<expr> {ret->current_value}));
                rename_temp_var_with_version(phi.checked_as<var>(), v);
                // update the local var mapping to the phi node
                insert_local_var(v, phi)->for_loop_phi.emplace_back(phi);
                // remember that we need to update this phi node after for-loop
                // exits
                return phi;
            }
        } else {
            // if is global variable, add a "load instance"
            return add_def(ret->current_value);
        }
        return ret->current_value;
    }

    stmt_c visit(define_c v) override {
        expr_c lhs;
        assert(v->linkage_ == linkage::local);

        auto info = insert_local_var(v->var_, expr());
        enum { LOCAL_VAR, GLOBAL_VAR, TENSOR } type;
        if (v->var_.isa<var>()) {
            if (is_old_var_global(v->var_.get())) {
                type = GLOBAL_VAR;
            } else {
                type = LOCAL_VAR;
            }
        } else {
            assert(v->var_.isa<tensor>());
            type = TENSOR;
        }
        if (type == LOCAL_VAR && !v->init_.defined()) {
            // pure local var-def without init value, simply remove it
            info->current_value
                    = make_expr<constant_node>(INT64_C(0), v->var_->dtype_);
            init_ssa_data(info->current_value.get());
            return stmt_c();
        }
        auto newvar = v->var_->remake();
        init_ssa_data(newvar.get());
        info->current_value = newvar;
        if (type == GLOBAL_VAR) { newvar->ssa_data_->is_global_ = true; }
        expr_c init;
        if (v->init_.defined()) {
            need_flatten_ = false;
            init = dispatch(v->init_);
        }
        return copy_attr(*v,
                builder::make_var_tensor_def_unattached(
                        newvar, v->linkage_, init));
    }

    uint64_t var_version_idx = 0;

    void rename_temp_var_with_version(const var &newv, const var_c &old_var) {
        if (newv->ssa_data_->is_local()) {
            newv->name_
                    = old_var->name_ + "_" + std::to_string(var_version_idx++);
        }
    }

    stmt_c visit(assign_c v) override {
        if (v->var_.isa<var>()) {
            auto rhs = dispatch(v->value_);
            auto var_info = get_local_var_for_update(v->var_);
            if (!var_info->current_value.defined()
                    || !var_info->current_value->ssa_data_->is_global_) {
                var_info->current_value = rhs.remove_const();
                assert(var_info->current_value.isa<var>()
                        || var_info->current_value.isa<constant>());
                if (var_info->current_value.isa<var>()) {
                    auto cur_value = var_info->current_value.static_as<var>();
                    rename_temp_var_with_version(
                            cur_value, v->var_.static_as<var>());
                }
                return stmt_c(); // no extra instructions needs to
                // be inserted
            } else {
                // if is global var
                return copy_attr(*v,
                        builder::make_assign_unattached(
                                var_info->current_value, rhs));
            }
        } else {
            assert(v->var_.isa<indexing>());
            need_flatten_ = false;
            auto lhs = dispatch(v->var_);
            return copy_attr(*v,
                    builder::make_assign_unattached(lhs, dispatch(v->value_)));
        }
    }

    stmt_c visit(for_loop_c v) override {
        auto begin = dispatch(v->iter_begin_);
        auto end = dispatch(v->iter_end_);
        auto step = dispatch(v->step_);

        push_scope(ssa_scope_t::kind::for_loop);
        auto thevar = v->var_->remake();
        insert_local_var(v->var_, thevar);
        init_ssa_data(thevar.get());
        auto body = dispatch(v->body_);
        ssa_scope_t scope = pop_scope();
        for (auto &kv : scope.vars_) {
            auto parent_var = get_local_var_nothrow(kv.first);
            if (parent_var) {
                // if the variable is a for-loop-phi
                for (auto &phi : kv.second.for_loop_phi) {
                    if (phi.ptr_same(kv.second.current_value)) {
                        // if the variable is unchanged in loop
                        continue;
                    }
                    // if the variable is changed in loop, we need to update the
                    // phi node input
                    phi->ssa_data_->get_value_of_var()
                            .checked_as<ssa_phi>()
                            ->values_.emplace_back(kv.second.current_value);
                }
                auto new_var = make_expr<ssa_phi_node>(std::vector<expr> {
                        parent_var->current_value, kv.second.current_value});
                auto cur_v = add_def_after_current_stmt(new_var);
                get_local_var_for_update(kv.first)->current_value = cur_v;
                rename_temp_var_with_version(
                        cur_v.checked_as<var>(), kv.first.checked_as<var>());
            }
        }
        return copy_attr(*v,
                builder::make_for_loop_unattached(thevar, begin, end, step,
                        body, v->incremental_, v->kind_));
    }

    stmt_c visit(if_else_c v) override {
        auto cond = dispatch(v->condition_);
        push_scope(ssa_scope_t::kind::if_then);
        auto then_block = dispatch(v->then_case_);
        ssa_scope_t then_scope = pop_scope();

        stmt_c else_block;
        if (v->else_case_.defined()) {
            push_scope(ssa_scope_t::kind::if_else);
            else_block = dispatch(v->else_case_);
            ssa_scope_t else_scope = pop_scope();
            // merge ths diverged variables with phi
            std::map<expr_c, std::vector<expr>, var_cmper_t> updated_vars;
            for (auto &kv : then_scope.vars_) {
                updated_vars[kv.first].emplace_back(kv.second.current_value);
                // let parent for-loop to remember to reset the phi inputs
                auto &ph = get_local_var_for_update(kv.first)->for_loop_phi;
                ph.insert(ph.end(), kv.second.for_loop_phi.begin(),
                        kv.second.for_loop_phi.end());
            }
            for (auto &kv : else_scope.vars_) {
                updated_vars[kv.first].emplace_back(kv.second.current_value);
                auto &ph = get_local_var_for_update(kv.first)->for_loop_phi;
                ph.insert(ph.end(), kv.second.for_loop_phi.begin(),
                        kv.second.for_loop_phi.end());
            }
            for (auto &kv : updated_vars) {
                auto new_phi = make_expr<ssa_phi_node>(kv.second);
                auto new_var = add_def_after_current_stmt(new_phi);
                get_local_var_for_update(kv.first)->current_value = new_var;
                rename_temp_var_with_version(
                        new_var.checked_as<var>(), kv.first.checked_as<var>());
            }
        } else {
            for (auto &kv : then_scope.vars_) {
                auto parent_var = get_local_var_nothrow(kv.first);
                if (parent_var) {
                    auto status = get_local_var_for_update(kv.first);
                    auto &ph = status->for_loop_phi;
                    ph.insert(ph.end(), kv.second.for_loop_phi.begin(),
                            kv.second.for_loop_phi.end());

                    auto new_phi = make_expr<ssa_phi_node>(
                            std::vector<expr> {parent_var->current_value,
                                    kv.second.current_value});
                    auto new_var = add_def_after_current_stmt(new_phi);
                    status->current_value = new_var;
                    rename_temp_var_with_version(new_var.checked_as<var>(),
                            kv.first.checked_as<var>());
                }
            }
        }
        return copy_attr(*v,
                builder::make_if_else_unattached(cond, then_block, else_block));
    }
};

func_c ssa_transform_t::operator()(func_c f) {
    ssa_transform_impl_t impl;
    return impl.top_level_dispatch(std::move(f));
}

stmt_c ssa_transform_t::operator()(stmt_c f) {
    ssa_transform_impl_t impl;
    return impl.top_level_dispatch(std::move(f));
}

} // namespace sc