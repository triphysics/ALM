/*
 fitting.cpp

 Copyright (c) 2014-2018 Terumasa Tadano

 This file is distributed under the terms of the MIT license.
 Please see the file 'LICENCE.txt' in the root directory 
 or http://opensource.org/licenses/mit-license.php for information.
*/

#include "fitting.h"
#include "constants.h"
#include "constraint.h"
#include "error.h"
#include "fcs.h"
#include "files.h"
#include "interaction.h"
#include "mathfunctions.h"
#include "memory.h"
#include "symmetry.h"
#include "system.h"
#include "timer.h"
#include <iostream>
#include <cmath>
#include <string>
#include <vector>
#include <boost/lexical_cast.hpp>

using namespace ALM_NS;


Fitting::Fitting()
{
    set_default_variables();
}

Fitting::~Fitting()
{
    deallocate_variables();
}

void Fitting::set_default_variables()
{
    params = nullptr;
    u_in = nullptr;
    f_in = nullptr;
    ndata = 0;
    nstart = 1;
    nend = 0;
    ndata_used = 0;
}

void Fitting::deallocate_variables()
{
    if (params) {
        deallocate(params);
    }
    if (u_in) {
        deallocate(u_in);
    }
    if (f_in) {
        deallocate(f_in);
    }
}

void Fitting::fitmain(ALM *alm)
{
    alm->timer->start_clock("fitting");

    int i;
    const int nat = alm->system->supercell.number_of_atoms;
    const int natmin = alm->symmetry->nat_prim;
    const int maxorder = alm->interaction->maxorder;
    const int nconsts = alm->constraint->number_of_constraints;
    const int ndata_used = nend - nstart + 1;
    const int ntran = alm->symmetry->ntran;

    std::cout << " FITTING" << std::endl;
    std::cout << " =======" << std::endl << std::endl;

    std::cout << "  Reference files" << std::endl;
    std::cout << "   Displacement: " << alm->files->file_disp << std::endl;
    std::cout << "   Force       : " << alm->files->file_force << std::endl;
    std::cout << std::endl;

    std::cout << "  NSTART = " << nstart << "; NEND = " << nend << std::endl;
    std::cout << "  " << ndata_used << " entries will be used for fitting."
        << std::endl << std::endl;


    int N = 0;
    for (i = 0; i < maxorder; ++i) {
        N += alm->fcs->nequiv[i].size();
    }
    int M = 3 * natmin * ndata_used * ntran;

    std::cout << "  Total Number of Parameters : " << N
        << std::endl << std::endl;

    std::vector<double> amat;
    std::vector<double> bvec;
    std::vector<std::vector<double>> u_vec, f_vec;
    std::vector<double> param_tmp(N);

    // Copy displacement and force data sets from u_in & f_in.
    u_vec.resize(ndata_used, std::vector<double>(3 * nat));
    f_vec.resize(ndata_used, std::vector<double>(3 * nat));

    for (i = 0; i < ndata_used; ++i) {
        for (int j = 0; j < 3 * nat; ++j) {
            u_vec[i][j] = u_in[i][j];
            f_vec[i][j] = f_in[i][j];
        }
    }

    if (alm->constraint->constraint_algebraic) {
        int N_new = 0;
        for (i = 0; i < maxorder; ++i) {
            N_new += alm->constraint->index_bimap[i].size();
        }
        std::cout << "  Total Number of Free Parameters : "
            << N_new << std::endl << std::endl;

        // Calculate matrix elements for fitting

        double fnorm;

        get_matrix_elements_algebraic_constraint(maxorder,
                                                 ndata_used,
                                                 u_vec,
                                                 f_vec,
                                                 amat,
                                                 bvec,
                                                 fnorm,
                                                 alm->symmetry,
                                                 alm->fcs,
                                                 alm->constraint);

        // Perform fitting with SVD

        assert(!amat.empty());
        assert(!bvec.empty());

        fit_algebraic_constraints(N_new, M,
                                  &amat[0], &bvec[0],
                                  param_tmp,
                                  fnorm, maxorder,
                                  alm->fcs,
                                  alm->constraint);
    } else {

        // Calculate matrix elements for fitting


        get_matrix_elements(maxorder,
                            ndata_used,
                            u_vec,
                            f_vec,
                            amat,
                            bvec,
                            alm->symmetry,
                            alm->fcs);


        // Perform fitting with SVD or QRD

        assert(!amat.empty());
        assert(!bvec.empty());

        if (alm->constraint->exist_constraint) {
            fit_with_constraints(N, M, nconsts,
                                 &amat[0], &bvec[0],
                                 &param_tmp[0],
                                 alm->constraint->const_mat,
                                 alm->constraint->const_rhs);
        } else {
            fit_without_constraints(N, M,
                                    &amat[0], &bvec[0],
                                    &param_tmp[0]);
        }
    }


    // Copy force constants to public variable "params"
    if (params) {
        deallocate(params);
    }
    allocate(params, N);
    for (i = 0; i < N; ++i) params[i] = param_tmp[i];

    std::cout << std::endl;
    alm->timer->print_elapsed();
    std::cout << " -------------------------------------------------------------------" << std::endl;
    std::cout << std::endl;

    alm->timer->stop_clock("fitting");
}

void Fitting::set_displacement_and_force(const double * const *disp_in,
                                         const double * const *force_in,
                                         const int nat,
                                         const int ndata_used_in)
{
    ndata_used = ndata_used_in;

    if (u_in) {
        deallocate(u_in);
    }
    allocate(u_in, ndata_used, 3 * nat);

    if (f_in) {
        deallocate(f_in);
    }
    allocate(f_in, ndata_used, 3 * nat);

    for (int i = 0; i < ndata_used; i++) {
        for (int j = 0; j < 3 * nat; j++) {
            u_in[i][j] = disp_in[i][j];
            f_in[i][j] = force_in[i][j];
        }
    }
}

const int Fitting::get_ndata_used()
{
    return ndata_used;
}

void Fitting::fit_without_constraints(int N,
                                      int M,
                                      double *amat,
                                      double *bvec,
                                      double *param_out)
{
    int i, j;
    int nrhs = 1, nrank, INFO;
    auto rcond = -1.0;
    auto f_square = 0.0;
    double *WORK, *S, *fsum2;

    std::cout << "  Entering fitting routine: SVD without constraints" << std::endl;

    auto LMIN = std::min<int>(M, N);
    auto LMAX = std::max<int>(M, N);

    int LWORK = 3 * LMIN + std::max<int>(2 * LMIN, LMAX);
    LWORK = 2 * LWORK;

    allocate(WORK, LWORK);
    allocate(S, LMIN);
    allocate(fsum2, LMAX);

    unsigned long k = 0;

    for (i = 0; i < M; ++i) {
        fsum2[i] = bvec[i];
        f_square += std::pow(bvec[i], 2);
    }
    for (i = M; i < LMAX; ++i) fsum2[i] = 0.0;

    std::cout << "  SVD has started ... ";

    // Fitting with singular value decomposition
    dgelss_(&M, &N, &nrhs, amat, &M, fsum2, &LMAX,
            S, &rcond, &nrank, WORK, &LWORK, &INFO);

    std::cout << "finished !" << std::endl << std::endl;

    std::cout << "  RANK of the matrix = " << nrank << std::endl;
    if (nrank < N)
        warn("fit_without_constraints",
             "Matrix is rank-deficient. Force constants could not be determined uniquely :(");

    if (nrank == N) {
        auto f_residual = 0.0;
        for (i = N; i < M; ++i) {
            f_residual += std::pow(fsum2[i], 2);
        }
        std::cout << std::endl << "  Residual sum of squares for the solution: "
            << sqrt(f_residual) << std::endl;
        std::cout << "  Fitting error (%) : "
            << sqrt(f_residual / f_square) * 100.0 << std::endl;
    }

    for (i = 0; i < N; ++i) {
        param_out[i] = fsum2[i];
    }

    deallocate(WORK);
    deallocate(S);
    deallocate(fsum2);
}

void Fitting::fit_with_constraints(int N,
                                   int M,
                                   int P,
                                   double *amat,
                                   double *bvec,
                                   double *param_out,
                                   double **cmat,
                                   double *dvec)
{
    int i, j;
    double *fsum2;
    double *mat_tmp;

    std::cout << "  Entering fitting routine: QRD with constraints" << std::endl;

    allocate(fsum2, M);
    allocate(mat_tmp, (M + P) * N);

    unsigned long k = 0;
    unsigned long l = 0;

    // Concatenate two matrices as 1D array
    for (j = 0; j < N; ++j) {
        for (i = 0; i < M; ++i) {
            mat_tmp[k++] = amat[l++];
        }
        for (i = 0; i < P; ++i) {
            mat_tmp[k++] = cmat[i][j];
        }
    }

    const auto nrank = rankQRD((M + P), N, mat_tmp, eps12);
    deallocate(mat_tmp);

    if (nrank != N) {
        std::cout << std::endl;
        std::cout << " **************************************************************************" << std::endl;
        std::cout << "  WARNING : rank deficient.                                                " << std::endl;
        std::cout << "  rank ( (A) ) ! = N            A: Fitting matrix     B: Constraint matrix " << std::endl;
        std::cout << "       ( (B) )                  N: The number of parameters                " << std::endl;
        std::cout << "  rank = " << nrank << " N = " << N << std::endl << std::endl;
        std::cout << "  This can cause a difficulty in solving the fitting problem properly      " << std::endl;
        std::cout << "  with DGGLSE, especially when the difference is large. Please check if    " << std::endl;
        std::cout << "  you obtain reliable force constants in the .fcs file.                    " << std::endl << std::
            endl;
        std::cout << "  You may need to reduce the cutoff radii and/or increase NDATA            " << std::endl;
        std::cout << "  by giving linearly-independent displacement patterns.                    " << std::endl;
        std::cout << " **************************************************************************" << std::endl;
        std::cout << std::endl;
    }

    auto f_square = 0.0;
    for (i = 0; i < M; ++i) {
        fsum2[i] = bvec[i];
        f_square += std::pow(bvec[i], 2);
    }
    std::cout << "  QR-Decomposition has started ...";

    double *cmat_mod;
    allocate(cmat_mod, P * N);

    k = 0;
    for (j = 0; j < N; ++j) {
        for (i = 0; i < P; ++i) {
            cmat_mod[k++] = cmat[i][j];
        }
    }

    // Fitting

    int LWORK = P + std::min<int>(M, N) + 10 * std::max<int>(M, N);
    int INFO;
    double *WORK, *x;
    allocate(WORK, LWORK);
    allocate(x, N);

    dgglse_(&M, &N, &P, amat, &M, cmat_mod, &P,
            fsum2, dvec, x, WORK, &LWORK, &INFO);

    std::cout << " finished. " << std::endl;

    double f_residual = 0.0;
    for (i = N - P; i < M; ++i) {
        f_residual += std::pow(fsum2[i], 2);
    }
    std::cout << std::endl << "  Residual sum of squares for the solution: "
        << sqrt(f_residual) << std::endl;
    std::cout << "  Fitting error (%) : "
        << std::sqrt(f_residual / f_square) * 100.0 << std::endl;

    // copy fcs to bvec
    for (i = 0; i < N; ++i) {
        param_out[i] = x[i];
    }

    deallocate(cmat_mod);
    deallocate(WORK);
    deallocate(x);
    deallocate(fsum2);
}

void Fitting::fit_algebraic_constraints(int N,
                                        int M,
                                        double *amat,
                                        double *bvec,
                                        std::vector<double> &param_out,
                                        const double fnorm,
                                        const int maxorder,
                                        Fcs *fcs,
                                        Constraint *constraint)
{
    int i, j;
    unsigned long k;
    int nrhs = 1, nrank, INFO, LWORK;
    int LMIN, LMAX;
    double rcond = -1.0;
    double *WORK, *S, *fsum2;

    std::cout << "  Entering fitting routine: SVD with constraints considered algebraically." << std::endl;

    LMIN = std::min<int>(M, N);
    LMAX = std::max<int>(M, N);

    LWORK = 3 * LMIN + std::max<int>(2 * LMIN, LMAX);
    LWORK = 2 * LWORK;

    allocate(WORK, LWORK);
    allocate(S, LMIN);
    allocate(fsum2, LMAX);

    for (i = 0; i < M; ++i) {
        fsum2[i] = bvec[i];
    }
    for (i = M; i < LMAX; ++i) fsum2[i] = 0.0;

    std::cout << "  SVD has started ... ";

    // Fitting with singular value decomposition
    dgelss_(&M, &N, &nrhs, amat, &M, fsum2, &LMAX,
            S, &rcond, &nrank, WORK, &LWORK, &INFO);

    deallocate(WORK);
    deallocate(S);

    std::cout << "finished !" << std::endl << std::endl;
    std::cout << "  RANK of the matrix = " << nrank << std::endl;
    if (nrank < N) {
        warn("fit_without_constraints",
             "Matrix is rank-deficient. Force constants could not be determined uniquely :(");
    }

    if (nrank == N) {
        double f_residual = 0.0;
        for (i = N; i < M; ++i) {
            f_residual += std::pow(fsum2[i], 2);
        }
        std::cout << std::endl;
        std::cout << "  Residual sum of squares for the solution: "
            << sqrt(f_residual) << std::endl;
        std::cout << "  Fitting error (%) : "
            << sqrt(f_residual / (fnorm * fnorm)) * 100.0 << std::endl;
    }

    std::vector<double> param_irred(N, 0.0);
    for (i = 0; i < LMIN; ++i) param_irred[i] = fsum2[i];
    deallocate(fsum2);

    // Recover reducible set of force constants

    recover_original_forceconstants(maxorder,
                                    param_irred,
                                    param_out,
                                    fcs->nequiv,
                                    constraint);
}


void Fitting::calc_matrix_elements(const int M,
                                   const int N,
                                   const int natmin,
                                   const int ndata_fit,
                                   const int nmulti,
                                   const int maxorder,
                                   double **u,
                                   double **f,
                                   double *amat,
                                   double *bvec,
                                   Symmetry *symmetry,
                                   Fcs *fcs)
{
    int i, j;
    int irow;

    std::cout << "  Calculation of matrix elements for direct fitting started ... ";
    for (i = 0; i < M * N; ++i) {
        amat[i] = 0.0;
    }
    for (i = 0; i < M; ++i) {
        bvec[i] = 0.0;
    }

    const int ncycle = ndata_fit * nmulti;
    const int natmin3 = 3 * natmin;

#ifdef _OPENMP
#pragma omp parallel private(irow, i, j)
#endif
    {
        int *ind;
        int mm, order, iat, k;
        int im, idata, iparam;
        double amat_tmp;
        double **amat_orig_tmp;

        allocate(ind, maxorder + 1);
        allocate(amat_orig_tmp, natmin3, N);

#ifdef _OPENMP
#pragma omp for schedule(guided)
#endif
        for (irow = 0; irow < ncycle; ++irow) {

            // generate r.h.s vector B
            for (i = 0; i < natmin; ++i) {
                iat = symmetry->map_p2s[i][0];
                for (j = 0; j < 3; ++j) {
                    im = 3 * i + j + natmin3 * irow;
                    bvec[im] = f[irow][3 * iat + j];
                }
            }

            for (i = 0; i < natmin3; ++i) {
                for (j = 0; j < N; ++j) {
                    amat_orig_tmp[i][j] = 0.0;
                }
            }

            // generate l.h.s. matrix A

            idata = natmin3 * irow;
            iparam = 0;

            for (order = 0; order < maxorder; ++order) {

                mm = 0;

                for (const auto &iter : fcs->nequiv[order]) {
                    for (i = 0; i < iter; ++i) {
                        ind[0] = fcs->fc_table[order][mm].elems[0];
                        k = inprim_index(ind[0], symmetry);
                        amat_tmp = 1.0;
                        for (j = 1; j < order + 2; ++j) {
                            ind[j] = fcs->fc_table[order][mm].elems[j];
                            amat_tmp *= u[irow][fcs->fc_table[order][mm].elems[j]];
                        }
                        amat_orig_tmp[k][iparam] -= gamma(order + 2, ind) * fcs->fc_table[order][mm].sign * amat_tmp;
                        ++mm;
                    }
                    ++iparam;
                }
            }
            for (i = 0; i < natmin3; ++i) {
                for (j = 0; j < N; ++j) {
                    // Transpose here for later use of lapack without transpose
                    amat[natmin3 * ncycle * j + i + idata] = amat_orig_tmp[i][j];
                }
            }
        }

        deallocate(ind);
        deallocate(amat_orig_tmp);
    }

    std::cout << "done!" << std::endl << std::endl;
}


void Fitting::get_matrix_elements(const int maxorder,
                                  const int ndata_fit,
                                  const std::vector<std::vector<double>> &u,
                                  const std::vector<std::vector<double>> &f,
                                  std::vector<double> &amat,
                                  std::vector<double> &bvec,
                                  Symmetry *symmetry,
                                  Fcs *fcs)
{
    int i, j;
    int irow;

    std::vector<std::vector<double>> u_multi, f_multi;

    data_multiplier(u, u_multi, ndata_fit, symmetry);
    data_multiplier(f, f_multi, ndata_fit, symmetry);

    const int natmin = symmetry->nat_prim;
    const int natmin3 = 3 * natmin;
    const int nrows = natmin3 * ndata_fit * symmetry->ntran;
    auto ncols = 0;

    for (i = 0; i < maxorder; ++i) ncols += fcs->nequiv[i].size();

    amat.resize(nrows * ncols, 0.0);
    bvec.resize(nrows, 0.0);

    const int ncycle = ndata_fit * symmetry->ntran;

#ifdef _OPENMP
#pragma omp parallel private(irow, i, j)
#endif
    {
        int *ind;
        int mm, order, iat, k;
        int im, idata, iparam;
        double amat_tmp;
        double **amat_orig_tmp;

        allocate(ind, maxorder + 1);
        allocate(amat_orig_tmp, natmin3, ncols);

#ifdef _OPENMP
#pragma omp for schedule(guided)
#endif
        for (irow = 0; irow < ncycle; ++irow) {

            // generate r.h.s vector B
            for (i = 0; i < natmin; ++i) {
                iat = symmetry->map_p2s[i][0];
                for (j = 0; j < 3; ++j) {
                    im = 3 * i + j + natmin3 * irow;
                    bvec[im] = f_multi[irow][3 * iat + j];
                }
            }

            for (i = 0; i < natmin3; ++i) {
                for (j = 0; j < ncols; ++j) {
                    amat_orig_tmp[i][j] = 0.0;
                }
            }

            // generate l.h.s. matrix A

            idata = natmin3 * irow;
            iparam = 0;

            for (order = 0; order < maxorder; ++order) {

                mm = 0;

                for (const auto &iter : fcs->nequiv[order]) {
                    for (i = 0; i < iter; ++i) {
                        ind[0] = fcs->fc_table[order][mm].elems[0];
                        k = inprim_index(ind[0], symmetry);
                        amat_tmp = 1.0;
                        for (j = 1; j < order + 2; ++j) {
                            ind[j] = fcs->fc_table[order][mm].elems[j];
                            amat_tmp *= u_multi[irow][fcs->fc_table[order][mm].elems[j]];
                        }
                        amat_orig_tmp[k][iparam] -= gamma(order + 2, ind) * fcs->fc_table[order][mm].sign * amat_tmp;
                        ++mm;
                    }
                    ++iparam;
                }
            }

            for (i = 0; i < natmin3; ++i) {
                for (j = 0; j < ncols; ++j) {
                    // Transpose here for later use of lapack without transpose
                    amat[natmin3 * ncycle * j + i + idata] = amat_orig_tmp[i][j];
                }
            }
        }

        deallocate(ind);
        deallocate(amat_orig_tmp);
    }

    u_multi.clear();
    f_multi.clear();
}

void Fitting::get_matrix_elements(const int maxorder,
                                  const int ndata_fit,
                                  const int nat,
                                  double *amat,
                                  double *bvec,
                                  Symmetry *symmetry,
                                  Fcs *fcs)
{
    // amat: flattened array of a MxN real matrix (signal matrix). Row major
    // bvec: real 1D array of atomic forces
    //   M : 3 * natmin * ndata_fit * symmetry->ntran
    //   N : number of irreducible force constants

    int i, j;
    int irow;

    const int natmin = symmetry->nat_prim;
    const int nrows = 3 * natmin * ndata_fit * symmetry->ntran;
    auto ncols = 0;

    for (i = 0; i < maxorder; ++i) ncols += fcs->nequiv[i].size();

    const int ncycle = ndata_fit * symmetry->ntran;

    double **u_multi, **f_multi;

    allocate(u_multi, ncycle, 3 * nat);
    allocate(f_multi, ncycle, 3 * nat);
    data_multiplier(u_multi, f_multi, nat, ndata_fit, symmetry);

    for (i = 0; i < nrows * ncols; ++i) amat[i] = 0.0;
    for (i = 0; i < nrows; ++i) bvec[i] = 0.0;

#ifdef _OPENMP
#pragma omp parallel private(irow, i, j)
#endif
    {
        int *ind;
        int mm, order, iat, k;
        int im, idata, iparam;
        double amat_tmp;

        allocate(ind, maxorder + 1);

#ifdef _OPENMP
#pragma omp for schedule(guided)
#endif
        for (irow = 0; irow < ncycle; ++irow) {

            // generate r.h.s vector B
            for (i = 0; i < natmin; ++i) {
                iat = symmetry->map_p2s[i][0];
                for (j = 0; j < 3; ++j) {
                    im = 3 * i + j + 3 * natmin * irow;
                    bvec[im] = f_multi[irow][3 * iat + j];
                }
            }

            // generate l.h.s. matrix A

            idata = 3 * natmin * irow;
            iparam = 0;

            for (order = 0; order < maxorder; ++order) {

                mm = 0;

                for (const auto &iter : fcs->nequiv[order]) {
                    for (i = 0; i < iter; ++i) {
                        ind[0] = fcs->fc_table[order][mm].elems[0];
                        k = idata + inprim_index(fcs->fc_table[order][mm].elems[0], symmetry);
                        amat_tmp = 1.0;
                        for (j = 1; j < order + 2; ++j) {
                            ind[j] = fcs->fc_table[order][mm].elems[j];
                            amat_tmp *= u_multi[irow][fcs->fc_table[order][mm].elems[j]];
                        }
                        amat[k * ncols + iparam] -= gamma(order + 2, ind) * fcs->fc_table[order][mm].sign * amat_tmp;
                        ++mm;
                    }
                    ++iparam;
                }
            }
        }

        deallocate(ind);
    }

    deallocate(u_multi);
    deallocate(f_multi);
}


void Fitting::calc_matrix_elements_algebraic_constraint(const int M,
                                                        const int N,
                                                        const int N_new,
                                                        const int nat,
                                                        const int natmin,
                                                        const int ndata_fit,
                                                        const int nmulti,
                                                        const int maxorder,
                                                        double **u,
                                                        double **f,
                                                        double *amat,
                                                        double *bvec,
                                                        double *bvec_orig,
                                                        Symmetry *symmetry,
                                                        Fcs *fcs,
                                                        Constraint *constraint)
{
    int i, j;
    int irow;

    std::cout << "  Calculation of matrix elements for direct fitting started ... ";

    const int ncycle = ndata_fit * nmulti;
    const auto natmin3 = 3 * natmin;

#ifdef _OPENMP
#pragma omp parallel for private(j)
#endif
    for (i = 0; i < M * N_new; ++i) {
        amat[i] = 0.0;
    }
    for (i = 0; i < M; ++i) {
        bvec[i] = 0.0;
        bvec_orig[i] = 0.0;
    }

#ifdef _OPENMP
#pragma omp parallel private(irow, i, j)
#endif
    {
        int *ind;
        int mm, order, iat, k;
        int im, idata, iparam;
        int ishift;
        int iold, inew;
        double amat_tmp;
        double **amat_orig;
        double **amat_mod;

        allocate(ind, maxorder + 1);
        allocate(amat_orig, natmin3, N);
        allocate(amat_mod, natmin3, N_new);

#ifdef _OPENMP
#pragma omp for schedule(guided)
#endif
        for (irow = 0; irow < ncycle; ++irow) {

            // generate r.h.s vector B
            for (i = 0; i < natmin; ++i) {
                iat = symmetry->map_p2s[i][0];
                for (j = 0; j < 3; ++j) {
                    im = 3 * i + j + natmin3 * irow;
                    bvec[im] = f[irow][3 * iat + j];
                    bvec_orig[im] = f[irow][3 * iat + j];
                }
            }

            for (i = 0; i < natmin3; ++i) {
                for (j = 0; j < N; ++j) {
                    amat_orig[i][j] = 0.0;
                }
                for (j = 0; j < N_new; ++j) {
                    amat_mod[i][j] = 0.0;
                }
            }

            // generate l.h.s. matrix A

            idata = natmin3 * irow;
            iparam = 0;

            for (order = 0; order < maxorder; ++order) {

                mm = 0;

                for (const auto &iter : fcs->nequiv[order]) {
                    for (i = 0; i < iter; ++i) {
                        ind[0] = fcs->fc_table[order][mm].elems[0];
                        k = inprim_index(ind[0], symmetry);

                        amat_tmp = 1.0;
                        for (j = 1; j < order + 2; ++j) {
                            ind[j] = fcs->fc_table[order][mm].elems[j];
                            amat_tmp *= u[irow][fcs->fc_table[order][mm].elems[j]];
                        }
                        amat_orig[k][iparam] -= gamma(order + 2, ind) * fcs->fc_table[order][mm].sign * amat_tmp;
                        ++mm;
                    }
                    ++iparam;
                }
            }

            ishift = 0;
            iparam = 0;

            for (order = 0; order < maxorder; ++order) {

                for (i = 0; i < constraint->const_fix[order].size(); ++i) {

                    for (j = 0; j < natmin3; ++j) {
                        bvec[j + idata] -= constraint->const_fix[order][i].val_to_fix
                            * amat_orig[j][ishift + constraint->const_fix[order][i].p_index_target];
                    }
                }

                for (const auto &it : constraint->index_bimap[order]) {
                    inew = it.left + iparam;
                    iold = it.right + ishift;

                    for (j = 0; j < natmin3; ++j) {
                        amat_mod[j][inew] = amat_orig[j][iold];
                    }
                }

                for (i = 0; i < constraint->const_relate[order].size(); ++i) {

                    iold = constraint->const_relate[order][i].p_index_target + ishift;

                    for (j = 0; j < constraint->const_relate[order][i].alpha.size(); ++j) {

                        inew = constraint->index_bimap[order].right.at(
                                constraint->const_relate[order][i].p_index_orig[j])
                            + iparam;
                        for (k = 0; k < natmin3; ++k) {
                            amat_mod[k][inew] -= amat_orig[k][iold] * constraint->const_relate[order][i].alpha[j];
                        }
                    }
                }

                ishift += fcs->nequiv[order].size();
                iparam += constraint->index_bimap[order].size();
            }

            for (i = 0; i < natmin3; ++i) {
                for (j = 0; j < N_new; ++j) {
                    amat[natmin3 * ncycle * j + i + idata] = amat_mod[i][j];
                }
            }

        }

        deallocate(ind);
        deallocate(amat_orig);
        deallocate(amat_mod);
    }

    std::cout << "done!" << std::endl << std::endl;
}


void Fitting::get_matrix_elements_algebraic_constraint(const int maxorder,
                                                       const int ndata_fit,
                                                       const std::vector<std::vector<double>> &u,
                                                       const std::vector<std::vector<double>> &f,
                                                       std::vector<double> &amat,
                                                       std::vector<double> &bvec,
                                                       double &fnorm,
                                                       Symmetry *symmetry,
                                                       Fcs *fcs,
                                                       Constraint *constraint)
{
    int i, j;
    int irow;

    std::vector<std::vector<double>> u_multi, f_multi;

    data_multiplier(u, u_multi, ndata_fit, symmetry);
    data_multiplier(f, f_multi, ndata_fit, symmetry);

    const int natmin = symmetry->nat_prim;
    const int natmin3 = 3 * natmin;
    const int nrows = natmin3 * ndata_fit * symmetry->ntran;
    auto ncols = 0;
    auto ncols_new = 0;

    for (i = 0; i < maxorder; ++i) {
        ncols += fcs->nequiv[i].size();
        ncols_new += constraint->index_bimap[i].size();
    }

    const int ncycle = ndata_fit * symmetry->ntran;

    std::vector<double> bvec_orig(nrows, 0.0);

    amat.resize(nrows * ncols_new, 0.0);
    bvec.resize(nrows, 0.0);

#ifdef _OPENMP
#pragma omp parallel private(irow, i, j)
#endif
    {
        int *ind;
        int mm, order, iat, k;
        int im, idata, iparam;
        int ishift;
        int iold, inew;
        double amat_tmp;
        double **amat_orig_tmp;
        double **amat_mod_tmp;

        allocate(ind, maxorder + 1);
        allocate(amat_orig_tmp, natmin3, ncols);
        allocate(amat_mod_tmp, natmin3, ncols_new);

#ifdef _OPENMP
#pragma omp for schedule(guided)
#endif
        for (irow = 0; irow < ncycle; ++irow) {

            // generate r.h.s vector B
            for (i = 0; i < natmin; ++i) {
                iat = symmetry->map_p2s[i][0];
                for (j = 0; j < 3; ++j) {
                    im = 3 * i + j + natmin3 * irow;
                    bvec[im] = f_multi[irow][3 * iat + j];
                    bvec_orig[im] = f_multi[irow][3 * iat + j];
                }
            }

            for (i = 0; i < natmin3; ++i) {
                for (j = 0; j < ncols; ++j) {
                    amat_orig_tmp[i][j] = 0.0;
                }
                for (j = 0; j < ncols_new; ++j) {
                    amat_mod_tmp[i][j] = 0.0;
                }
            }

            // generate l.h.s. matrix A

            idata = natmin3 * irow;
            iparam = 0;

            for (order = 0; order < maxorder; ++order) {

                mm = 0;

                for (const auto &iter : fcs->nequiv[order]) {
                    for (i = 0; i < iter; ++i) {
                        ind[0] = fcs->fc_table[order][mm].elems[0];
                        k = inprim_index(ind[0], symmetry);

                        amat_tmp = 1.0;
                        for (j = 1; j < order + 2; ++j) {
                            ind[j] = fcs->fc_table[order][mm].elems[j];
                            amat_tmp *= u_multi[irow][fcs->fc_table[order][mm].elems[j]];
                        }
                        amat_orig_tmp[k][iparam] -= gamma(order + 2, ind)
                            * fcs->fc_table[order][mm].sign * amat_tmp;
                        ++mm;
                    }
                    ++iparam;
                }
            }

            // Convert the full matrix and vector into a smaller irreducible form
            // by using constraint information.

            ishift = 0;
            iparam = 0;

            for (order = 0; order < maxorder; ++order) {

                for (i = 0; i < constraint->const_fix[order].size(); ++i) {

                    for (j = 0; j < natmin3; ++j) {
                        bvec[j + idata] -= constraint->const_fix[order][i].val_to_fix
                            * amat_orig_tmp[j][ishift + constraint->const_fix[order][i].p_index_target];
                    }
                }

                for (const auto &it : constraint->index_bimap[order]) {
                    inew = it.left + iparam;
                    iold = it.right + ishift;

                    for (j = 0; j < natmin3; ++j) {
                        amat_mod_tmp[j][inew] = amat_orig_tmp[j][iold];
                    }
                }

                for (i = 0; i < constraint->const_relate[order].size(); ++i) {

                    iold = constraint->const_relate[order][i].p_index_target + ishift;

                    for (j = 0; j < constraint->const_relate[order][i].alpha.size(); ++j) {

                        inew = constraint->index_bimap[order].right.at(
                                constraint->const_relate[order][i].p_index_orig[j]) +
                            iparam;

                        for (k = 0; k < natmin3; ++k) {
                            amat_mod_tmp[k][inew] -= amat_orig_tmp[k][iold]
                                * constraint->const_relate[order][i].alpha[j];
                        }
                    }
                }

                ishift += fcs->nequiv[order].size();
                iparam += constraint->index_bimap[order].size();
            }

            for (i = 0; i < natmin3; ++i) {
                for (j = 0; j < ncols_new; ++j) {
                    // Transpose here for later use of lapack without transpose
                    // amat[i + idata][j] = amat_mod_tmp[i][j];
                    amat[natmin3 * ncycle * j + i + idata] = amat_mod_tmp[i][j];
                }
            }
        }

        deallocate(ind);
        deallocate(amat_orig_tmp);
        deallocate(amat_mod_tmp);
    }

    fnorm = 0.0;
    for (i = 0; i < bvec_orig.size(); ++i) {
        fnorm += bvec_orig[i] * bvec_orig[i];
    }
    fnorm = std::sqrt(fnorm);
}


void Fitting::recover_original_forceconstants(const int maxorder,
                                              const std::vector<double> &param_in,
                                              std::vector<double> &param_out,
                                              std::vector<int> *nequiv,
                                              Constraint *constraint)
{
    // Expand the given force constants into the larger sets 
    // by using the constraint matrix.

    int i, j, k;
    int ishift = 0;
    int iparam = 0;
    double tmp;
    int inew, iold;

    unsigned int nparams = 0;

    for (i = 0; i < maxorder; ++i) nparams += nequiv[i].size();
    if (nparams == param_in.size()) return;

    param_out.resize(nparams, 0.0);

    for (i = 0; i < maxorder; ++i) {
        for (j = 0; j < constraint->const_fix[i].size(); ++j) {
            param_out[constraint->const_fix[i][j].p_index_target + ishift]
                = constraint->const_fix[i][j].val_to_fix;
        }

        for (const auto &it : constraint->index_bimap[i]) {
            inew = it.left + iparam;
            iold = it.right + ishift;

            param_out[iold] = param_in[inew];
        }

        for (j = 0; j < constraint->const_relate[i].size(); ++j) {
            tmp = 0.0;

            for (k = 0; k < constraint->const_relate[i][j].alpha.size(); ++k) {
                tmp += constraint->const_relate[i][j].alpha[k]
                    * param_out[constraint->const_relate[i][j].p_index_orig[k] + ishift];
            }
            param_out[constraint->const_relate[i][j].p_index_target + ishift] = -tmp;
        }

        ishift += nequiv[i].size();
        iparam += constraint->index_bimap[i].size();
    }
}


void Fitting::data_multiplier(double **u,
                              double **f,
                              const int nat,
                              const int ndata_used,
                              Symmetry *symmetry)
{
    int i, j, k;
    int n_mapped;

    // Multiply data
    int idata = 0;
    for (i = 0; i < ndata_used; ++i) {
        for (int itran = 0; itran < symmetry->ntran; ++itran) {
            for (j = 0; j < nat; ++j) {
                n_mapped = symmetry->map_sym[j][symmetry->symnum_tran[itran]];
                for (k = 0; k < 3; ++k) {
                    u[idata][3 * n_mapped + k] = u_in[i][3 * j + k];
                    f[idata][3 * n_mapped + k] = f_in[i][3 * j + k];
                }
            }
            ++idata;
        }
    }
}

void Fitting::data_multiplier(const std::vector<std::vector<double>> &data_in,
                              std::vector<std::vector<double>> &data_out,
                              const int ndata_used,
                              Symmetry *symmetry)
{
    int i, j, k;
    int n_mapped;
    const int ndata_in = data_in.size();

    if (ndata_in < ndata_used) {
        exit("data_multiplier", "Number of data sets is insufficient.");
    }

    int ndata_out = ndata_used * symmetry->ntran;
    const int nat = symmetry->nat_prim * symmetry->ntran;

    auto idata = 0;
    for (i = 0; i < ndata_used; ++i) {
        std::vector<double> data_tmp(3 * nat, 0.0);

        for (int itran = 0; itran < symmetry->ntran; ++itran) {
            for (j = 0; j < nat; ++j) {
                n_mapped = symmetry->map_sym[j][symmetry->symnum_tran[itran]];
                for (k = 0; k < 3; ++k) {
                    data_tmp[3 * n_mapped + k] = data_in[i][3 * j + k];
                }
            }
            data_out.emplace_back(data_tmp);
            ++idata;
        }
    }
}

int Fitting::inprim_index(const int n,
                          Symmetry *symmetry)
{
    int in = -1;
    const auto atmn = n / 3;
    const auto crdn = n % 3;

    for (int i = 0; i < symmetry->nat_prim; ++i) {
        if (symmetry->map_p2s[i][0] == atmn) {
            in = 3 * i + crdn;
            break;
        }
    }
    return in;
}

double Fitting::gamma(const int n,
                      const int *arr)
{
    int *arr_tmp, *nsame;
    int i;

    allocate(arr_tmp, n);
    allocate(nsame, n);

    for (i = 0; i < n; ++i) {
        arr_tmp[i] = arr[i];
        nsame[i] = 0;
    }

    const auto ind_front = arr[0];
    auto nsame_to_front = 1;

    insort(n, arr_tmp);

    auto nuniq = 1;
    auto iuniq = 0;

    nsame[0] = 1;

    for (i = 1; i < n; ++i) {
        if (arr_tmp[i] == arr_tmp[i - 1]) {
            ++nsame[iuniq];
        } else {
            ++nsame[++iuniq];
            ++nuniq;
        }

        if (arr[i] == ind_front) ++nsame_to_front;
    }

    auto denom = 1;

    for (i = 0; i < nuniq; ++i) {
        denom *= factorial(nsame[i]);
    }

    deallocate(arr_tmp);
    deallocate(nsame);

    return static_cast<double>(nsame_to_front) / static_cast<double>(denom);
}

int Fitting::factorial(const int n)
{
    if (n == 1 || n == 0) {
        return 1;
    }
    return n * factorial(n - 1);
}


int Fitting::rankQRD(const int m,
                     const int n,
                     double *mat,
                     const double tolerance)
{
    // Return the rank of matrix mat revealed by the column pivoting QR decomposition
    // The matrix mat is destroyed.

    auto m_ = m;
    auto n_ = n;

    auto LDA = m_;

    auto LWORK = 10 * n_;
    int INFO;
    int *JPVT;
    double *WORK, *TAU;

    const auto nmin = std::min<int>(m_, n_);

    allocate(JPVT, n_);
    allocate(WORK, LWORK);
    allocate(TAU, nmin);

    for (int i = 0; i < n_; ++i) JPVT[i] = 0;

    dgeqp3_(&m_, &n_, mat, &LDA, JPVT, TAU, WORK, &LWORK, &INFO);

    deallocate(JPVT);
    deallocate(WORK);
    deallocate(TAU);

    if (std::abs(mat[0]) < eps) return 0;

    double **mat_tmp;
    allocate(mat_tmp, m_, n_);

    unsigned long k = 0;

    for (int j = 0; j < n_; ++j) {
        for (int i = 0; i < m_; ++i) {
            mat_tmp[i][j] = mat[k++];
        }
    }

    auto nrank = 0;
    for (int i = 0; i < nmin; ++i) {
        if (std::abs(mat_tmp[i][i]) > tolerance * std::abs(mat[0])) ++nrank;
    }

    deallocate(mat_tmp);

    return nrank;
}

int Fitting::rankSVD(const int m,
                     const int n,
                     double *mat,
                     const double tolerance)
{
    auto m_ = m;
    auto n_ = n;

    auto LWORK = 10 * m;
    int INFO;
    int *IWORK;
    auto ldu = 1, ldvt = 1;
    double *s, *WORK;
    double u[1], vt[1];

    const auto nmin = std::min<int>(m, n);

    allocate(IWORK, 8 * nmin);
    allocate(WORK, LWORK);
    allocate(s, nmin);

    char mode[] = "N";

    dgesdd_(mode, &m_, &n_, mat, &m_, s, u, &ldu, vt, &ldvt,
            WORK, &LWORK, IWORK, &INFO);

    auto rank = 0;
    for (int i = 0; i < nmin; ++i) {
        if (s[i] > s[0] * tolerance) ++rank;
    }

    deallocate(WORK);
    deallocate(IWORK);
    deallocate(s);

    return rank;
}

int Fitting::rankSVD2(const int m_in,
                      const int n_in,
                      double **mat,
                      const double tolerance)
{
    // Reveal the rank of matrix mat without destroying the matrix elements

    int i;
    double *arr;

    auto m = m_in;
    auto n = n_in;

    allocate(arr, m * n);

    auto k = 0;

    for (int j = 0; j < n; ++j) {
        for (i = 0; i < m; ++i) {
            arr[k++] = mat[i][j];
        }
    }

    auto LWORK = 10 * m;
    int INFO;
    int *IWORK;
    auto ldu = 1, ldvt = 1;
    double *s, *WORK;
    double u[1], vt[1];

    const int nmin = std::min<int>(m, n);

    allocate(IWORK, 8 * nmin);
    allocate(WORK, LWORK);
    allocate(s, nmin);

    char mode[] = "N";

    dgesdd_(mode, &m, &n, arr, &m, s, u, &ldu, vt, &ldvt,
            WORK, &LWORK, IWORK, &INFO);

    int rank = 0;
    for (i = 0; i < nmin; ++i) {
        if (s[i] > s[0] * tolerance) ++rank;
    }

    deallocate(IWORK);
    deallocate(WORK);
    deallocate(s);
    deallocate(arr);

    return rank;
}
