/*++
Copyright (c) 2015 Microsoft Corporation

Module Name:

    qe_arith.cpp

Abstract:

    Simple projection function for real arithmetic based on Loos-W.

Author:

    Nikolaj Bjorner (nbjorner) 2013-09-12

Revision History:

    Moved projection functionality to model_based_opt module. 2016-06-26

--*/

#include "qe_arith.h"
#include "qe_mbp.h"
#include "ast_util.h"
#include "arith_decl_plugin.h"
#include "ast_pp.h"
#include "model_v2_pp.h"
#include "th_rewriter.h"
#include "expr_functors.h"
#include "expr_safe_replace.h"
#include "model_based_opt.h"
#include "model_evaluator.h"

namespace qe {
    
    struct arith_project_plugin::imp {

        ast_manager&      m;
        arith_util        a;

        void insert_mul(expr* x, rational const& v, obj_map<expr, rational>& ts) {
            TRACE("qe", tout << "Adding variable " << mk_pp(x, m) << " " << v << "\n";);
            rational w;
            if (ts.find(x, w)) {
                ts.insert(x, w + v);
            }
            else {
                ts.insert(x, v); 
            }
        }

        //
        // extract linear inequalities from literal 'lit' into the model-based optimization manager 'mbo'.
        // It uses the current model to choose values for conditionals and it primes mbo with the current
        // interpretation of sub-expressions that are treated as variables for mbo.
        // 
        bool linearize(opt::model_based_opt& mbo, model_evaluator& eval, expr* lit, expr_ref_vector& fmls, obj_map<expr, unsigned>& tids) {
            obj_map<expr, rational> ts;
            rational c(0), mul(1);
            expr_ref t(m);
            opt::ineq_type ty = opt::t_le;
            expr* e1, *e2;
            DEBUG_CODE(expr_ref val(m); 
                       eval(lit, val); 
                       CTRACE("qe", !m.is_true(val), tout << mk_pp(lit, m) << " := " << val << "\n";);
                       SASSERT(m.is_true(val)););

            bool is_not = m.is_not(lit, lit);
            if (is_not) {
                mul.neg();
            }
            SASSERT(!m.is_not(lit));
            if ((a.is_le(lit, e1, e2) || a.is_ge(lit, e2, e1))) {
                linearize(mbo, eval, mul, e1, c, fmls, ts, tids);
                linearize(mbo, eval, -mul, e2, c, fmls, ts, tids);
                ty = is_not ? opt::t_lt : opt::t_le;
            }
            else if ((a.is_lt(lit, e1, e2) || a.is_gt(lit, e2, e1))) {
                linearize(mbo, eval,  mul, e1, c, fmls, ts, tids);
                linearize(mbo, eval, -mul, e2, c, fmls, ts, tids);
                ty = is_not ? opt::t_le: opt::t_lt;
            }
            else if (m.is_eq(lit, e1, e2) && !is_not && is_arith(e1)) {
                linearize(mbo, eval,  mul, e1, c, fmls, ts, tids);
                linearize(mbo, eval, -mul, e2, c, fmls, ts, tids);
                ty = opt::t_eq;
            }  
            else if (m.is_eq(lit, e1, e2) && is_not && is_arith(e1)) {
                
                rational r1, r2;
                expr_ref val1 = eval(e1); 
                expr_ref val2 = eval(e2);
                VERIFY(a.is_numeral(val1, r1));
                VERIFY(a.is_numeral(val2, r2));
                SASSERT(r1 != r2);
                if (r1 < r2) {
                    std::swap(e1, e2);
                }                
                ty = opt::t_lt;
                linearize(mbo, eval,  mul, e1, c, fmls, ts, tids);
                linearize(mbo, eval, -mul, e2, c, fmls, ts, tids);                
            }                        
            else if (m.is_distinct(lit) && !is_not && is_arith(to_app(lit)->get_arg(0))) {
                expr_ref val(m);
                rational r;
                app* alit = to_app(lit);
                vector<std::pair<expr*,rational> > nums;
                for (unsigned i = 0; i < alit->get_num_args(); ++i) {
                    val = eval(alit->get_arg(i));
                    VERIFY(a.is_numeral(val, r));
                    nums.push_back(std::make_pair(alit->get_arg(i), r));
                }
                std::sort(nums.begin(), nums.end(), compare_second());
                for (unsigned i = 0; i + 1 < nums.size(); ++i) {
                    SASSERT(nums[i].second < nums[i+1].second);
                    expr_ref fml(a.mk_lt(nums[i].first, nums[i+1].first), m);
                    if (!linearize(mbo, eval, fml, fmls, tids)) {
                        return false;
                    }
                }
                return true;
            }
            else if (m.is_distinct(lit) && is_not && is_arith(to_app(lit)->get_arg(0))) {
                // find the two arguments that are equal.
                // linearize these.                
                map<rational, expr*, rational::hash_proc, rational::eq_proc> values;
                bool found_eq = false;
                for (unsigned i = 0; !found_eq && i < to_app(lit)->get_num_args(); ++i) {
                    expr* arg1 = to_app(lit)->get_arg(i), *arg2 = 0;
                    rational r;
                    expr_ref val = eval(arg1);
                    VERIFY(a.is_numeral(val, r));
                    if (values.find(r, arg2)) {
                        ty = opt::t_eq;
                        linearize(mbo, eval,  mul, arg1, c, fmls, ts, tids);
                        linearize(mbo, eval, -mul, arg2, c, fmls, ts, tids);
                        found_eq = true;
                    }
                    else {
                        values.insert(r, arg1);
                    }
                }
                SASSERT(found_eq);
            }
            else {
                TRACE("qe", tout << "Skipping " << mk_pp(lit, m) << "\n";);
                return false;
            }
            vars coeffs;
            extract_coefficients(mbo, eval, ts, tids, coeffs);
            mbo.add_constraint(coeffs, c, ty);
            return true;
        }

        //
        // convert linear arithmetic term into an inequality for mbo.
        // 
        void linearize(opt::model_based_opt& mbo, model_evaluator& eval, rational const& mul, expr* t, rational& c, 
                       expr_ref_vector& fmls, obj_map<expr, rational>& ts, obj_map<expr, unsigned>& tids) {
            expr* t1, *t2, *t3;
            rational mul1;
            expr_ref val(m);
            if (a.is_mul(t, t1, t2) && is_numeral(t1, mul1)) {
                linearize(mbo, eval, mul* mul1, t2, c, fmls, ts, tids);
            }
            else if (a.is_mul(t, t1, t2) && is_numeral(t2, mul1)) {
                linearize(mbo, eval, mul* mul1, t1, c, fmls, ts, tids);
            }
            else if (a.is_add(t)) {
                app* ap = to_app(t);
                for (unsigned i = 0; i < ap->get_num_args(); ++i) {
                    linearize(mbo, eval, mul, ap->get_arg(i), c, fmls, ts, tids);
                }
            }
            else if (a.is_sub(t, t1, t2)) {
                linearize(mbo, eval,  mul, t1, c, fmls, ts, tids);
                linearize(mbo, eval, -mul, t2, c, fmls, ts, tids);
            }
            else if (a.is_uminus(t, t1)) {
                linearize(mbo, eval, -mul, t1, c, fmls, ts, tids);
            }
            else if (a.is_numeral(t, mul1)) {
                c += mul*mul1;
            }
            else if (m.is_ite(t, t1, t2, t3)) {
                val = eval(t1);
                SASSERT(m.is_true(val) || m.is_false(val));
                TRACE("qe", tout << mk_pp(t1, m) << " := " << val << "\n";);
                if (m.is_true(val)) {
                    linearize(mbo, eval, mul, t2, c, fmls, ts, tids);
                    fmls.push_back(t1);
                }
                else {
                    expr_ref not_t1(mk_not(m, t1), m);
                    fmls.push_back(not_t1);
                    linearize(mbo, eval, mul, t3, c, fmls, ts, tids);
                }
            }
            else if (a.is_mod(t, t1, t2) && is_numeral(t2, mul1)) {
                rational r;
                val = eval(t);
                VERIFY(a.is_numeral(val, r));
                c += mul*r;
                // t1 mod mul1 == r               
                rational c0(-r), mul0(1);
                obj_map<expr, rational> ts0;
                linearize(mbo, eval, mul0, t1, c0, fmls, ts0, tids);
                vars coeffs;
                extract_coefficients(mbo, eval, ts0, tids, coeffs);
                mbo.add_divides(coeffs, c0, mul1);
            }
            else {
                insert_mul(t, mul, ts);
            }
        }

        bool is_numeral(expr* t, rational& r) {
            expr* t1, *t2;
            rational r1, r2;
            if (a.is_numeral(t, r)) {
                // no-op
            }
            else if (a.is_uminus(t, t1) && is_numeral(t1, r)) {
                r.neg();
            }         
            else if (a.is_mul(t)) {
                app* ap = to_app(t);
                r = rational(1);
                for (unsigned i = 0; i < ap->get_num_args(); ++i) {
                    if (!is_numeral(ap->get_arg(i), r1)) return false;
                    r *= r1;
                }
            }
            else if (a.is_add(t)) {
                app* ap = to_app(t);
                r = rational(0);
                for (unsigned i = 0; i < ap->get_num_args(); ++i) {
                    if (!is_numeral(ap->get_arg(i), r1)) return false;
                    r += r1;
                }
            }
            else if (a.is_sub(t, t1, t2) && is_numeral(t1, r1) && is_numeral(t2, r2)) {
                r = r1 - r2;
            }
            else {
                return false;
            }
            return true;
        }

        struct compare_second {
            bool operator()(std::pair<expr*, rational> const& a, 
                            std::pair<expr*, rational> const& b) const {
                return a.second < b.second;
            }
        };

        bool is_arith(expr* e) {
            return a.is_int(e) || a.is_real(e);
        }

        rational n_sign(rational const& b) {
            return rational(b.is_pos()?-1:1);
        }

        imp(ast_manager& m): 
            m(m), a(m) {}

        ~imp() {}

        bool solve(model& model, app_ref_vector& vars, expr_ref_vector& lits) {
            return false;
        }

        bool operator()(model& model, app* v, app_ref_vector& vars, expr_ref_vector& lits) {
            app_ref_vector vs(m);
            vs.push_back(v);
            (*this)(model, vs, lits);
            return vs.empty();
        }

        typedef opt::model_based_opt::var var;
        typedef opt::model_based_opt::row row;
        typedef vector<var> vars;

        void operator()(model& model, app_ref_vector& vars, expr_ref_vector& fmls) {
            bool has_arith = false;
            for (unsigned i = 0; !has_arith && i < vars.size(); ++i) {
                expr* v = vars[i].get();
                has_arith |= is_arith(v);
            }
            if (!has_arith) {
                return;
            }
            model_evaluator eval(model);
            // eval.set_model_completion(true);

            opt::model_based_opt mbo;
            obj_map<expr, unsigned> tids;
            unsigned j = 0;
            for (unsigned i = 0; i < fmls.size(); ++i) {
                expr* fml = fmls[i].get();
                if (!linearize(mbo, eval, fml, fmls, tids)) {
                    if (i != j) {
                        fmls[j] = fmls[i].get();
                    }
                    ++j;
                }
                else {
                    TRACE("qe", tout << mk_pp(fml, m) << "\n";);
                }
            }
            fmls.resize(j);

            // fmls holds residue,
            // mbo holds linear inequalities that are in scope
            // collect variables in residue an in tids.
            // filter variables that are absent from residue.
            // project those.
            // collect result of projection
            // return those to fmls.

            expr_mark var_mark, fmls_mark;
            for (unsigned i = 0; i < vars.size(); ++i) {
                app* v = vars[i].get();
                var_mark.mark(v);
                if (is_arith(v) && !tids.contains(v)) {
                    rational r;
                    expr_ref val = eval(v);
                    a.is_numeral(val, r);
                    TRACE("qe", tout << mk_pp(v, m) << " " << val << "\n";);
                    tids.insert(v, mbo.add_var(r, a.is_int(v)));
                }
            }
            for (unsigned i = 0; i < fmls.size(); ++i) {
                fmls_mark.mark(fmls[i].get());
            }
            obj_map<expr, unsigned>::iterator it = tids.begin(), end = tids.end();
            ptr_vector<expr> index2expr;
            for (; it != end; ++it) {
                expr* e = it->m_key;
                if (!var_mark.is_marked(e)) {
                    mark_rec(fmls_mark, e);
                }
                index2expr.setx(it->m_value, e, 0);
            }
            j = 0;
            unsigned_vector real_vars;
            for (unsigned i = 0; i < vars.size(); ++i) {
                app* v = vars[i].get();
                if (is_arith(v) && !fmls_mark.is_marked(v)) {
                    real_vars.push_back(tids.find(v));
                }
                else {
                    if (i != j) {
                        vars[j] = v;
                    }
                    ++j;
                }
            }
            vars.resize(j);
            TRACE("qe", tout << "remaining vars: " << vars << "\n"; 
                  for (unsigned i = 0; i < real_vars.size(); ++i) {
                      unsigned v = real_vars[i];
                      tout << "v" << v << " " << mk_pp(index2expr[v], m) << "\n";
                  }
                  mbo.display(tout););
            mbo.project(real_vars.size(), real_vars.c_ptr());
            TRACE("qe", mbo.display(tout););
            vector<row> rows;
            mbo.get_live_rows(rows);
            
            for (unsigned i = 0; i < rows.size(); ++i) {
                expr_ref_vector ts(m);
                expr_ref t(m), s(m), val(m);
                row const& r = rows[i];
                if (r.m_vars.size() == 0) {
                    continue;
                }
                if (r.m_vars.size() == 1 && r.m_vars[0].m_coeff.is_neg() && r.m_type != opt::t_mod) {
                    var const& v = r.m_vars[0];
                    t = index2expr[v.m_id];
                    if (!v.m_coeff.is_minus_one()) {
                        t = a.mk_mul(a.mk_numeral(-v.m_coeff, a.is_int(t)), t);
                    }
                    s = a.mk_numeral(r.m_coeff, a.is_int(t));
                    switch (r.m_type) {
                    case opt::t_lt: t = a.mk_gt(t, s); break;
                    case opt::t_le: t = a.mk_ge(t, s); break;
                    case opt::t_eq: t = a.mk_eq(t, s); break;
                    default: UNREACHABLE();
                    }
                    fmls.push_back(t);
                    val = eval(t);
                    CTRACE("qe", !m.is_true(val), tout << "Evaluated unit " << t << " to " << val << "\n";);
                    continue;
                }
                for (j = 0; j < r.m_vars.size(); ++j) {
                    var const& v = r.m_vars[j];
                    t = index2expr[v.m_id];
                    if (!v.m_coeff.is_one()) {
                        t = a.mk_mul(a.mk_numeral(v.m_coeff, a.is_int(t)), t);
                    }
                    ts.push_back(t);
                }
                s = a.mk_numeral(-r.m_coeff, a.is_int(t));
                if (ts.size() == 1) {
                    t = ts[0].get();
                }
                else {
                    t = a.mk_add(ts.size(), ts.c_ptr());
                }
                switch (r.m_type) {
                case opt::t_lt: t = a.mk_lt(t, s); break;
                case opt::t_le: t = a.mk_le(t, s); break;
                case opt::t_eq: t = a.mk_eq(t, s); break;
                case opt::t_mod: {
                    if (!r.m_coeff.is_zero()) {
                        t = a.mk_sub(t, s);
                    }
                    t = a.mk_eq(a.mk_mod(t, a.mk_numeral(r.m_mod, true)), a.mk_int(0));
                    break;
                }
                }
                fmls.push_back(t);
                                
                val = eval(t);
                CTRACE("qe", !m.is_true(val), tout << "Evaluated " << t << " to " << val << "\n";);

            }
        }        

        opt::inf_eps maximize(expr_ref_vector const& fmls0, model& mdl, app* t, expr_ref& ge, expr_ref& gt) {
            SASSERT(a.is_real(t));
            expr_ref_vector fmls(fmls0);
            opt::model_based_opt mbo;
            opt::inf_eps value;
            obj_map<expr, rational> ts;
            obj_map<expr, unsigned> tids;
            model_evaluator eval(mdl);
            // extract objective function.
            vars coeffs;
            rational c(0), mul(1);
            linearize(mbo, eval, mul, t, c, fmls, ts, tids);
            extract_coefficients(mbo, eval, ts, tids, coeffs);
            mbo.set_objective(coeffs, c);

            SASSERT(validate_model(eval, fmls0));

            // extract linear constraints
            
            for (unsigned i = 0; i < fmls.size(); ++i) {
                linearize(mbo, eval, fmls[i].get(), fmls, tids);
            }
            
            // find optimal value
            value = mbo.maximize();


            // update model to use new values that satisfy optimality
            ptr_vector<expr> vars;
            obj_map<expr, unsigned>::iterator it = tids.begin(), end = tids.end();
            for (; it != end; ++it) {
                expr* e = it->m_key;
                if (is_uninterp_const(e)) {
                    unsigned id = it->m_value;
                    func_decl* f = to_app(e)->get_decl();
                    expr_ref val(a.mk_numeral(mbo.get_value(id), false), m);
                    mdl.register_decl(f, val);
                }
                else {
                    TRACE("qe", tout << "omitting model update for non-uninterpreted constant " << mk_pp(e, m) << "\n";);
                }
            }
            expr_ref val(a.mk_numeral(value.get_rational(), false), m);
            expr_ref tval = eval(t);

            // update the predicate 'bound' which forces larger values when 'strict' is true.
            // strict:  bound := valuue < t
            // !strict: bound := value <= t
            if (!value.is_finite()) {
                ge = a.mk_ge(t, tval);
                gt = m.mk_false();
            }
            else if (value.get_infinitesimal().is_neg()) {
                ge = a.mk_ge(t, tval);
                gt = a.mk_ge(t, val);
            }
            else {
                ge = a.mk_ge(t, val);
                gt = a.mk_gt(t, val);
            }
            SASSERT(validate_model(eval, fmls0));
            return value;
        }

        bool validate_model(model_evaluator& eval, expr_ref_vector const& fmls) {
            bool valid = true;
            for (unsigned i = 0; i < fmls.size(); ++i) {
                expr_ref val = eval(fmls[i]);
                if (!m.is_true(val)) {
                    valid = false;
                    TRACE("qe", tout << mk_pp(fmls[i], m) << " := " << val << "\n";);
                }
            }
            return valid;
        }

        void extract_coefficients(opt::model_based_opt& mbo, model_evaluator& eval, obj_map<expr, rational> const& ts, obj_map<expr, unsigned>& tids, vars& coeffs) {
            coeffs.reset();
            eval.set_model_completion(true);
            obj_map<expr, rational>::iterator it = ts.begin(), end = ts.end();
            for (; it != end; ++it) {
                unsigned id;
                expr* v = it->m_key;
                if (!tids.find(v, id)) {
                    rational r;
                    expr_ref val = eval(v);
                    a.is_numeral(val, r);
                    id = mbo.add_var(r, a.is_int(v));
                    tids.insert(v, id);
                }
                CTRACE("qe", it->m_value.is_zero(), tout << mk_pp(v, m) << " has coefficeint 0\n";);
                if (!it->m_value.is_zero()) {
                    coeffs.push_back(var(id, it->m_value));                
                }
            }
        }

    };

    arith_project_plugin::arith_project_plugin(ast_manager& m) {
        m_imp = alloc(imp, m);
    }

    arith_project_plugin::~arith_project_plugin() {
        dealloc(m_imp);
    }

    bool arith_project_plugin::operator()(model& model, app* var, app_ref_vector& vars, expr_ref_vector& lits) {
        return (*m_imp)(model, var, vars, lits);
    }

    void arith_project_plugin::operator()(model& model, app_ref_vector& vars, expr_ref_vector& lits) {
        (*m_imp)(model, vars, lits);
    }

    bool arith_project_plugin::solve(model& model, app_ref_vector& vars, expr_ref_vector& lits) {
        return m_imp->solve(model, vars, lits);
    }

    family_id arith_project_plugin::get_family_id() {
        return m_imp->a.get_family_id();
    }

    opt::inf_eps arith_project_plugin::maximize(expr_ref_vector const& fmls, model& mdl, app* t, expr_ref& ge, expr_ref& gt) {
        return m_imp->maximize(fmls, mdl, t, ge, gt);
    }

    bool arith_project(model& model, app* var, expr_ref_vector& lits) {
        ast_manager& m = lits.get_manager();
        arith_project_plugin ap(m);
        app_ref_vector vars(m);
        return ap(model, var, vars, lits);
    }
}
