/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "slicot_dple.hpp"
#include "slicot_layer.hpp"
#include "slicot_la.hpp"

#include "../../core/std_vector_tools.hpp"
#include "../../core/function/mx_function.hpp"
#include "../../core/function/sx_function.hpp"

#include <cassert>
#include <ctime>
#include <numeric>

using namespace std;
namespace casadi {

  extern "C"
  int CASADI_DPLE_SLICOT_EXPORT
  casadi_register_dple_slicot(Dple::Plugin* plugin) {
    plugin->creator = SlicotDple::creator;
    plugin->name = "slicot";
    plugin->doc = SlicotDple::meta_doc.c_str();
    plugin->version = 31;
    return 0;
  }

  extern "C"
  void CASADI_DPLE_SLICOT_EXPORT casadi_load_dple_slicot() {
    Dple::registerPlugin(casadi_register_dple_slicot);
  }

  Options SlicotDple::options_
  = {{&FunctionInternal::options_},
     {{"linear_solver",
       {OT_STRING,
        "User-defined linear solver class. Needed for sensitivities."}},
      {"linear_solver_options",
        {OT_DICT,
         "Options to be passed to the linear solver."}},
      {"psd_num_zero",
        {OT_DOUBLE,
          "Numerical zero used in Periodic Schur decomposition with slicot."
          "This option is needed when your systems has Floquet multipliers"
          "zero or close to zero"}}
     }
  };


  SlicotDple::SlicotDple(const std::string& name, const SpDict & st) : Dple(name, st) {

  }

  SlicotDple::~SlicotDple() {
    clear_memory();
  }

  void SlicotDple::init(const Dict& opts) {

    Dple::init(opts);

    linear_solver_ = "csparse";
    psd_num_zero_ = 1e-12;

    // Read user options
    for (auto&& op : opts) {
      if (op.first=="linear_solver") {
        linear_solver_ = op.second.as_string();
      } else if (op.first=="linear_solver_options") {
        linear_solver_options_ = op.second;
      } else if (op.first=="psd_num_zero") {
        psd_num_zero_ = op.second;
      }
    }

    casadi_assert_message(!pos_def_,
                          "pos_def option set to True: Solver only handles the indefinite case.");
    casadi_assert_message(const_dim_,
                          "const_dim option set to False: Solver only handles the True case.");

    //for (int k=0;k<K_;k++) {
    //  casadi_assert_message(A_[k].isdense(), "Solver requires arguments to be dense.");
    //  casadi_assert_message(V_[k].isdense(), "Solver requires arguments to be dense.");
    //}


    n_ = V_.colind()[1];

    alloc_w(n_*n_*K_, true); // VZ_
    alloc_w(n_*n_*K_, true); // T_
    alloc_w(n_*n_*K_, true); // Z_
    alloc_w(n_*n_*K_, true); // X_

    alloc_w(n_*n_*K_, true); // Xbar_

    alloc_w(n_*n_*K_, true); // nnKa_
    alloc_w(n_*n_*K_, true); // nnKb_

    alloc_w(n_, true); // eig_real_
    alloc_w(n_, true); // eig_imag_

    alloc_w(2*2*n_*K_, true); // F_
    alloc_w(2*2*K_, true); // FF_

    // There can be at most n partitions
    alloc_iw(n_+1, true); // partition_

    alloc_w(std::max(n_+K_-2, 4*n_)+(n_-1)*K_+2*n_); // dwork_
    alloc_w(n_*K_);
    alloc_iw(n_*K_);
    alloc_w(4*K_*4+4*K_, true); // A_
    alloc_w(4*K_, true); // B_
  }


  void SlicotDple::set_work(void* mem, const double**& arg, double**& res,
                                int*& iw, double*& w) const {
    auto m = static_cast<SlicotDpleMemory*>(mem);

    // Set work in base classes
    Dple::set_work(mem, arg, res, iw, w);

    // Lagrange multipliers of the NLP
    m->VZ = w; w += n_*n_*K_;
    m->T = w; w += n_*n_*K_;
    m->Z = w; w += n_*n_*K_;
    m->X = w; w += n_*n_*K_;

    m->Xbar = w; w += n_*n_*K_;
    m->nnKa = w; w += n_*n_*K_;
    m->nnKb = w; w += n_*n_*K_;

    m->eig_real = w; w += n_;
    m->eig_imag = w; w += n_;

    m->F = w; w += 2*2*n_*K_;
    m->FF = w; w += 2*2*K_;

    m->A = w; w += 4*K_*4+4*K_;
    m->B = w; w += 4*K_;
    m->dwork = w;
    m->wruntime = w;
    m->partition = iw; iw+= n_+1;
    m->iwruntime = iw;
  }


  /** \brief Initalize memory block */
  void SlicotDple::init_memory(void* mem) const {
    Dple::init_memory(mem);
    auto m = static_cast<SlicotDpleMemory*>(mem);

    // Construct linear solvers for low-order Discrete Periodic Sylvester Equations
    // IX00
    // 0IX0
    // 00IX
    // X00I
    //  Special case K=1
    // I+X
    // Solver complexity:  K
    m->dpse_solvers.resize(3);
    for (int i=0;i<3;++i) {
      int np = std::pow(2, i);

      Sparsity sp = Sparsity::dense(np, np);
      if (K_>1)
        sp = kron(Sparsity::band(K_, -1)+Sparsity::band(K_, K_-1), sp) + Sparsity::diag(np*K_);

      m->dpse_solvers[i].reserve(n_*(n_+1)/2);
      for (int k=0;k<n_*(n_+1)/2;++k) {
        m->dpse_solvers[i].push_back(Linsol("solver", linear_solver_));
        m->dpse_solvers[i][k].reset(sp);
      }
    }
  }

  /// \cond INTERNAL
  inline int SlicotDple::partindex(const SlicotDpleMemory* m,
      int i, int j, int k, int r, int c) const {
    return k*n_*n_+(m->partition[i]+r)*n_ + m->partition[j]+c;
  }
  /// \endcond

  void SlicotDple::eval(void* mem, const double** arg, double** res, int* iw, double* w) const {
    auto m = static_cast<SlicotDpleMemory*>(mem);

    setup(mem, arg, res, iw, w);

    // Transpose operation (after #554)
    casadi_trans(arg[DPLE_A], sparsity_in(DPLE_A), m->X, sparsity_in(DPLE_A), m->iwruntime);

    slicot_periodic_schur(n_, K_, m->X, m->T, m->Z,
      m->dwork, m->eig_real, m->eig_imag, psd_num_zero_);

    if (error_unstable_) {
      for (int i=0;i<n_;++i) {
        double modulus = sqrt(m->eig_real[i]*m->eig_real[i]+m->eig_imag[i]*m->eig_imag[i]);
        casadi_assert_message(modulus+eps_unstable_ <= 1,
          "SlicotDple: system is unstable."
          "Found an eigenvalue " << m->eig_real[i] << " + " <<
          m->eig_imag[i] << "j, with modulus " << modulus <<
          " (corresponding eps= " << 1-modulus << ")." <<
          std::endl << "Use options and 'error_unstable'"
          "and 'eps_unstable' to influence this message.");
      }
    }

    // Find a block partition of the T hessenberg form
    int* p = m->partition;
    p[0] = 0;
    int p_i = 1;
    int i = 0, j = 0;
    while (j<n_) {
      while (i<n_ && m->T[i+n_*j]!=0) i+=1;
      j = i;
      p[p_i++] = i;
      i += 1;
    }

    // Main loops to loop over blocks of the block-upper triangular A
    // Outer main loop
    for (int l=0;l<p_i-1;++l) {

      // Inner main loop
      for (int r=0;r<l+1;++r) {

        int n1 = p[r+1]-p[r];
        int n2 = p[l+1]-p[l];
        int np = n1*n2;

        casadi_assert(n1-1+n2-1>=0);

        Linsol & solver = m->dpse_solvers[n1-1+n2-1][((l+1)*l)/2+r];

        // ********** START ***************
        double * A = m->A;
        std::fill(A, A+4*K_*4+4*K_, 1);
        double * T = m->T;

        if (K_==1) { // Special case if K==1
          dense_kron_stride(np, n2, T+p[r]*n_ + p[r], T+p[l]*n_ + p[l], A, n_, n_, np);
          for (int ll=0;ll<np;++ll)
            A[ll*np+ll]+= 1;
        } else { // Other cases
          for (int k=0;k<K_-1;++k) {
            dense_kron_stride(np, n2,
              T+p[r]*n_ + p[r], T+p[l]*n_ + p[l], A+np*(np+1)*((k+1)%K_), n_, n_, np+1);
            T+= n_*n_;
          }

          dense_kron_stride(np, n2, T+p[r]*n_ + p[r], T+p[l]*n_ + p[l], A+1, n_, n_, np+1);
        }
        // ********** STOP ***************
        // Solve Discrete Periodic Sylvester Equation Solver

        solver.pivoting(m->A);
        solver.factorize(m->A);

      }
    }

    for (int d=0;d<nrhs_;++d) {

      // V = blocks([mul([sZ[k].T, V[k], sZ[k]]) for k in range(p)])
      for (int k=0;k<K_;++k) { // K
        double * nnKa = m->nnKa+k*n_*n_, * nnKb = m->nnKb+k*n_*n_;
        // n^2 K

        std::fill(nnKa, nnKa+n_*n_, 0);
        // nnKa[k] <- V[k]*Z[k+1]
        // n^3 K
        dense_mul_nt(n_, n_, n_, arg[DPLE_V]+d*n_*n_*K_ + k*n_*n_, m->Z+((k+1) % K_)*n_*n_, nnKa);
        std::fill(nnKb, nnKb+n_*n_, 0);
        // nnKb[k] <- Z[k+1]'*V[k]*Z[k+1]
        dense_mul_nn(n_, n_, n_, m->Z + ((k+1) % K_)*n_*n_, nnKa, nnKb);
      }

      std::fill(m->X, m->X+n_*n_*K_, 0);

      // Main loops to loop over blocks of the block-upper triangular A
      // Outer main loop
      for (int l=0;l<p_i-1;++l) { // n
        int n2 = p[l+1]-p[l];

        // F serves an an accumulator for intermediate summation results
        // n^2 K
        std::fill(m->F, m->F+2*2*n_*K_, 0);

        //for i in range(l):
        //  F[i] = [sum(mul(X[i][j][k], A[l][j][k].T) for j in range(l)) for k in range(p) ]
        for (int k=0;k<K_;++k) {
          double *X = m->X+k*n_*n_, *T = m->T+ k*n_*n_;
          for (int i=0;i<l;++i) // n^2
            for (int j=0;j<l;++j) // n^3
              dense_mul_nt_stride(p[i+1]-p[i], n2, p[j+1]-p[j],
                X+ p[i]*n_+ p[j], T+p[l]*n_+ p[j], m->F + k*4*n_+4*i, n_, n_, 2);
        }

        // Inner main loop
        for (int r=0;r<l+1;++r) { // n^2
          int n1 = p[r+1]-p[r];
          int np = n1*n2;

          // F[r] = [sum(mul(X[r][j][k], A[l][j][k].T) for j in range(l)) for k in range(p) ]
          if (r==l) {
            for (int k=0;k<K_;++k) { // n^3 K
              double *X = m->X+k*n_*n_, *T = m->T+ k*n_*n_;
              for (int j=0;j<l;++j) // n^3
                dense_mul_nt_stride(n1, n2, p[j+1]-p[j],
                  X+ p[r]*n_+ p[j], T+p[l]*n_+ p[j], m->F + k*4*n_+4*r, n_, n_, 2);
            }
          }

          // FF =   [sum(mul(A[r][i][k], X[i][l][k]) for i in range(r)) for k in range(p)]
          // Each entry of FF is na1-by-na2
          // n^2 K
          std::fill(m->FF, m->FF+2*2*K_, 0);
          for (int k=0;k<K_;++k) { // n^3 K
            double *X = m->X+k*n_*n_, *T = m->T+ k*n_*n_;
            for (int i=0;i<r;++i) // n^3
              dense_mul_nn_stride(n1, n2, p[i+1]-p[i],
                T+p[r]*n_ + p[i], X+p[i]*n_ + p[l], m->FF+k*4, n_, n_, 2);
          }

          Linsol & solver = m->dpse_solvers[n1-1+n2-1][((l+1)*l)/2+r];

          // M <- V
          for (int k=0;k<K_;++k)
            dense_copy_stride(n1, n2, m->nnKb+ k*n_*n_+ p[r]*n_ + p[l], m->B+np*((k+1)%K_), n_, n2);

          // M+= [sum(mul(A[r][i][k], F[i][k])  for i in range(r+1)) for k in rang(p)]
          for (int k=0;k<K_;++k) { // n^3 K
            double *B = m->B + np*((k+1)%K_), *T = m->T+ k*n_*n_;
            for (int i=0;i<r+1;++i) // n^3
              dense_mul_nn_stride(n1, n2, p[i+1]-p[i],
                T+p[r]*n_+ p[i], m->F+k*4*n_+4*i, B, n_, 2, n2);
          }

          // M+= [mul(FF[k], A[l][l][k].T) for k in rang(p)]
          for (int k=0;k<K_;++k) // n^2 K
            dense_mul_nt_stride(n1, n2, n2,
              m->FF+k*4, m->T + k*n_*n_+p[l]*n_+ p[l], m->B+np*((k+1)%K_),  2, n_, n2);

          // Critical observation: Prepare step is not needed
          // n^2 K
          solver.solve(m->B, 1, true);

          // Extract solution and store it in X
          double * sol = m->B;

          for (int k=0;k<K_;++k) {
            double *X = m->X+ k*n_*n_, *S = sol+ n1*n2*k;
            dense_copy_stride(p[r+1]-p[r],   p[l+1]-p[l], S, X+ p[r]*n_ + p[l],  n2, n_);
            dense_copy_t_stride(p[r+1]-p[r], p[l+1]-p[l], S, X+ p[l]*n_ + p[r],  n2, n_);
          }

        }

        // n^3 K
        std::fill(res[DPLE_P]+d*n_*n_*K_, res[DPLE_P]+(d+1)*n_*n_*K_, 0);
      }


      for (int k=0;k<K_;++k) {
        std::fill(m->nnKa+k*n_*n_, m->nnKa+(k+1)*n_*n_, 0);
        // nnKa[k] <- V[k]*Z[k]'
        // n^3 K
        dense_mul_nn(n_, n_, n_, m->X + k*n_*n_, m->Z+ k*n_*n_, m->nnKa+ k*n_*n_);
        // output <- Z[k]*V[k]*Z[k]'
        dense_mul_tn(n_, n_, n_, m->Z + k*n_*n_, m->nnKa+ k*n_*n_,
                     res[DPLE_P]+d*n_*n_*K_+ k*n_*n_);
      }

    }

  }


  void slicot_periodic_schur(int n, int K, const double* a,
                             double* t,  double * z,
                             double* dwork, double* eig_real,
                             double *eig_imag, double num_zero) {
    int mem_base = std::max(n+K-2, 4*n);
    int mem_needed = mem_base+(n-1)*K;

    // a is immutable, we need a mutable pointer, so we use available buffer
    std::copy(a, a+n*n*K, z);

    int ret;

    ret = slicot_mb03vd(n, K, 1, n, z, n, n, dwork+mem_base, n-1, dwork);
    casadi_assert_message(ret==0, "mb03vd return code "<< ret);
    std::copy(z, z+n*n*K, t);

    ret = slicot_mb03vy(n, K, 1, n, z, n, n, dwork+mem_base, n-1, dwork, mem_needed);
    casadi_assert_message(ret==0, "mb03vy return code "<< ret);
    // Set numerical zeros to zero
    if (num_zero>0) {
      for (int k = 0;k<n*n*K;++k) {
        double &r = t[k];
        if (fabs(r)<num_zero) r = 0.0;
      }
    }

    ret = slicot_mb03wd('S', 'V', n, K, 1, n, 1, n, t, n, n, z, n, n,
                  eig_real, eig_imag, dwork, mem_needed);
    casadi_assert_message(ret==0, "mb03wd return code "<< ret);
  }

} // namespace casadi
