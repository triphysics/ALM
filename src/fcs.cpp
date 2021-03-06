/*
 fcs.cpp

 Copyright (c) 2014, 2015, 2016 Terumasa Tadano

 This file is distributed under the terms of the MIT license.
 Please see the file 'LICENCE.txt' in the root directory
 or http://opensource.org/licenses/mit-license.php for information.
*/

#include "fcs.h"
#include "constants.h"
#include "constraint.h"
#include "error.h"
#include "cluster.h"
#include "mathfunctions.h"
#include "memory.h"
#include "rref.h"
#include "symmetry.h"
#include "timer.h"
#include <iostream>
#include <iomanip>
#include <limits>
#include <cstddef>
#include <string>
#include <cmath>
#include "../external/combination.hpp"
#include <unordered_set>
#include <boost/algorithm/string/case_conv.hpp>

#if defined(_WIN32) || defined(_WIN64)
#undef min
#undef max
#endif

using namespace ALM_NS;

Fcs::Fcs()
{
    set_default_variables();
};

Fcs::~Fcs()
{
    deallocate_variables();
};

void Fcs::init(const Cluster *cluster,
               const Symmetry *symmetry,
               const size_t number_of_atoms,
               const int verbosity,
               Timer *timer)
{
    int i;
    const auto maxorder = cluster->get_maxorder();

    timer->start_clock("fcs");

    if (verbosity > 0) {
        std::cout << " FORCE CONSTANT" << std::endl;
        std::cout << " ==============" << std::endl << std::endl;
    }

    if (fc_table) {
        deallocate(fc_table);
    }
    allocate(fc_table, maxorder);

    if (nequiv) {
        deallocate(nequiv);
    }
    allocate(nequiv, maxorder);

    if (fc_zeros) {
        deallocate(fc_zeros);
    }
    allocate(fc_zeros, maxorder);

    // Generate force constants using the information of interacting atom pairs
    for (i = 0; i < maxorder; ++i) {
        generate_force_constant_table(i,
                                      number_of_atoms,
                                      cluster->get_cluster_list(i),
                                      symmetry,
                                      "Cartesian",
                                      fc_table[i],
                                      nequiv[i],
                                      fc_zeros[i],
                                      store_zeros);
    }

    if (verbosity > 0) {
        std::cout << std::endl;
        for (i = 0; i < maxorder; ++i) {
            std::cout << "  Number of " << std::setw(9)
                << cluster->get_ordername(i)
                << " FCs : " << nequiv[i].size();
            std::cout << std::endl;
        }
        std::cout << std::endl;


        timer->print_elapsed();
        std::cout << " -------------------------------------------------------------------" << std::endl;
        std::cout << std::endl;
    }

    timer->stop_clock("fcs");
}

void Fcs::set_default_variables()
{
    nequiv = nullptr;
    fc_table = nullptr;
    fc_zeros = nullptr;
    store_zeros = true;
}

void Fcs::deallocate_variables()
{
    if (nequiv) {
        deallocate(nequiv);
    }
    if (fc_table) {
        deallocate(fc_table);
    }
    if (fc_zeros) {
        deallocate(fc_zeros);
    }
}


void Fcs::generate_force_constant_table(const int order,
                                        const size_t nat,
                                        const std::set<IntList> &pairs,
                                        const Symmetry *symm_in,
                                        const std::string basis,
                                        std::vector<FcProperty> &fc_vec,
                                        std::vector<size_t> &ndup,
                                        std::vector<FcProperty> &fc_zeros_out,
                                        const bool store_zeros_in) const
{
    size_t i, j;
    int i1, i2;
    int i_prim;
    int *atmn, *atmn_mapped;
    int *ind, *ind_mapped;
    int *ind_tmp, *ind_mapped_tmp;
    int nxyz;
    unsigned int isym;

    double c_tmp;

    int **xyzcomponent;

    const auto nsym = symm_in->get_SymmData().size();
    bool is_zero;
    bool *is_searched;
    int **map_sym;
    double ***rotation;
    bool use_compatible = true;

    if (order < 0) return;

    allocate(rotation, nsym, 3, 3);
    allocate(map_sym, nat, nsym);
    int nsym_in_use = 0;

    get_available_symmop(nat,
                         symm_in,
                         basis,
                         nsym_in_use,
                         map_sym,
                         rotation,
                         use_compatible);

    allocate(atmn, order + 2);
    allocate(atmn_mapped, order + 2);
    allocate(ind, order + 2);
    allocate(ind_mapped, order + 2);
    allocate(ind_tmp, order);
    allocate(ind_mapped_tmp, order + 2);
    allocate(is_searched, 3 * nat);

    fc_vec.clear();
    ndup.clear();
    fc_zeros_out.clear();
    size_t nmother = 0;

    nxyz = static_cast<int>(std::pow(3.0, order + 2));

    allocate(xyzcomponent, nxyz, order + 2);
    get_xyzcomponent(order + 2, xyzcomponent);

    std::unordered_set<IntList> list_found;

    for (const auto &pair : pairs) {

        for (i = 0; i < order + 2; ++i) atmn[i] = pair.iarray[i];

        for (i1 = 0; i1 < nxyz; ++i1) {
            for (i = 0; i < order + 2; ++i) ind[i] = 3 * atmn[i] + xyzcomponent[i1][i];

            if (!is_ascending(order + 2, ind)) continue;

            i_prim = get_minimum_index_in_primitive(order + 2, ind, nat,
                                                    symm_in->get_nat_prim(),
                                                    symm_in->get_map_p2s());
            std::swap(ind[0], ind[i_prim]);
            sort_tail(order + 2, ind);

            is_zero = false;

            if (list_found.find(IntList(order + 2, ind)) != list_found.end()) continue; // Already exits!

            // Search symmetrically-dependent parameter set

            size_t ndeps = 0;

            for (isym = 0; isym < nsym_in_use; ++isym) {

                for (i = 0; i < order + 2; ++i) atmn_mapped[i] = map_sym[atmn[i]][isym];

                if (!is_inprim(order + 2,
                               atmn_mapped,
                               symm_in->get_nat_prim(),
                               symm_in->get_map_p2s()))
                    continue;

                for (i2 = 0; i2 < nxyz; ++i2) {

                    c_tmp = coef_sym(order + 2,
                                     rotation[isym],
                                     xyzcomponent[i1],
                                     xyzcomponent[i2]);

                    if (std::abs(c_tmp) > eps12) {
                        for (i = 0; i < order + 2; ++i)
                            ind_mapped[i] = 3 * atmn_mapped[i] + xyzcomponent[i2][i];

                        i_prim = get_minimum_index_in_primitive(order + 2,
                                                                ind_mapped,
                                                                nat,
                                                                symm_in->get_nat_prim(),
                                                                symm_in->get_map_p2s());
                        std::swap(ind_mapped[0], ind_mapped[i_prim]);
                        sort_tail(order + 2, ind_mapped);

                        if (!is_zero) {
                            bool zeroflag = true;
                            for (i = 0; i < order + 2; ++i) {
                                zeroflag = zeroflag & (ind[i] == ind_mapped[i]);
                            }
                            zeroflag = zeroflag & (std::abs(c_tmp + 1.0) < eps8);
                            is_zero = zeroflag;
                        }

                        // Add to found list (set) and fcset (vector) if the created is new one.

                        if (list_found.find(IntList(order + 2, ind_mapped)) == list_found.end()) {
                            list_found.insert(IntList(order + 2, ind_mapped));

                            fc_vec.emplace_back(FcProperty(order + 2,
                                                           c_tmp,
                                                           ind_mapped,
                                                           nmother));
                            ++ndeps;

                            // Add equivalent interaction list (permutation) if there are two or more indices
                            // which belong to the primitive cell.
                            // This procedure is necessary for fitting.

                            for (i = 0; i < 3 * nat; ++i) is_searched[i] = false;
                            is_searched[ind_mapped[0]] = true;
                            for (i = 1; i < order + 2; ++i) {
                                if ((!is_searched[ind_mapped[i]]) && is_inprim(ind_mapped[i],
                                                                               symm_in->get_nat_prim(),
                                                                               symm_in->get_map_p2s())) {

                                    for (j = 0; j < order + 2; ++j) ind_mapped_tmp[j] = ind_mapped[j];
                                    std::swap(ind_mapped_tmp[0], ind_mapped_tmp[i]);
                                    sort_tail(order + 2, ind_mapped_tmp);
                                    fc_vec.emplace_back(FcProperty(order + 2,
                                                                   c_tmp,
                                                                   ind_mapped_tmp,
                                                                   nmother));

                                    ++ndeps;

                                    is_searched[ind_mapped[i]] = true;
                                }
                            }


                        }
                    }
                }
            } // close symmetry loop

            if (is_zero) {
                if (store_zeros_in) {
                    for (auto it = fc_vec.rbegin(); it != fc_vec.rbegin() + ndeps; ++it) {
                        (*it).mother = std::numeric_limits<size_t>::max();
                        fc_zeros_out.push_back(*it);
                    }
                }
                for (i = 0; i < ndeps; ++i) fc_vec.pop_back();
            } else {
                ndup.push_back(ndeps);
                ++nmother;
            }

        } // close xyz component loop
    } // close atom number loop (iterator)

    deallocate(xyzcomponent);
    list_found.clear();
    deallocate(atmn);
    deallocate(atmn_mapped);
    deallocate(ind);
    deallocate(ind_mapped);
    deallocate(ind_tmp);
    deallocate(ind_mapped_tmp);
    deallocate(is_searched);
    deallocate(rotation);
    deallocate(map_sym);

    // sort fc_vec

    if (!ndup.empty()) {
        std::sort(fc_vec.begin(), fc_vec.begin() + ndup[0]);
        size_t nbegin = ndup[0];
        size_t nend;
        for (size_t mm = 1; mm < ndup.size(); ++mm) {
            nend = nbegin + ndup[mm];
            std::sort(fc_vec.begin() + nbegin, fc_vec.begin() + nend);
            nbegin += ndup[mm];
        }
    }
}

void Fcs::get_constraint_symmetry(const size_t nat,
                                  const Symmetry *symmetry,
                                  const int order,
                                  const std::string basis,
                                  const std::vector<FcProperty> &fc_table_in,
                                  const size_t nparams,
                                  const double tolerance,
                                  ConstraintSparseForm &const_out,
                                  const bool do_rref) const
{
    // Create constraint matrices arising from the crystal symmetry.
    // Necessary for hexagonal systems.

    int i;
    // int j;
    unsigned int isym;
    int ixyz;
    int *index_tmp;
    int **xyzcomponent;
    int nsym_in_use;
    std::unordered_set<FcProperty> list_found;

    typedef std::vector<ConstraintDoubleElement> ConstEntry;
    std::vector<ConstEntry> constraint_all;
    ConstEntry const_tmp;

    int **map_sym;
    double ***rotation;

    if (order < 0) return;

    const auto nsym = symmetry->get_SymmData().size();
    const auto natmin = symmetry->get_nat_prim();
    const auto nfcs = fc_table_in.size();
    const auto use_compatible = false;

    if (nparams == 0) return;

    allocate(rotation, nsym, 3, 3);
    allocate(map_sym, nat, nsym);
    allocate(index_tmp, order + 2);

    const auto nxyz = static_cast<int>(std::pow(static_cast<double>(3), order + 2));
    allocate(xyzcomponent, nxyz, order + 2);
    get_xyzcomponent(order + 2, xyzcomponent);

    const_out.clear();

    get_available_symmop(nat,
                         symmetry,
                         basis,
                         nsym_in_use,
                         map_sym,
                         rotation,
                         use_compatible);

    // Generate temporary list of parameters
    list_found.clear();
    for (const auto &p : fc_table_in) {
        for (i = 0; i < order + 2; ++i) index_tmp[i] = p.elems[i];
        list_found.insert(FcProperty(order + 2, p.sign,
                                     index_tmp, p.mother));
    }


#ifdef _OPENMP
#pragma omp parallel
#endif
    {
        int j;
        int i_prim;
        int loc_nonzero;
        int *ind;
        int *atm_index, *atm_index_symm;
        int *xyz_index;
        double c_tmp;
        // double maxabs;

        std::unordered_set<FcProperty>::iterator iter_found;
        std::vector<double> const_now_omp;
        std::vector<std::vector<double>> const_omp;

        ConstEntry const_tmp_omp;
        std::vector<ConstEntry> constraint_list_omp;

        allocate(ind, order + 2);
        allocate(atm_index, order + 2);
        allocate(atm_index_symm, order + 2);
        allocate(xyz_index, order + 2);

        const_omp.clear();
        const_now_omp.resize(nparams);

#ifdef _OPENMP
#pragma omp for private(i, isym, ixyz), schedule(static)
#endif
        for (long ii = 0; ii < nfcs; ++ii) {
            FcProperty list_tmp = fc_table_in[ii];

            for (i = 0; i < order + 2; ++i) {
                atm_index[i] = list_tmp.elems[i] / 3;
                xyz_index[i] = list_tmp.elems[i] % 3;
            }

            for (isym = 0; isym < nsym_in_use; ++isym) {

                for (i = 0; i < order + 2; ++i)
                    atm_index_symm[i] = map_sym[atm_index[i]][isym];
                if (!is_inprim(order + 2, atm_index_symm, natmin, symmetry->get_map_p2s())) continue;

                for (i = 0; i < nparams; ++i) const_now_omp[i] = 0.0;

                const_now_omp[list_tmp.mother] = -list_tmp.sign;

                for (ixyz = 0; ixyz < nxyz; ++ixyz) {
                    for (i = 0; i < order + 2; ++i)
                        ind[i] = 3 * atm_index_symm[i] + xyzcomponent[ixyz][i];

                    i_prim = get_minimum_index_in_primitive(order + 2, ind, nat, natmin, symmetry->get_map_p2s());
                    std::swap(ind[0], ind[i_prim]);
                    sort_tail(order + 2, ind);

                    iter_found = list_found.find(FcProperty(order + 2, 1.0, ind, 1));
                    if (iter_found != list_found.end()) {
                        c_tmp = coef_sym(order + 2, rotation[isym], xyz_index, xyzcomponent[ixyz]);
                        const_now_omp[(*iter_found).mother] += (*iter_found).sign * c_tmp;
                    }
                }

                if (!is_allzero(const_now_omp, eps8, loc_nonzero)) {
                    if (const_now_omp[loc_nonzero] < 0.0) {
                        for (j = 0; j < nparams; ++j) const_now_omp[j] *= -1.0;
                    }
                    // maxabs = 0.0;
                    // for (j = 0; j < nparams; ++j) {
                    //     maxabs = std::max(maxabs, std::abs(const_now_omp[j]));
                    // }
                    // std::cout << "maxabs = " << maxabs << std::endl;

                    const_tmp_omp.clear();
                    for (j = 0; j < nparams; ++j) {
                        if (std::abs(const_now_omp[j]) >= eps8) {
                            const_tmp_omp.emplace_back(j, const_now_omp[j]);
                        }
                    }
                    constraint_list_omp.emplace_back(const_tmp_omp);
                }

            } // close isym loop
        } // close ii loop

        deallocate(ind);
        deallocate(atm_index);
        deallocate(atm_index_symm);
        deallocate(xyz_index);

#pragma omp critical
        {
            for (const auto &it : constraint_list_omp) {
                constraint_all.emplace_back(it);
            }
        }
        constraint_list_omp.clear();
    } // close openmp region

    deallocate(xyzcomponent);
    deallocate(index_tmp);
    deallocate(rotation);
    deallocate(map_sym);

    std::sort(constraint_all.begin(), constraint_all.end());
    constraint_all.erase(std::unique(constraint_all.begin(),
                                     constraint_all.end()),
                         constraint_all.end());

    typedef std::map<size_t, double> ConstDoubleEntry;
    ConstDoubleEntry const_tmp2;
    auto division_factor = 1.0;
    int counter;
    const_out.clear();

    for (const auto &it : constraint_all) {
        const_tmp2.clear();
        counter = 0;
        for (const auto &it2 : it) {
            if (counter == 0) {
                division_factor = 1.0 / it2.val;
            }
            const_tmp2[it2.col] = it2.val * division_factor;
            ++counter;
        }
        const_out.emplace_back(const_tmp2);
    }
    constraint_all.clear();

    if (do_rref) rref_sparse(nparams, const_out, tolerance);
}

std::vector<size_t>* Fcs::get_nequiv() const
{
    return nequiv;
}

std::vector<FcProperty>* Fcs::get_fc_table() const
{
    return fc_table;
}

void Fcs::get_available_symmop(const size_t nat,
                               const Symmetry *symmetry,
                               const std::string basis,
                               int &nsym_avail,
                               int **mapping_symm,
                               double ***rotation,
                               const bool use_compatible) const
{
    // Return mapping information of atoms and the rotation matrices of symmetry operations
    // that are (compatible, incompatible) with the given lattice basis (Cartesian or Lattice).

    // use_compatible == true returns the compatible space group (for creating fc_table)
    // use_compatible == false returnes the incompatible supace group (for creating constraint)

    int i, j;
    int counter = 0;

    nsym_avail = 0;

    if (basis == "Cartesian") {

        for (auto it = symmetry->get_SymmData().begin(); it != symmetry->get_SymmData().end(); ++it) {

            if ((*it).compatible_with_cartesian == use_compatible) {

                for (i = 0; i < 3; ++i) {
                    for (j = 0; j < 3; ++j) {
                        rotation[nsym_avail][i][j] = (*it).rotation_cart[i][j];
                    }
                }
                for (i = 0; i < nat; ++i) {
                    mapping_symm[i][nsym_avail] = symmetry->get_map_sym()[i][counter];
                }
                ++nsym_avail;
            }
            ++counter;
        }

    } else if (basis == "Lattice") {

        for (auto it = symmetry->get_SymmData().begin(); it != symmetry->get_SymmData().end(); ++it) {
            if ((*it).compatible_with_lattice == use_compatible) {
                for (i = 0; i < 3; ++i) {
                    for (j = 0; j < 3; ++j) {
                        rotation[nsym_avail][i][j]
                            = static_cast<double>((*it).rotation[i][j]);
                    }
                }
                for (i = 0; i < nat; ++i) {
                    mapping_symm[i][nsym_avail] = symmetry->get_map_sym()[i][counter];
                }
                ++nsym_avail;
            }
            ++counter;
        }


    } else {
        deallocate(rotation);
        deallocate(mapping_symm);
        exit("get_available_symmop", "Invalid basis input");
    }
}

double Fcs::coef_sym(const int n,
                     const double * const *rot,
                     const int *arr1,
                     const int *arr2) const
{
    auto tmp = 1.0;

    for (auto i = 0; i < n; ++i) {
        tmp *= rot[arr2[i]][arr1[i]];
    }
    return tmp;
}

bool Fcs::is_ascending(const int n,
                       const int *arr) const
{
    for (auto i = 0; i < n - 1; ++i) {
        if (arr[i] > arr[i + 1]) return false;
    }
    return true;
}

int Fcs::get_minimum_index_in_primitive(const int n,
                                        const int *arr,
                                        const size_t nat,
                                        const size_t natmin,
                                        const std::vector<std::vector<int>> &map_p2s) const
{
    int i, atmnum;

    std::vector<size_t> ind(n, 3 * nat);

    for (i = 0; i < n; ++i) {

        atmnum = arr[i] / 3;

        for (size_t j = 0; j < natmin; ++j) {
            if (map_p2s[j][0] == atmnum) {
                ind[i] = arr[i];
            }
        }
    }

    auto minval = ind[0];
    auto minloc = 0;

    for (i = 0; i < n; ++i) {
        if (ind[i] < minval) {
            minval = ind[i];
            minloc = i;
        }
    }

    return minloc;
}

bool Fcs::is_inprim(const int n,
                    const int *arr,
                    const size_t natmin,
                    const std::vector<std::vector<int>> &map_p2s) const
{
    for (auto i = 0; i < n; ++i) {
        for (size_t j = 0; j < natmin; ++j) {
            if (map_p2s[j][0] == arr[i]) return true;
        }
    }
    return false;
}

bool Fcs::is_inprim(const int n,
                    const size_t natmin,
                    const std::vector<std::vector<int>> &map_p2s) const
{
    const auto atmn = n / 3;

    for (size_t i = 0; i < natmin; ++i) {
        if (map_p2s[i][0] == atmn) return true;
    }

    return false;
}

void Fcs::get_xyzcomponent(const int n,
                           int **xyz) const
{
    // Return xyz component for the given order using boost algorithm library

    int i;

    std::vector<int> v(3 * n);

    for (i = 0; i < n; ++i) v[i] = 0;
    for (i = n; i < 2 * n; ++i) v[i] = 1;
    for (i = 2 * n; i < 3 * n; ++i) v[i] = 2;

    auto m = 0;

    do {
        xyz[m][0] = v[0];
        for (i = 1; i < n; ++i) xyz[m][i] = v[i];
        ++m;
    } while (boost::next_partial_permutation(v.begin(), v.begin() + n, v.end()));
}

bool Fcs::is_allzero(const std::vector<double> &vec,
                     const double tol,
                     int &loc) const
{
    loc = -1;
    const auto n = vec.size();
    for (auto i = 0; i < n; ++i) {
        if (std::abs(vec[i]) > tol) {
            loc = i;
            return false;
        }
    }
    return true;
}
