/*++
Copyright (c) 2006 Microsoft Corporation

Module Name:

    asserted_formulas.cpp

Abstract:

    <abstract>

Author:

    Leonardo de Moura (leonardo) 2008-06-11.

Revision History:

--*/
#include "util/warning.h"
#include "ast/ast_ll_pp.h"
#include "ast/ast_pp.h"
#include "ast/for_each_expr.h"
#include "ast/well_sorted.h"
#include "ast/simplifier/arith_simplifier_plugin.h"
#include "ast/simplifier/array_simplifier_plugin.h"
#include "ast/simplifier/datatype_simplifier_plugin.h"
#include "ast/simplifier/fpa_simplifier_plugin.h"
#include "ast/simplifier/seq_simplifier_plugin.h"
#include "ast/simplifier/bv_simplifier_plugin.h"
#include "ast/simplifier/pull_ite_tree.h"
#include "ast/simplifier/push_app_ite.h"
#include "ast/simplifier/bv_elim.h"
#include "ast/simplifier/inj_axiom.h"
#include "ast/simplifier/elim_bounds.h"
#include "ast/simplifier/bit2int.h"
#include "ast/normal_forms/pull_quant.h"
#include "ast/normal_forms/nnf.h"
#include "ast/pattern/pattern_inference.h"
#include "ast/rewriter/der.h"
#include "ast/rewriter/distribute_forall.h"
#include "ast/macros/quasi_macros.h"
#include "smt/asserted_formulas.h"
#include "smt/elim_term_ite.h"

asserted_formulas::asserted_formulas(ast_manager & m, smt_params & p):
    m(m),
    m_params(p),
    m_pre_simplifier(m),
    m_simplifier(m),
    m_defined_names(m),
    m_static_features(m),
    m_asserted_formulas(m),
    m_asserted_formula_prs(m),
    m_asserted_qhead(0),
    m_macro_manager(m, m_simplifier),
    m_bit2int(m),
    m_bv_sharing(m),
    m_inconsistent(false){

    m_bsimp = 0;
    m_bvsimp = 0;
    arith_simplifier_plugin * arith_simp = 0;
    setup_simplifier_plugins(m_simplifier, m_bsimp, arith_simp, m_bvsimp);
    SASSERT(m_bsimp != 0);
    SASSERT(arith_simp != 0);
    m_macro_finder = alloc(macro_finder, m, m_macro_manager);

    basic_simplifier_plugin * basic_simp = 0;
    bv_simplifier_plugin * bv_simp = 0;
    setup_simplifier_plugins(m_pre_simplifier, basic_simp, arith_simp, bv_simp);
    m_bit2int.set_bv_simplifier(bv_simp);
    m_pre_simplifier.enable_presimp();
}

void asserted_formulas::setup() {
    switch (m_params.m_lift_ite) {
    case LI_FULL:
        m_params.m_ng_lift_ite = LI_NONE; 
        break;
    case LI_CONSERVATIVE:
        if (m_params.m_ng_lift_ite == LI_CONSERVATIVE)
            m_params.m_ng_lift_ite = LI_NONE;
        break;
    default:
        break;
    }
 
    if (m_params.m_relevancy_lvl == 0)
        m_params.m_relevancy_lemma = false;
}

void asserted_formulas::setup_simplifier_plugins(simplifier & s, basic_simplifier_plugin * & bsimp, arith_simplifier_plugin * & asimp, bv_simplifier_plugin * & bvsimp) {
    bsimp = alloc(basic_simplifier_plugin, m);
    s.register_plugin(bsimp);
    asimp = alloc(arith_simplifier_plugin, m, *bsimp, m_params);
    s.register_plugin(asimp);
    s.register_plugin(alloc(array_simplifier_plugin, m, *bsimp, s, m_params));
    bvsimp = alloc(bv_simplifier_plugin, m, *bsimp, m_params);
    s.register_plugin(bvsimp);
    s.register_plugin(alloc(datatype_simplifier_plugin, m, *bsimp));    
    s.register_plugin(alloc(fpa_simplifier_plugin, m, *bsimp));
    s.register_plugin(alloc(seq_simplifier_plugin, m, *bsimp));
}

void asserted_formulas::init(unsigned num_formulas, expr * const * formulas, proof * const * prs) {
    SASSERT(m_asserted_formulas.empty());
    SASSERT(m_asserted_formula_prs.empty());
    SASSERT(!m_inconsistent);
    SASSERT(m_scopes.empty());
    m_asserted_formulas.append(num_formulas, formulas);
    if (m.proofs_enabled())
        m_asserted_formula_prs.append(num_formulas, prs);
}

bool asserted_formulas::has_bv() const {
    // approaximated answer... assume the formula has bit-vectors if the bv_simplifier_plugin was invoked at least once.
    return m_bvsimp->reduce_invoked();
}

asserted_formulas::~asserted_formulas() {
}

void asserted_formulas::push_assertion(expr * e, proof * pr, expr_ref_vector & result, proof_ref_vector & result_prs) {
    if (inconsistent()) {
        SASSERT(!result.empty());
        return;
    }
    if (m.is_false(e))
        m_inconsistent = true;
    ::push_assertion(m, e, pr, result, result_prs);
}

void asserted_formulas::set_eliminate_and(bool flag) {
    if (m_bsimp->eliminate_and() == flag)
        return;
    TRACE("eliminate_and", tout << "flushing cache...\n";);
    flush_cache();
    m_bsimp->set_eliminate_and(flag);
}


void asserted_formulas::assert_expr(expr * e, proof * _in_pr) {
    if (inconsistent()) 
        return;
    if (!m_params.m_preprocess) {
        push_assertion(e, _in_pr, m_asserted_formulas, m_asserted_formula_prs);
        return;
    }
    proof_ref  in_pr(_in_pr, m);
    expr_ref   r1(m);
    proof_ref  pr1(m);
    expr_ref   r2(m);
    proof_ref  pr2(m);
    TRACE("assert_expr_before_simp", tout << mk_ll_pp(e, m) << "\n";);
    TRACE("assert_expr_bug", tout << mk_pp(e, m) << "\n";);
    if (m_params.m_pre_simplifier) {
        m_pre_simplifier(e, r1, pr1);
    }
    else {
        r1  = e;
        pr1 = 0;
    }
    set_eliminate_and(false); // do not eliminate and before nnf.
    m_simplifier(r1, r2, pr2);
    TRACE("assert_expr_bug", tout << "after...\n" << mk_pp(r1, m) << "\n";);
    if (m.proofs_enabled()) {
        if (e == r2)
            pr2 = in_pr;
        else
            pr2 = m.mk_modus_ponens(in_pr, m.mk_transitivity(pr1, pr2));
    }
    TRACE("assert_expr_after_simp", tout << mk_ll_pp(r1, m) << "\n";);
    push_assertion(r2, pr2, m_asserted_formulas, m_asserted_formula_prs);
    TRACE("asserted_formulas_bug", tout << "after assert_expr\n"; display(tout););
}

void asserted_formulas::assert_expr(expr * e) {
    if (inconsistent()) 
        return;
    assert_expr(e, m.mk_asserted(e));
}

void asserted_formulas::get_assertions(ptr_vector<expr> & result) {
    result.append(m_asserted_formulas.size(), m_asserted_formulas.c_ptr());
}

void asserted_formulas::push_scope() {
    SASSERT(inconsistent() || m_asserted_qhead == m_asserted_formulas.size() || m.canceled());
    TRACE("asserted_formulas_scopes", tout << "push:\n"; display(tout););
    m_scopes.push_back(scope());
    m_macro_manager.push_scope();
    scope & s = m_scopes.back();
    s.m_asserted_formulas_lim    = m_asserted_formulas.size();
    SASSERT(inconsistent() || s.m_asserted_formulas_lim == m_asserted_qhead || m.canceled());
    s.m_inconsistent_old         = m_inconsistent;
    m_defined_names.push();
    m_bv_sharing.push_scope();
    commit();
}
 
void asserted_formulas::pop_scope(unsigned num_scopes) {
    TRACE("asserted_formulas_scopes", tout << "before pop " << num_scopes << "\n"; display(tout););
    m_bv_sharing.pop_scope(num_scopes);
    m_macro_manager.pop_scope(num_scopes);
    unsigned new_lvl    = m_scopes.size() - num_scopes;
    scope & s           = m_scopes[new_lvl];
    m_inconsistent      = s.m_inconsistent_old;
    m_defined_names.pop(num_scopes);
    m_asserted_formulas.shrink(s.m_asserted_formulas_lim);
    if (m.proofs_enabled())
        m_asserted_formula_prs.shrink(s.m_asserted_formulas_lim);
    m_asserted_qhead    = s.m_asserted_formulas_lim;
    m_scopes.shrink(new_lvl);
    flush_cache();
    TRACE("asserted_formulas_scopes", tout << "after pop " << num_scopes << "\n"; display(tout););
}

void asserted_formulas::reset() {
    m_defined_names.reset();
    m_asserted_qhead = 0;
    m_asserted_formulas.reset();
    m_asserted_formula_prs.reset();
    m_macro_manager.reset();
    m_bv_sharing.reset();
    m_inconsistent = false;
}


#ifdef Z3DEBUG
bool asserted_formulas::check_well_sorted() const {
    for (unsigned i = 0; i < m_asserted_formulas.size(); i++) { 
        if (!is_well_sorted(m, m_asserted_formulas.get(i))) return false; 
    }
    return true;
}
#endif

void asserted_formulas::reduce() {
    if (inconsistent()) 
        return;
    if (canceled()) {
        return;
    }
    if (m_asserted_qhead == m_asserted_formulas.size())
        return;
    if (!m_params.m_preprocess)
        return;

    if (m_macro_manager.has_macros())
        expand_macros();
    TRACE("before_reduce", display(tout););
    CASSERT("well_sorted", check_well_sorted());


#define INVOKE(COND, FUNC) if (COND) { FUNC; IF_VERBOSE(10000, verbose_stream() << "total size: " << get_total_size() << "\n";); }  TRACE("reduce_step_ll", ast_mark visited; display_ll(tout, visited);); TRACE("reduce_step", display(tout << #FUNC << " ");); CASSERT("well_sorted",check_well_sorted()); if (inconsistent() || canceled()) { TRACE("after_reduce", display(tout);); TRACE("after_reduce_ll", ast_mark visited; display_ll(tout, visited);); return;  }
    
    set_eliminate_and(false); // do not eliminate and before nnf.
    INVOKE(m_params.m_propagate_booleans, propagate_booleans());
    INVOKE(m_params.m_propagate_values, propagate_values());
    INVOKE(m_params.m_macro_finder && has_quantifiers(), find_macros());
    INVOKE(m_params.m_nnf_cnf || (m_params.m_mbqi && has_quantifiers()), nnf_cnf());
    INVOKE(m_params.m_eliminate_and, eliminate_and());
    INVOKE(m_params.m_pull_cheap_ite_trees, pull_cheap_ite_trees());
    INVOKE(m_params.m_pull_nested_quantifiers && has_quantifiers(), pull_nested_quantifiers());
    INVOKE(m_params.m_ng_lift_ite != LI_NONE, ng_lift_ite());
    INVOKE(m_params.m_lift_ite != LI_NONE, lift_ite());
    INVOKE(m_params.m_eliminate_term_ite && m_params.m_lift_ite != LI_FULL, eliminate_term_ite());
    INVOKE(m_params.m_refine_inj_axiom && has_quantifiers(), refine_inj_axiom());
    INVOKE(m_params.m_distribute_forall && has_quantifiers(), apply_distribute_forall());    
    TRACE("qbv_bug", tout << "after distribute_forall:\n"; display(tout););    
    INVOKE(m_params.m_macro_finder && has_quantifiers(), find_macros());
    INVOKE(m_params.m_quasi_macros && has_quantifiers(), apply_quasi_macros());    
    INVOKE(m_params.m_simplify_bit2int, apply_bit2int());
    INVOKE(m_params.m_eliminate_bounds && has_quantifiers(), cheap_quant_fourier_motzkin());
    INVOKE(m_params.m_ematching && has_quantifiers(), infer_patterns());
    INVOKE(m_params.m_max_bv_sharing && has_bv(), max_bv_sharing());
    INVOKE(m_params.m_bb_quantifiers, elim_bvs_from_quantifiers());
    // temporary HACK: make sure that arith & bv are list-assoc 
    // this may destroy some simplification steps such as max_bv_sharing
    reduce_asserted_formulas(); 

    CASSERT("well_sorted",check_well_sorted());

    IF_VERBOSE(10, verbose_stream() << "(smt.simplifier-done)\n";);
    TRACE("after_reduce", display(tout););
    TRACE("after_reduce_ll", ast_mark visited; display_ll(tout, visited););
    TRACE("macros", m_macro_manager.display(tout););
    flush_cache();
}

void asserted_formulas::eliminate_and() {
    IF_IVERBOSE(10, verbose_stream() << "(smt.eliminating-and)\n";);
    set_eliminate_and(true);
    reduce_asserted_formulas();    
    TRACE("after_elim_and", display(tout););
}

unsigned asserted_formulas::get_formulas_last_level() const {
    if (m_scopes.empty()) {
        return 0;
    }
    else {
        return m_scopes.back().m_asserted_formulas_lim;
    }
}

void asserted_formulas::collect_static_features() {
    if (m_params.m_display_features) {
        unsigned sz   = m_asserted_formulas.size();
        unsigned head = m_asserted_qhead;
        while (head < sz) {
            expr * f   = m_asserted_formulas.get(head);
            head++;
            m_static_features.collect(f);
        }
        m_static_features.display_primitive(std::cout);
        m_static_features.display(std::cout);
    }
}

void asserted_formulas::display(std::ostream & out) const {
    out << "asserted formulas:\n";
    for (unsigned i = 0; i < m_asserted_formulas.size(); i++) {
        if (i == m_asserted_qhead)
            out << "[HEAD] ==>\n";
        out << mk_pp(m_asserted_formulas.get(i), m) << "\n";
    }
    out << "inconsistent: " << inconsistent() << "\n";
}

void asserted_formulas::display_ll(std::ostream & out, ast_mark & pp_visited) const {
    if (!m_asserted_formulas.empty()) {
        unsigned sz = m_asserted_formulas.size();
        for (unsigned i = 0; i < sz; i++) 
            ast_def_ll_pp(out, m, m_asserted_formulas.get(i), pp_visited, true, false);
        out << "asserted formulas:\n";
        for (unsigned i = 0; i < sz; i++) 
            out << "#" << m_asserted_formulas[i]->get_id() << " ";
        out << "\n";
    }
}

void asserted_formulas::collect_statistics(statistics & st) const {
}

void asserted_formulas::reduce_asserted_formulas() {
    if (inconsistent()) {
        return;
    }
    expr_ref_vector  new_exprs(m);
    proof_ref_vector new_prs(m);
    unsigned i  = m_asserted_qhead;
    unsigned sz = m_asserted_formulas.size();
    for (; i < sz && !inconsistent(); i++) {
        expr * n    = m_asserted_formulas.get(i);
        SASSERT(n != 0);
        proof * pr  = m_asserted_formula_prs.get(i, 0);
        expr_ref new_n(m);
        proof_ref new_pr(m);
        m_simplifier(n, new_n, new_pr);
        TRACE("reduce_asserted_formulas", tout << mk_pp(n, m) << " -> " << mk_pp(new_n, m) << "\n";);
        if (n == new_n.get()) {
            push_assertion(n, pr, new_exprs, new_prs);
        }
        else {
            new_pr = m.mk_modus_ponens(pr, new_pr);
            push_assertion(new_n, new_pr, new_exprs, new_prs);
        }
        if (canceled()) {
            return;
        }
    }
    swap_asserted_formulas(new_exprs, new_prs);
}

void asserted_formulas::swap_asserted_formulas(expr_ref_vector & new_exprs, proof_ref_vector & new_prs) {
    SASSERT(!inconsistent() || !new_exprs.empty());
    m_asserted_formulas.shrink(m_asserted_qhead);
    m_asserted_formulas.append(new_exprs);
    if (m.proofs_enabled()) {
        m_asserted_formula_prs.shrink(m_asserted_qhead);
        m_asserted_formula_prs.append(new_prs);
    }
}

void asserted_formulas::find_macros_core() {
    expr_ref_vector  new_exprs(m);
    proof_ref_vector new_prs(m);
    unsigned sz = m_asserted_formulas.size();
    m_macro_finder->operator()(sz - m_asserted_qhead, m_asserted_formulas.c_ptr() + m_asserted_qhead, 
                               m_asserted_formula_prs.c_ptr() + m_asserted_qhead, new_exprs, new_prs);
    swap_asserted_formulas(new_exprs, new_prs);
    reduce_and_solve();
}

void asserted_formulas::find_macros() {
    IF_IVERBOSE(10, verbose_stream() << "(smt.find-macros)\n";);
    TRACE("before_find_macros", display(tout););
    find_macros_core();
    TRACE("after_find_macros", display(tout););
}

void asserted_formulas::expand_macros() {
    IF_IVERBOSE(10, verbose_stream() << "(smt.expand-macros)\n";);
    find_macros_core();
}

void asserted_formulas::apply_quasi_macros() {
    IF_IVERBOSE(10, verbose_stream() << "(smt.find-quasi-macros)\n";);
    TRACE("before_quasi_macros", display(tout););
    expr_ref_vector  new_exprs(m);
    proof_ref_vector new_prs(m);      
    quasi_macros proc(m, m_macro_manager, m_simplifier);    
    while (proc(m_asserted_formulas.size() - m_asserted_qhead, 
                m_asserted_formulas.c_ptr() + m_asserted_qhead, 
                m_asserted_formula_prs.c_ptr() + m_asserted_qhead,
                new_exprs, new_prs)) {
        swap_asserted_formulas(new_exprs, new_prs);
        new_exprs.reset();
        new_prs.reset();
    }
    TRACE("after_quasi_macros", display(tout););
    reduce_and_solve();
}

void asserted_formulas::nnf_cnf() {
    IF_IVERBOSE(10, verbose_stream() << "(smt.nnf)\n";);
    nnf              apply_nnf(m, m_defined_names);
    expr_ref_vector  new_exprs(m);
    proof_ref_vector new_prs(m);
    expr_ref_vector  push_todo(m);
    proof_ref_vector push_todo_prs(m);
    
    unsigned i  = m_asserted_qhead;
    unsigned sz = m_asserted_formulas.size();
    TRACE("nnf_bug", tout << "i: " << i << " sz: " << sz << "\n";);
    for (; i < sz; i++) {
        expr * n    = m_asserted_formulas.get(i);
        TRACE("nnf_bug", tout << "processing:\n" << mk_pp(n, m) << "\n";);
        proof * pr  = m_asserted_formula_prs.get(i, 0);
        expr_ref   r1(m);
        proof_ref  pr1(m);
        CASSERT("well_sorted",is_well_sorted(m, n));
        push_todo.reset();
        push_todo_prs.reset();
        apply_nnf(n, push_todo, push_todo_prs, r1, pr1);
        CASSERT("well_sorted",is_well_sorted(m, r1));
        pr = m.mk_modus_ponens(pr, pr1);
        push_todo.push_back(r1);
        push_todo_prs.push_back(pr);

        if (canceled()) {
            return;
        }
        unsigned sz2 = push_todo.size();
        for (unsigned k = 0; k < sz2; k++) {
            expr * n   = push_todo.get(k);
            proof * pr = 0;
            m_simplifier(n, r1, pr1);
            CASSERT("well_sorted",is_well_sorted(m, r1));
            if (canceled()) {
                return;
            }        
            
            if (m.proofs_enabled())
                pr = m.mk_modus_ponens(push_todo_prs.get(k), pr1);
            else
                pr = 0;
            push_assertion(r1, pr, new_exprs, new_prs);
        }
    }
    swap_asserted_formulas(new_exprs, new_prs);
}

#define MK_SIMPLE_SIMPLIFIER(NAME, FUNCTOR_DEF, LABEL, MSG)                                                             \
void asserted_formulas::NAME() {                                                                                        \
    IF_IVERBOSE(10, verbose_stream() << "(smt." << MSG << ")\n";);     \
    TRACE(LABEL, tout << "before:\n"; display(tout););                                                                  \
    FUNCTOR_DEF;                                                                                                        \
    expr_ref_vector  new_exprs(m);                                                                              \
    proof_ref_vector new_prs(m);                                                                                \
    unsigned i  = m_asserted_qhead;                                                                                     \
    unsigned sz = m_asserted_formulas.size();                                                                           \
    for (; i < sz; i++) {                                                                                               \
        expr * n    = m_asserted_formulas.get(i);                                                                       \
        proof * pr  = m_asserted_formula_prs.get(i, 0);                                                                 \
        expr_ref new_n(m);                                                                                      \
        functor(n, new_n);                                                                                              \
        TRACE("simplifier_simple_step", tout << mk_pp(n, m) << "\n" << mk_pp(new_n, m) << "\n";);       \
        if (n == new_n.get()) {                                                                                         \
            push_assertion(n, pr, new_exprs, new_prs);                                                                  \
        }                                                                                                               \
        else if (m.proofs_enabled()) {                                                                          \
            proof_ref new_pr(m);                                                                                \
            new_pr = m.mk_rewrite_star(n, new_n, 0, 0);            \
            new_pr = m.mk_modus_ponens(pr, new_pr);                                                             \
            push_assertion(new_n, new_pr, new_exprs, new_prs);                                                          \
        }                                                                                                               \
        else {                                                                                                          \
            push_assertion(new_n, 0, new_exprs, new_prs);                                                               \
        }                                                                                                               \
    }                                                                                                                   \
    swap_asserted_formulas(new_exprs, new_prs);                                                                         \
    TRACE(LABEL, display(tout););                                                                                       \
    reduce_and_solve();                                                                                                 \
    TRACE(LABEL, display(tout););                                                                                       \
}

MK_SIMPLE_SIMPLIFIER(apply_distribute_forall, distribute_forall functor(m), "distribute_forall", "distribute-forall");

void asserted_formulas::reduce_and_solve() {
    IF_IVERBOSE(10, verbose_stream() << "(smt.reducing)\n";);
    flush_cache(); // collect garbage
    reduce_asserted_formulas();
}

void asserted_formulas::infer_patterns() {
    IF_IVERBOSE(10, verbose_stream() << "(smt.pattern-inference)\n";);
    TRACE("before_pattern_inference", display(tout););
    pattern_inference infer(m, m_params);
    expr_ref_vector  new_exprs(m);
    proof_ref_vector new_prs(m);
    unsigned i  = m_asserted_qhead;
    unsigned sz = m_asserted_formulas.size();
    for (; i < sz; i++) {
        expr * n    = m_asserted_formulas.get(i);
        proof * pr  = m_asserted_formula_prs.get(i, 0);
        expr_ref  new_n(m);
        proof_ref new_pr(m);
        infer(n, new_n, new_pr);
        if (n == new_n.get()) {
            push_assertion(n, pr, new_exprs, new_prs);
        }
        else if (m.proofs_enabled()) {
            new_pr = m.mk_modus_ponens(pr, new_pr);
            push_assertion(new_n, new_pr, new_exprs, new_prs);
        }
        else {
            push_assertion(new_n, 0, new_exprs, new_prs);
        }
    }
    swap_asserted_formulas(new_exprs, new_prs);
    TRACE("after_pattern_inference", display(tout););
}

void asserted_formulas::commit() {
    commit(m_asserted_formulas.size());
}

void asserted_formulas::commit(unsigned new_qhead) {
    m_macro_manager.mark_forbidden(new_qhead - m_asserted_qhead, m_asserted_formulas.c_ptr() + m_asserted_qhead);
    m_asserted_qhead = new_qhead;
}

void asserted_formulas::eliminate_term_ite() {
    IF_IVERBOSE(10, verbose_stream() << "(smt.eliminating-ite-term)\n";);
    TRACE("before_elim_term_ite", display(tout););
    elim_term_ite   elim(m, m_defined_names);
    expr_ref_vector  new_exprs(m);
    proof_ref_vector new_prs(m);
    unsigned i  = m_asserted_qhead;
    unsigned sz = m_asserted_formulas.size();
    for (; i < sz; i++) {
        expr * n    = m_asserted_formulas.get(i);
        proof * pr  = m_asserted_formula_prs.get(i, 0);
        expr_ref  new_n(m);
        proof_ref new_pr(m);
        elim(n, new_exprs, new_prs, new_n, new_pr);
        SASSERT(new_n.get() != 0);
        DEBUG_CODE({
                for (unsigned i = 0; i < new_exprs.size(); i++) {
                    SASSERT(new_exprs.get(i) != 0);
                }
            });
        if (n == new_n.get()) {
            push_assertion(n, pr, new_exprs, new_prs);
        }
        else if (m.proofs_enabled()) {
            new_pr = m.mk_modus_ponens(pr, new_pr);
            push_assertion(new_n, new_pr, new_exprs, new_prs);
        }
        else {
            push_assertion(new_n, 0, new_exprs, new_prs);
        }
    }
    swap_asserted_formulas(new_exprs, new_prs);
    TRACE("after_elim_term_ite", display(tout););
    reduce_and_solve();
    TRACE("after_elim_term_ite", display(tout););
}

void asserted_formulas::propagate_values() {
    IF_IVERBOSE(10, verbose_stream() << "(smt.constant-propagation)\n";);
    TRACE("propagate_values", tout << "before:\n"; display(tout););
    flush_cache();
    bool found = false;
    // Separate the formulas in two sets: C and R
    // C is a set which contains formulas of the form
    // { x = n }, where x is a variable and n a numeral.
    // R contains the rest.
    // 
    // - new_exprs1 is the set C
    // - new_exprs2 is the set R
    //
    // The loop also updates the m_cache. It adds the entries x -> n to it.
    expr_ref_vector  new_exprs1(m);
    proof_ref_vector new_prs1(m);
    expr_ref_vector  new_exprs2(m);
    proof_ref_vector new_prs2(m);
    unsigned sz = m_asserted_formulas.size();
    for (unsigned i = 0; i < sz; i++) {
        expr_ref   n(m_asserted_formulas.get(i), m);
        proof_ref pr(m_asserted_formula_prs.get(i, 0), m);
        TRACE("simplifier", tout << mk_pp(n, m) << "\n";);
        expr* lhs, *rhs;
        if (m.is_eq(n, lhs, rhs) &&
            (m.is_value(lhs) || m.is_value(rhs))) {
            if (m.is_value(lhs)) {
                std::swap(lhs, rhs);
                n  = m.mk_eq(lhs, rhs);
                pr = m.mk_symmetry(pr);
            }
            if (!m.is_value(lhs) && !m_simplifier.is_cached(lhs)) {
                if (i >= m_asserted_qhead) {
                    new_exprs1.push_back(n);
                    if (m.proofs_enabled())
                        new_prs1.push_back(pr);
                }
                TRACE("propagate_values", tout << "found:\n" << mk_pp(lhs, m) << "\n->\n" << mk_pp(rhs, m) << "\n";
                      if (pr) tout << "proof: " << mk_pp(pr, m) << "\n";);
                m_simplifier.cache_result(lhs, rhs, pr);
                found = true;
                continue;
            }
        }
        if (i >= m_asserted_qhead) {
            new_exprs2.push_back(n);
            if (m.proofs_enabled())
                new_prs2.push_back(pr);
        }
    }
    TRACE("propagate_values", tout << "found: " << found << "\n";);
    // If C is not empty, then reduce R using the updated simplifier cache with entries
    // x -> n for each constraint 'x = n' in C.
    if (found) {
        unsigned sz = new_exprs2.size();
        for (unsigned i = 0; i < sz; i++) {
            expr * n    = new_exprs2.get(i);
            proof * pr  = new_prs2.get(i, 0);
            expr_ref new_n(m);
            proof_ref new_pr(m);
            m_simplifier(n, new_n, new_pr);
            if (n == new_n.get()) {
                push_assertion(n, pr, new_exprs1, new_prs1);
            }
            else {
                new_pr = m.mk_modus_ponens(pr, new_pr);
                push_assertion(new_n, new_pr, new_exprs1, new_prs1);
            }
        }
        swap_asserted_formulas(new_exprs1, new_prs1);
        // IMPORTANT: the cache MUST be flushed. This guarantees that all entries
        // x->n will be removed from m_cache. If we don't do that, the next transformation
        // may simplify constraints in C using these entries, and the variables x in C
        // will be (silently) eliminated, and models produced by Z3 will not contain them.
        flush_cache(); 
    }
    TRACE("propagate_values", tout << "after:\n"; display(tout););
}

void asserted_formulas::propagate_booleans() {
    bool cont     = true;
    bool modified = false;
    flush_cache();
    while (cont) {
        TRACE("propagate_booleans", tout << "before:\n"; display(tout););
        IF_IVERBOSE(10, verbose_stream() << "(smt.propagate-booleans)\n";);
        cont        = false;
        unsigned i  = m_asserted_qhead;
        unsigned sz = m_asserted_formulas.size();
#define PROCESS() {                                                     \
            expr * n    = m_asserted_formulas.get(i);                   \
            proof * pr  = m_asserted_formula_prs.get(i, 0);             \
            expr_ref new_n(m);                                          \
            proof_ref new_pr(m);                                        \
            m_simplifier(n, new_n, new_pr);                             \
            m_asserted_formulas.set(i, new_n);                          \
            if (m.proofs_enabled()) {                                   \
                new_pr = m.mk_modus_ponens(pr, new_pr);                 \
                m_asserted_formula_prs.set(i, new_pr);                  \
            }                                                           \
            if (n != new_n) {                                           \
                cont = true;                                            \
                modified = true;                                        \
            }                                                           \
            if (m.is_not(new_n))                                        \
                m_simplifier.cache_result(to_app(new_n)->get_arg(0), m.mk_false(), m.mk_iff_false(new_pr)); \
            else                                                        \
                m_simplifier.cache_result(new_n, m.mk_true(), m.mk_iff_true(new_pr)); \
        }
        for (; i < sz; i++) {
            PROCESS();
        }
        flush_cache();
        TRACE("propagate_booleans", tout << "middle:\n"; display(tout););
        i = sz;
        while (i > m_asserted_qhead) {
            --i;
            PROCESS();
        }
        flush_cache();
        TRACE("propagate_booleans", tout << "after:\n"; display(tout););
    }
    if (modified)
        reduce_asserted_formulas();
}

#define MK_SIMPLIFIER(NAME, FUNCTOR, TAG, MSG, REDUCE)                  \
    bool asserted_formulas::NAME() {                                    \
        IF_IVERBOSE(10, verbose_stream() << "(smt." << MSG << ")\n";);  \
        TRACE(TAG, ast_mark visited; display_ll(tout, visited););       \
        FUNCTOR;                                                        \
        bool changed = false;                                           \
        expr_ref_vector  new_exprs(m);                                  \
        proof_ref_vector new_prs(m);                                    \
        unsigned i  = m_asserted_qhead;                                 \
        unsigned sz = m_asserted_formulas.size();                       \
        for (; i < sz; i++) {                                           \
            expr * n    = m_asserted_formulas.get(i);                   \
            proof * pr  = m_asserted_formula_prs.get(i, 0);             \
            expr_ref  new_n(m);                                         \
            proof_ref new_pr(m);                                        \
            functor(n, new_n, new_pr);                                  \
            if (n == new_n.get()) {                                     \
                push_assertion(n, pr, new_exprs, new_prs);              \
            }                                                           \
            else if (m.proofs_enabled()) {                              \
                changed = true;                                         \
                if (!new_pr) new_pr = m.mk_rewrite(n, new_n);           \
                new_pr = m.mk_modus_ponens(pr, new_pr);                 \
                push_assertion(new_n, new_pr, new_exprs, new_prs);      \
            }                                                           \
            else {                                                      \
                changed = true;                                         \
                push_assertion(new_n, 0, new_exprs, new_prs);           \
            }                                                           \
        }                                                               \
        swap_asserted_formulas(new_exprs, new_prs);                     \
        TRACE(TAG, ast_mark visited; display_ll(tout, visited););       \
        if (changed && REDUCE) {                                        \
            reduce_and_solve();                                         \
            TRACE(TAG, ast_mark visited; display_ll(tout, visited););   \
        }                                                               \
        return changed;                                                 \
    }

MK_SIMPLIFIER(pull_cheap_ite_trees, pull_cheap_ite_tree_star functor(m, m_simplifier), "pull_cheap_ite_trees", "pull-cheap-ite-trees", false);

MK_SIMPLIFIER(pull_nested_quantifiers, pull_nested_quant functor(m), "pull_nested_quantifiers", "pull-nested-quantifiers", false);

proof * asserted_formulas::get_inconsistency_proof() const {
    if (!inconsistent())
        return 0;
    if (!m.proofs_enabled())
        return 0;
    unsigned sz = m_asserted_formulas.size();
    for (unsigned i = 0; i < sz; i++) {
        expr * f = m_asserted_formulas.get(i);
        if (m.is_false(f))
            return m_asserted_formula_prs.get(i);
    }
    UNREACHABLE();
    return 0;
}

void asserted_formulas::refine_inj_axiom() {
    IF_IVERBOSE(10, verbose_stream() << "(smt.refine-injectivity)\n";);
    TRACE("inj_axiom", display(tout););
    unsigned i  = m_asserted_qhead;
    unsigned sz = m_asserted_formulas.size();
    for (; i < sz; i++) {
        expr * n    = m_asserted_formulas.get(i);
        proof * pr  = m_asserted_formula_prs.get(i, 0);
        expr_ref new_n(m);
        if (is_quantifier(n) && simplify_inj_axiom(m, to_quantifier(n), new_n)) {
            TRACE("inj_axiom", tout << "simplifying...\n" << mk_pp(n, m) << "\n" << mk_pp(new_n, m) << "\n";);
            m_asserted_formulas.set(i, new_n);
            if (m.proofs_enabled()) {
                proof_ref new_pr(m);     
                new_pr = m.mk_rewrite(n, new_n);
                new_pr = m.mk_modus_ponens(pr, new_pr);
                m_asserted_formula_prs.set(i, new_pr);
            }
        }
    }
    TRACE("inj_axiom", display(tout););
}

MK_SIMPLIFIER(apply_bit2int, bit2int& functor = m_bit2int, "bit2int", "propagate-bit-vector-over-integers", true);

MK_SIMPLIFIER(cheap_quant_fourier_motzkin, elim_bounds_star functor(m), "elim_bounds", "cheap-fourier-motzkin", true);



MK_SIMPLIFIER(elim_bvs_from_quantifiers, bv_elim_star functor(m), "bv_elim", "eliminate-bit-vectors-from-quantifiers", true);

#define LIFT_ITE(NAME, FUNCTOR, MSG)                                    \
    void asserted_formulas::NAME() {                                    \
        IF_IVERBOSE(10, verbose_stream() << "(smt." << MSG << ")\n";);  \
        TRACE("lift_ite", display(tout););                              \
        FUNCTOR;                                                        \
        unsigned i  = m_asserted_qhead;                                 \
        unsigned sz = m_asserted_formulas.size();                       \
        for (; i < sz; i++) {                                           \
            expr * n    = m_asserted_formulas.get(i);                   \
            proof * pr  = m_asserted_formula_prs.get(i, 0);             \
            expr_ref  new_n(m);                                         \
            proof_ref new_pr(m);                                        \
            functor(n, new_n, new_pr);                                  \
            TRACE("lift_ite_step", tout << mk_pp(n, m) << "\n";);       \
            IF_IVERBOSE(10000, verbose_stream() << "lift before: " << get_num_exprs(n)  << ", after: " << get_num_exprs(new_n) << "\n";); \
            m_asserted_formulas.set(i, new_n);                          \
            if (m.proofs_enabled()) {                                   \
                new_pr = m.mk_modus_ponens(pr, new_pr);                 \
                m_asserted_formula_prs.set(i, new_pr);                  \
            }                                                           \
        }                                                               \
        TRACE("lift_ite", display(tout););                              \
        reduce_and_solve();                                             \
    }

LIFT_ITE(lift_ite, push_app_ite functor(m_simplifier, m_params.m_lift_ite == LI_CONSERVATIVE), "lifting ite");
LIFT_ITE(ng_lift_ite, ng_push_app_ite functor(m_simplifier, m_params.m_ng_lift_ite == LI_CONSERVATIVE), "lifting ng ite");

unsigned asserted_formulas::get_total_size() const {
    expr_mark visited;
    unsigned r  = 0;
    unsigned sz = m_asserted_formulas.size();
    for (unsigned i = 0; i < sz; i++)
        r += get_num_exprs(m_asserted_formulas.get(i), visited);
    return r;
}

void asserted_formulas::max_bv_sharing() {
    IF_IVERBOSE(10, verbose_stream() << "(smt.maximizing-bv-sharing)\n";);
    TRACE("bv_sharing", display(tout););
    unsigned i  = m_asserted_qhead;
    unsigned sz = m_asserted_formulas.size();
    for (; i < sz; i++) {
        expr * n    = m_asserted_formulas.get(i);
        proof * pr  = m_asserted_formula_prs.get(i, 0);
        expr_ref new_n(m);
        proof_ref new_pr(m);
        m_bv_sharing(n, new_n, new_pr);
        m_asserted_formulas.set(i, new_n);
        if (m.proofs_enabled()) {
            new_pr = m.mk_modus_ponens(pr, new_pr);
            m_asserted_formula_prs.set(i, new_pr);
        }
    }
    reduce_asserted_formulas();
    TRACE("bv_sharing", display(tout););
    
}

#ifdef Z3DEBUG
void pp(asserted_formulas & f) {
    f.display(std::cout);
}
#endif
