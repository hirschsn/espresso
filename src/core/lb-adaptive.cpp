/*
   Copyright (C) 2010,2011,2012,2013,2014,2015 The ESPResSo project
   Copyright (C) 2002,2003,2004,2005,2006,2007,2008,2009,2010
   Max-Planck-Institute for Polymer Research, Theory Group,

   This file is part of ESPResSo.

   ESPResSo is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   ESPResSo is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
   */

/** \file lb-adaptive.cpp
 *
 * Adaptive Lattice Boltzmann Scheme using CPU.
 * Implementation file for \ref lb-adaptive.hpp.
 */

#include <algorithm>
#include <assert.h>
#include <fstream>
#include <iostream>
#include <stdlib.h>

#include "communication.hpp"
#include "constraint.hpp"
#include "lb-adaptive.hpp"
#include "lb-boundaries.hpp"
#include "lb-d3q19.hpp"
#include "lb.hpp"
#include "random.hpp"
#include "utils.hpp"

#ifdef LB_ADAPTIVE

/* Code duplication from lb.cpp */
/* For the D3Q19 model most functions have a separate implementation
 * where the coefficients and the velocity vectors are hardcoded
 * explicitly. This saves a lot of multiplications with 1's and 0's
 * thus making the code more efficient. */
#ifndef D3Q19
#define D3Q19
#endif // D3Q19

#if (!defined(FLATNOISE) && !defined(GAUSSRANDOMCUT) && !defined(GAUSSRANDOM))
#define FLATNOISE
#endif // (!defined(FLATNOISE) && !defined(GAUSSRANDOMCUT) &&
       // !defined(GAUSSRANDOM))

/* "external variables" */
p8est_connectivity_t *conn;
p8est_t *p8est;
p8est_ghost_t *lbadapt_ghost;
p8est_ghostvirt_t *lbadapt_ghost_virt;
p8est_mesh_t *lbadapt_mesh;
lbadapt_payload_t **lbadapt_local_data;
lbadapt_payload_t **lbadapt_ghost_data;
int coarsest_level_local;
int finest_level_local;
int coarsest_level_ghost;
int finest_level_ghost;
int finest_level_global;

/*** MAPPING OF CI FROM ESPRESSO LBM TO P4EST FACE-/EDGE ENUMERATION ***/
/**
 * | ESPResSo c_i | p4est face | p4est edge | vec          |
 * |--------------+------------+------------+--------------|
 * |            0 |          - |          - | { 0,  0,  0} |
 * |            1 |          1 |          - | { 1,  0,  0} |
 * |            2 |          0 |          - | {-1,  0,  0} |
 * |            3 |          3 |          - | { 0,  1,  0} |
 * |            4 |          2 |          - | { 0, -1,  0} |
 * |            5 |          5 |          - | { 0,  0,  1} |
 * |            6 |          4 |          - | { 0,  0, -1} |
 * |            7 |          - |         11 | { 1,  1,  0} |
 * |            8 |          - |          8 | {-1, -1,  0} |
 * |            9 |          - |          9 | { 1, -1,  0} |
 * |           10 |          - |         10 | {-1,  1,  0} |
 * |           11 |          - |          7 | { 1,  0,  1} |
 * |           12 |          - |          4 | {-1,  0, -1} |
 * |           13 |          - |          5 | { 1,  0, -1} |
 * |           14 |          - |          6 | {-1,  0,  1} |
 * |           15 |          - |          3 | { 0,  1,  1} |
 * |           16 |          - |          0 | { 0, -1, -1} |
 * |           17 |          - |          1 | { 0,  1, -1} |
 * |           18 |          - |          2 | { 0, -1,  1} |
 */

/*** SETUP ***/
void lbadapt_allocate_data() {
  int level;
  coarsest_level_local = -1;
  finest_level_local = -1;
  coarsest_level_ghost = -1;
  finest_level_ghost = -1;

  /** local cells */
  for (level = 0; level < P8EST_MAXLEVEL; ++level) {
    if ((((lbadapt_mesh->quad_level + level)->elem_count > 0) ||
         ((lbadapt_mesh->virtual_qlevels + level)->elem_count > 0)) &&
        coarsest_level_local == -1) {
      coarsest_level_local = level;
    }
    if ((coarsest_level_local != -1) &&
        (((lbadapt_mesh->quad_level + level)->elem_count == 0) &&
         ((lbadapt_mesh->virtual_qlevels + level)->elem_count == 0))) {
      finest_level_local = level;
      break;
    }
  }
  if (finest_level_local - coarsest_level_local > 0) {
    lbadapt_local_data = P4EST_ALLOC(lbadapt_payload_t *,
                                     finest_level_local - coarsest_level_local);
    for (level = coarsest_level_local; level < finest_level_local; ++level) {
      lbadapt_local_data[level - coarsest_level_local] = P4EST_ALLOC(
          lbadapt_payload_t,
          (lbadapt_mesh->quad_level + level)->elem_count +
              P8EST_CHILDREN *
                  (lbadapt_mesh->virtual_qlevels + level)->elem_count);
    }
  }

  /** ghost */
  for (int level = 0; level < P8EST_MAXLEVEL; ++level) {
    if ((((lbadapt_mesh->ghost_level + level)->elem_count > 0) ||
         ((lbadapt_mesh->virtual_glevels + level)->elem_count > 0)) &&
        coarsest_level_ghost == -1) {
      coarsest_level_ghost = level;
    }
    if ((coarsest_level_ghost != -1) &&
        (((lbadapt_mesh->ghost_level + level)->elem_count == 0) &&
         ((lbadapt_mesh->virtual_glevels + level)->elem_count == 0))) {
      finest_level_ghost = level;
      break;
    }
  }
  if (finest_level_ghost - coarsest_level_ghost > 0) {
    lbadapt_ghost_data = P4EST_ALLOC(lbadapt_payload_t *,
                                     finest_level_ghost - coarsest_level_ghost);
    for (level = coarsest_level_ghost; level < finest_level_ghost; ++level) {
      lbadapt_ghost_data[level - coarsest_level_ghost] = P4EST_ALLOC(
          lbadapt_payload_t,
          (lbadapt_mesh->ghost_level + level)->elem_count +
              P8EST_CHILDREN *
                  (lbadapt_mesh->virtual_glevels + level)->elem_count);
    }
  }
}

void lbadapt_init() {
  if (lbadapt_local_data == NULL) {
    lbadapt_allocate_data();
  }
  int status = 0;
  lbadapt_payload_t *data;
  for (int level = 0; level < P8EST_MAXLEVEL; ++level) {
    p8est_meshiter_t *mesh_iter = p8est_meshiter_new_ext(
        p8est, lbadapt_ghost, lbadapt_mesh, level, P8EST_CONNECT_EDGE,
        P8EST_TRAVERSE_LOCALGHOST, P8EST_TRAVERSE_REALVIRTUAL,
        P8EST_TRAVERSE_PARBOUNDINNER);

    while (status != P8EST_MESHITER_DONE) {
      status = p8est_meshiter_next(mesh_iter);
      if (status != P8EST_MESHITER_DONE) {
        if (!mesh_iter->current_is_ghost) {
          data =
              &lbadapt_local_data[level][p8est_meshiter_get_current_storage_id(
                  mesh_iter)];
        } else {
          data =
              &lbadapt_ghost_data[level][p8est_meshiter_get_current_storage_id(
                  mesh_iter)];
        }

        data->boundary = 0;
        // data->lbfields = (LB_FluidNode) malloc(sizeof(LB_FluidNode));
        for (int i = 0; i < lbmodel.n_veloc; i++) {
          data->lbfluid[0][i] = 0.;
          data->lbfluid[1][i] = 0.;
          data->modes[i] = 0.;
        }

        // ints
        data->lbfields.recalc_fields = 1;
        data->lbfields.has_force = 0;

        // 1D "array"
        data->lbfields.rho[0] = 0.;

        // 3D arrays
        for (int i = 0; i < 3; i++) {
          data->lbfields.j[i] = 0;
          data->lbfields.force[i] = 0;
#ifdef IMMERSED_BOUNDARY
          data->lbfields.force_buf[i] = 0;
#endif // IMMERSED_BOUNDARY
        }

        // 6D array
        for (int i = 0; i < 6; i++) {
          data->lbfields.pi[i] = 0;
        }
      }
    }
    p8est_meshiter_destroy(mesh_iter);
  }
}

void lbadapt_init_force_per_cell() {
  if (lbadapt_local_data == NULL) {
    lbadapt_allocate_data();
  }
  int status = 0;
  lbadapt_payload_t *data;
  double h; /* local meshwidth */
  for (int level = 0; level < P8EST_MAXLEVEL; ++level) {
    h = (double)P8EST_QUADRANT_LEN(level) / (double)P8EST_ROOT_LEN;

    p8est_meshiter_t *mesh_iter = p8est_meshiter_new_ext(
        p8est, lbadapt_ghost, lbadapt_mesh, level, P8EST_CONNECT_EDGE,
        P8EST_TRAVERSE_LOCALGHOST, P8EST_TRAVERSE_REALVIRTUAL,
        P8EST_TRAVERSE_PARBOUNDINNER);

    while (status != P8EST_MESHITER_DONE) {
      status = p8est_meshiter_next(mesh_iter);
      if (status != P8EST_MESHITER_DONE) {
        if (!mesh_iter->current_is_ghost) {
          data =
              &lbadapt_local_data[level][p8est_meshiter_get_current_storage_id(
                  mesh_iter)];
        } else {
          data =
              &lbadapt_ghost_data[level][p8est_meshiter_get_current_storage_id(
                  mesh_iter)];
        }
#ifdef EXTERNAL_FORCES
        // unit conversion: force density
        data->lbfields.force[0] = lbpar.ext_force[0] * SQR(h) * SQR(lbpar.tau);
        data->lbfields.force[1] = lbpar.ext_force[1] * SQR(h) * SQR(lbpar.tau);
        data->lbfields.force[2] = lbpar.ext_force[2] * SQR(h) * SQR(lbpar.tau);
#else  // EXTERNAL_FORCES
        data->lbfields.force[0] = 0.0;
        data->lbfields.force[1] = 0.0;
        data->lbfields.force[2] = 0.0;
        data->lbfields.has_force = 0;
#endif // EXTERNAL_FORCES
      }
    }
    p8est_meshiter_destroy(mesh_iter);
  }
}

void lbadapt_init_fluid_per_cell() {
  if (lbadapt_local_data == NULL) {
    lbadapt_allocate_data();
  }
  int status = 0;
  lbadapt_payload_t *data;
  double h; /* local meshwidth */
  for (int level = 0; level < P8EST_MAXLEVEL; ++level) {
    h = (double)P8EST_QUADRANT_LEN(level) / (double)P8EST_ROOT_LEN;

    p8est_meshiter_t *mesh_iter = p8est_meshiter_new_ext(
        p8est, lbadapt_ghost, lbadapt_mesh, level, P8EST_CONNECT_EDGE,
        P8EST_TRAVERSE_LOCALGHOST, P8EST_TRAVERSE_REALVIRTUAL,
        P8EST_TRAVERSE_PARBOUNDINNER);

    while (status != P8EST_MESHITER_DONE) {
      status = p8est_meshiter_next(mesh_iter);
      if (status != P8EST_MESHITER_DONE) {
        if (!mesh_iter->current_is_ghost) {
          data =
              &lbadapt_local_data[level][p8est_meshiter_get_current_storage_id(
                  mesh_iter)];
        } else {
          data =
              &lbadapt_ghost_data[level][p8est_meshiter_get_current_storage_id(
                  mesh_iter)];
        }
        // convert rho to lattice units
        double rho = lbpar.rho[0] * h * h * h;
        // start with fluid at rest and no stress
        double j[3] = {0., 0., 0.};
        double pi[6] = {0., 0., 0., 0., 0., 0.};
        lbadapt_calc_n_from_rho_j_pi(data->lbfluid, rho, j, pi, h);
      }
    }
    p8est_meshiter_destroy(mesh_iter);
  }
}

void lbadapt_reinit_fluid_per_cell() {
  if (lbadapt_local_data == NULL) {
    lbadapt_allocate_data();
  }
  int status = 0;
  lbadapt_payload_t *data;
  double h; /* local meshwidth */
  for (int level = 0; level < P8EST_MAXLEVEL; ++level) {
    h = (double)P8EST_QUADRANT_LEN(level) / (double)P8EST_ROOT_LEN;

    p8est_meshiter_t *mesh_iter = p8est_meshiter_new_ext(
        p8est, lbadapt_ghost, lbadapt_mesh, level, P8EST_CONNECT_EDGE,
        P8EST_TRAVERSE_LOCALGHOST, P8EST_TRAVERSE_REALVIRTUAL,
        P8EST_TRAVERSE_PARBOUNDINNER);

    while (status != P8EST_MESHITER_DONE) {
      status = p8est_meshiter_next(mesh_iter);
      if (status != P8EST_MESHITER_DONE) {
        if (!mesh_iter->current_is_ghost) {
          data =
              &lbadapt_local_data[level][p8est_meshiter_get_current_storage_id(
                  mesh_iter)];
        } else {
          data =
              &lbadapt_ghost_data[level][p8est_meshiter_get_current_storage_id(
                  mesh_iter)];
        }
        // calculate equilibrium distribution
        lbadapt_calc_n_from_rho_j_pi(data->lbfluid, data->lbfields.rho[0],
                                     data->lbfields.j, data->lbfields.pi, h);
#ifdef LB_BOUNDARIES
        data->lbfields.boundary = 0;
#endif // LB_BOUNDARIES
      }
    }
    p8est_meshiter_destroy(mesh_iter);
  }
}

int lbadapt_get_global_maxlevel() {
  int i;
  int local_res = -1;
  int global_res;
  p8est_tree_t *tree;

  /* get local max level */
  for (i = p8est->first_local_tree; i <= p8est->last_local_tree; ++i) {
    tree = p8est_tree_array_index(p8est->trees, i);
    if (local_res < tree->maxlevel) {
      local_res = tree->maxlevel;
    }
  }

  /* synchronize and return obtained result */
  sc_MPI_Allreduce(&local_res, &global_res, 1, sc_MPI_INT, sc_MPI_MAX,
                   p8est->mpicomm);

  return global_res;
}

void lbadapt_replace_quads(p8est_t *p8est, p4est_topidx_t which_tree,
                           int num_outgoing, p8est_quadrant_t *outgoing[],
                           int num_incoming, p8est_quadrant_t *incoming[]) {
  lbadapt_payload_t *parent_data, *child_data;
  double h;

  if (num_outgoing > 1) {
    // coarsening
  } else {
    // refinement
    parent_data = (lbadapt_payload_t *)outgoing[0]->p.user_data;
    h = (double)P8EST_QUADRANT_LEN(incoming[0]->level) / (double)P8EST_ROOT_LEN;
    for (int i = 0; i < P8EST_CHILDREN; i++) {
      child_data = (lbadapt_payload_t *)incoming[i]->p.user_data;
      // init cells: first insert lbfields as good as possible
      // and calc populations from those values.
      // init force
      for (int j = 0; j < 3; j++)
        child_data->lbfields.force[j] = 0.25 * parent_data->lbfields.force[j];
      child_data->lbfields.rho[0] = 0.125 * parent_data->lbfields.rho[0];
      for (int j = 0; j < 3; j++)
        child_data->lbfields.j[j] = 0.;
      for (int j = 0; j < 6; j++)
        child_data->lbfields.pi[j] = 0.;

      lbadapt_calc_n_from_rho_j_pi(
          child_data->lbfluid, child_data->lbfields.rho[0],
          child_data->lbfields.j, child_data->lbfields.pi, h);
    }
  }
}

/*** REFINEMENT ***/
int refine_uniform(p8est_t *p8est, p4est_topidx_t which_tree,
                   p8est_quadrant_t *quadrant) {
  return 1;
}

int refine_random(p8est_t *p8est, p4est_topidx_t which_tree,
                  p8est_quadrant_t *quadrant) {
  return rand() % 2;
}

int refine_regional(p8est_t *p8est, p4est_topidx_t which_tree,
                    p8est_quadrant_t *q) {
  if ((0.25 <= (((double)(q->z >> (P8EST_QMAXLEVEL - (int)q->level + 1))) /
                ((double)(1 << (int)q->level))) +
                   1. / ((double)(1 << (int)q->level))) &&
      (0.75 >= (((double)(q->z >> (P8EST_QMAXLEVEL - (int)q->level + 1))) /
                ((double)(1 << (int)q->level))))) {
    return 1;
  }
  return 0;
}

/*** HELPER FUNCTIONS ***/
void lbadapt_get_midpoint(p8est_t *p8est, p4est_topidx_t which_tree,
                          p8est_quadrant_t *q, double *xyz) {
  int base = P8EST_QUADRANT_LEN(q->level);
  int root = P8EST_ROOT_LEN;
  double half_length = ((double)base / (double)root) * 0.5;

  p8est_qcoord_to_vertex(p8est->connectivity, which_tree, q->x, q->y, q->z,
                         xyz);

  for (int i = 0; i < P8EST_DIM; ++i) {
    xyz[i] += half_length;
  }
}

int lbadapt_calc_n_from_rho_j_pi(double datafield[2][19], double rho, double *j,
                                 double *pi, double h) {
  int i;
  double local_rho, local_j[3], local_pi[6], trace;
  const double avg_rho = lbpar.rho[0] * h * h * h;

  local_rho = rho;

  for (i = 0; i < 3; i++)
    local_j[i] = j[i];

  for (i = 0; i < 6; i++)
    local_pi[i] = pi[i];

  trace = local_pi[0] + local_pi[2] + local_pi[5];

#ifdef D3Q19
  double rho_times_coeff;
  double tmp1, tmp2;

  /* update the q=0 sublattice */
  datafield[0][0] = 1. / 3. * (local_rho - avg_rho) - 0.5 * trace;

  /* update the q=1 sublattice */
  rho_times_coeff = 1. / 18. * (local_rho - avg_rho);

  datafield[0][1] = rho_times_coeff + 1. / 6. * local_j[0] +
                    0.25 * local_pi[0] - 1. / 12. * trace;
  datafield[0][2] = rho_times_coeff - 1. / 6. * local_j[0] +
                    0.25 * local_pi[0] - 1. / 12. * trace;
  datafield[0][3] = rho_times_coeff + 1. / 6. * local_j[1] +
                    0.25 * local_pi[2] - 1. / 12. * trace;
  datafield[0][4] = rho_times_coeff - 1. / 6. * local_j[1] +
                    0.25 * local_pi[2] - 1. / 12. * trace;
  datafield[0][5] = rho_times_coeff + 1. / 6. * local_j[2] +
                    0.25 * local_pi[5] - 1. / 12. * trace;
  datafield[0][6] = rho_times_coeff - 1. / 6. * local_j[2] +
                    0.25 * local_pi[5] - 1. / 12. * trace;

  /* update the q=2 sublattice */
  rho_times_coeff = 1. / 36. * (local_rho - avg_rho);

  tmp1 = local_pi[0] + local_pi[2];
  tmp2 = 2.0 * local_pi[1];

  datafield[0][7] = rho_times_coeff + 1. / 12. * (local_j[0] + local_j[1]) +
                    0.125 * (tmp1 + tmp2) - 1. / 24. * trace;
  datafield[0][8] = rho_times_coeff - 1. / 12. * (local_j[0] + local_j[1]) +
                    0.125 * (tmp1 + tmp2) - 1. / 24. * trace;
  datafield[0][9] = rho_times_coeff + 1. / 12. * (local_j[0] - local_j[1]) +
                    0.125 * (tmp1 - tmp2) - 1. / 24. * trace;
  datafield[0][10] = rho_times_coeff - 1. / 12. * (local_j[0] - local_j[1]) +
                     0.125 * (tmp1 - tmp2) - 1. / 24. * trace;

  tmp1 = local_pi[0] + local_pi[5];
  tmp2 = 2.0 * local_pi[3];

  datafield[0][11] = rho_times_coeff + 1. / 12. * (local_j[0] + local_j[2]) +
                     0.125 * (tmp1 + tmp2) - 1. / 24. * trace;
  datafield[0][12] = rho_times_coeff - 1. / 12. * (local_j[0] + local_j[2]) +
                     0.125 * (tmp1 + tmp2) - 1. / 24. * trace;
  datafield[0][13] = rho_times_coeff + 1. / 12. * (local_j[0] - local_j[2]) +
                     0.125 * (tmp1 - tmp2) - 1. / 24. * trace;
  datafield[0][14] = rho_times_coeff - 1. / 12. * (local_j[0] - local_j[2]) +
                     0.125 * (tmp1 - tmp2) - 1. / 24. * trace;

  tmp1 = local_pi[2] + local_pi[5];
  tmp2 = 2.0 * local_pi[4];

  datafield[0][15] = rho_times_coeff + 1. / 12. * (local_j[1] + local_j[2]) +
                     0.125 * (tmp1 + tmp2) - 1. / 24. * trace;
  datafield[0][16] = rho_times_coeff - 1. / 12. * (local_j[1] + local_j[2]) +
                     0.125 * (tmp1 + tmp2) - 1. / 24. * trace;
  datafield[0][17] = rho_times_coeff + 1. / 12. * (local_j[1] - local_j[2]) +
                     0.125 * (tmp1 - tmp2) - 1. / 24. * trace;
  datafield[0][18] = rho_times_coeff - 1. / 12. * (local_j[1] - local_j[2]) +
                     0.125 * (tmp1 - tmp2) - 1. / 24. * trace;
#else  // D3Q19
  int i;
  double tmp = 0.0;
  double(*c)[3] = lbmodel.c;
  double(*coeff)[4] = lbmodel.coeff;

  for (i = 0; i < lbmodel.n_veloc; i++) {
    tmp = local_pi[0] * SQR(c[i][0]) +
          (2.0 * local_pi[1] * c[i][0] + local_pi[2] * c[i][1]) * c[i][1] +
          (2.0 * (local_pi[3] * c[i][0] + local_pi[4] * c[i][1]) +
           local_pi[5] * c[i][2]) *
              c[i][2];

    datafield[0][i] = coeff[i][0] * (local_rho - avg_rho);
    datafield[0][i] += coeff[i][1] * scalar(local_j, c[i]);
    datafield[0][i] += coeff[i][2] * tmp;
    datafield[0][i] += coeff[i][3] * trace;
  }
#endif // D3Q19

  return 0;
}

int lbadapt_calc_local_fields(double mode[19], double force[3], int boundary,
                              int has_force, double h, double *rho, double *j,
                              double *pi) {
#ifdef LB_BOUNDARIES
  if (boundary) {
    // set all to 0 on boundary
    *rho = lbpar.rho[0] * h * h * h;
    j[0] = 0.;
    j[1] = 0.;
    j[2] = 0.;
    if (pi) {
      pi[0] = 0.;
      pi[1] = 0.;
      pi[2] = 0.;
      pi[3] = 0.;
      pi[4] = 0.;
      pi[5] = 0.;
    }
    return 0;
  }
#endif // LB_BOUNDARIES
  double cpmode[19];
  for (int i = 0; i < 19; ++i) {
    cpmode[i] = mode[i];
  }
  double modes_from_pi_eq[6];

  *rho = cpmode[0] + lbpar.rho[0] * h * h * h;

  j[0] = cpmode[1];
  j[1] = cpmode[2];
  j[2] = cpmode[3];

#ifndef EXTERNAL_FORCES
  if (has_force)
#endif // EXTERNAL_FORCES
  {
    j[0] += 0.5 * force[0];
    j[1] += 0.5 * force[1];
    j[2] += 0.5 * force[2];
  }
  if (!pi)
    return 0;

  /* equilibrium part of the stress modes */
  modes_from_pi_eq[0] = scalar(j, j) / *rho;
  modes_from_pi_eq[1] = (SQR(j[0]) - SQR(j[1])) / *rho;
  modes_from_pi_eq[2] = (scalar(j, j) - 3.0 * SQR(j[2])) / *rho;
  modes_from_pi_eq[3] = j[0] * j[1] / *rho;
  modes_from_pi_eq[4] = j[0] * j[2] / *rho;
  modes_from_pi_eq[5] = j[1] * j[2] / *rho;

  /* Now we must predict the outcome of the next collision */
  /* We immediately average pre- and post-collision. */
  cpmode[4] = modes_from_pi_eq[0] +
              (0.5 + 0.5 * gamma_bulk) * (cpmode[4] - modes_from_pi_eq[0]);
  cpmode[5] = modes_from_pi_eq[1] +
              (0.5 + 0.5 * gamma_shear) * (cpmode[5] - modes_from_pi_eq[1]);
  cpmode[6] = modes_from_pi_eq[2] +
              (0.5 + 0.5 * gamma_shear) * (cpmode[6] - modes_from_pi_eq[2]);
  cpmode[7] = modes_from_pi_eq[3] +
              (0.5 + 0.5 * gamma_shear) * (cpmode[7] - modes_from_pi_eq[3]);
  cpmode[8] = modes_from_pi_eq[4] +
              (0.5 + 0.5 * gamma_shear) * (cpmode[8] - modes_from_pi_eq[4]);
  cpmode[9] = modes_from_pi_eq[5] +
              (0.5 + 0.5 * gamma_shear) * (cpmode[9] - modes_from_pi_eq[5]);

  // Transform the stress tensor components according to the modes that
  // correspond to those used by U. Schiller. In terms of populations this
  // expression then corresponds exactly to those in Eqs. 116 - 121 in the
  // Duenweg and Ladd paper, when these are written out in populations.
  // But to ensure this, the expression in Schiller's modes has to be
  // different!

  pi[0] =
      (2.0 * (cpmode[0] + cpmode[4]) + cpmode[6] + 3.0 * cpmode[5]) / 6.0; // xx
  pi[1] = cpmode[7];                                                       // xy
  pi[2] =
      (2.0 * (cpmode[0] + cpmode[4]) + cpmode[6] - 3.0 * cpmode[5]) / 6.0; // yy
  pi[3] = cpmode[8];                                                       // xz
  pi[4] = cpmode[9];                                                       // yz
  pi[5] = (cpmode[0] + cpmode[4] - cpmode[6]) / 3.0;                       // zz

  return 0;
}

int lbadapt_calc_modes(double population[2][19], double *mode) {
#ifdef D3Q19
  double n0, n1p, n1m, n2p, n2m, n3p, n3m, n4p, n4m, n5p, n5m, n6p, n6m, n7p,
      n7m, n8p, n8m, n9p, n9m;

  // clang-format off
  n0  = population[0][ 0];
  n1p = population[0][ 1] + population[0][ 2];
  n1m = population[0][ 1] - population[0][ 2];
  n2p = population[0][ 3] + population[0][ 4];
  n2m = population[0][ 3] - population[0][ 4];
  n3p = population[0][ 5] + population[0][ 6];
  n3m = population[0][ 5] - population[0][ 6];
  n4p = population[0][ 7] + population[0][ 8];
  n4m = population[0][ 7] - population[0][ 8];
  n5p = population[0][ 9] + population[0][10];
  n5m = population[0][ 9] - population[0][10];
  n6p = population[0][11] + population[0][12];
  n6m = population[0][11] - population[0][12];
  n7p = population[0][13] + population[0][14];
  n7m = population[0][13] - population[0][14];
  n8p = population[0][15] + population[0][16];
  n8m = population[0][15] - population[0][16];
  n9p = population[0][17] + population[0][18];
  n9m = population[0][17] - population[0][18];
  // clang-format on

  /* mass mode */
  mode[0] = n0 + n1p + n2p + n3p + n4p + n5p + n6p + n7p + n8p + n9p;

  /* momentum modes */
  mode[1] = n1m + n4m + n5m + n6m + n7m;
  mode[2] = n2m + n4m - n5m + n8m + n9m;
  mode[3] = n3m + n6m - n7m + n8m - n9m;

  /* stress modes */
  mode[4] = -n0 + n4p + n5p + n6p + n7p + n8p + n9p;
  mode[5] = n1p - n2p + n6p + n7p - n8p - n9p;
  mode[6] = n1p + n2p - n6p - n7p - n8p - n9p - 2. * (n3p - n4p - n5p);
  mode[7] = n4p - n5p;
  mode[8] = n6p - n7p;
  mode[9] = n8p - n9p;

#ifndef OLD_FLUCT
  /* kinetic modes */
  mode[10] = -2. * n1m + n4m + n5m + n6m + n7m;
  mode[11] = -2. * n2m + n4m - n5m + n8m + n9m;
  mode[12] = -2. * n3m + n6m - n7m + n8m - n9m;
  mode[13] = n4m + n5m - n6m - n7m;
  mode[14] = n4m - n5m - n8m - n9m;
  mode[15] = n6m - n7m - n8m + n9m;
  mode[16] = n0 + n4p + n5p + n6p + n7p + n8p + n9p - 2. * (n1p + n2p + n3p);
  mode[17] = -n1p + n2p + n6p + n7p - n8p - n9p;
  mode[18] = -n1p - n2p - n6p - n7p - n8p - n9p + 2. * (n3p + n4p + n5p);
#endif // !OLD_FLUCT

#else  // D3Q19
  int i, j;
  for (i = 0; i < lbmodel.n_veloc; i++) {
    mode[i] = 0.0;
    for (j = 0; j < lbmodel.n_veloc; j++) {
      mode[i] += lbmodel.e[i][j] * lbfluid[0][i][index];
    }
  }
#endif // D3Q19

  return 0;
}

int lbadapt_relax_modes(double *mode, double *force, double h) {
  double rho, j[3], pi_eq[6];

  /* re-construct the real density
   * remember that the populations are stored as differences to their
   * equilibrium value */
  rho = mode[0] + lbpar.rho[0] * h * h * h;

  j[0] = mode[1];
  j[1] = mode[2];
  j[2] = mode[3];

/* if forces are present, the momentum density is redefined to
 * include one half-step of the force action.  See the
 * Chapman-Enskog expansion in [Ladd & Verberg]. */
#ifndef EXTERNAL_FORCES
  if (lbfields[index].has_force)
#endif // !EXTERNAL_FORCES
  {
    j[0] += 0.5 * force[0];
    j[1] += 0.5 * force[1];
    j[2] += 0.5 * force[2];
  }

  /* equilibrium part of the stress modes */
  pi_eq[0] = scalar(j, j) / rho;
  pi_eq[1] = (SQR(j[0]) - SQR(j[1])) / rho;
  pi_eq[2] = (scalar(j, j) - 3.0 * SQR(j[2])) / rho;
  pi_eq[3] = j[0] * j[1] / rho;
  pi_eq[4] = j[0] * j[2] / rho;
  pi_eq[5] = j[1] * j[2] / rho;

  /* relax the stress modes */
  // clang-format off
  mode[4] = pi_eq[0] + gamma_bulk  * (mode[4] - pi_eq[0]);
  mode[5] = pi_eq[1] + gamma_shear * (mode[5] - pi_eq[1]);
  mode[6] = pi_eq[2] + gamma_shear * (mode[6] - pi_eq[2]);
  mode[7] = pi_eq[3] + gamma_shear * (mode[7] - pi_eq[3]);
  mode[8] = pi_eq[4] + gamma_shear * (mode[8] - pi_eq[4]);
  mode[9] = pi_eq[5] + gamma_shear * (mode[9] - pi_eq[5]);
// clang-format on

#ifndef OLD_FLUCT
  /* relax the ghost modes (project them out) */
  /* ghost modes have no equilibrium part due to orthogonality */
  // clang-format off
  mode[10] = gamma_odd  * mode[10];
  mode[11] = gamma_odd  * mode[11];
  mode[12] = gamma_odd  * mode[12];
  mode[13] = gamma_odd  * mode[13];
  mode[14] = gamma_odd  * mode[14];
  mode[15] = gamma_odd  * mode[15];
  mode[16] = gamma_even * mode[16];
  mode[17] = gamma_even * mode[17];
  mode[18] = gamma_even * mode[18];
// clang-format on
#endif // !OLD_FLUCT

  return 0;
}

int lbadapt_thermalize_modes(double *mode, double h) {
  double fluct[6];
#ifdef GAUSSRANDOM
  double rootrho_gauss = sqrt(fabs(mode[0] + lbpar.rho[0] * h * h * h));

  /* stress modes */
  mode[4] += (fluct[0] = rootrho_gauss * lb_phi[4] * gaussian_random());
  mode[5] += (fluct[1] = rootrho_gauss * lb_phi[5] * gaussian_random());
  mode[6] += (fluct[2] = rootrho_gauss * lb_phi[6] * gaussian_random());
  mode[7] += (fluct[3] = rootrho_gauss * lb_phi[7] * gaussian_random());
  mode[8] += (fluct[4] = rootrho_gauss * lb_phi[8] * gaussian_random());
  mode[9] += (fluct[5] = rootrho_gauss * lb_phi[9] * gaussian_random());

#ifndef OLD_FLUCT
  /* ghost modes */
  mode[10] += rootrho_gauss * lb_phi[10] * gaussian_random();
  mode[11] += rootrho_gauss * lb_phi[11] * gaussian_random();
  mode[12] += rootrho_gauss * lb_phi[12] * gaussian_random();
  mode[13] += rootrho_gauss * lb_phi[13] * gaussian_random();
  mode[14] += rootrho_gauss * lb_phi[14] * gaussian_random();
  mode[15] += rootrho_gauss * lb_phi[15] * gaussian_random();
  mode[16] += rootrho_gauss * lb_phi[16] * gaussian_random();
  mode[17] += rootrho_gauss * lb_phi[17] * gaussian_random();
  mode[18] += rootrho_gauss * lb_phi[18] * gaussian_random();
#endif // !OLD_FLUCT

#elif defined(GAUSSRANDOMCUT)
  double rootrho_gauss = sqrt(fabs(mode[0] + lbpar.rho[0] * h * h * h));

  /* stress modes */
  mode[4] += (fluct[0] = rootrho_gauss * lb_phi[4] * gaussian_random_cut());
  mode[5] += (fluct[1] = rootrho_gauss * lb_phi[5] * gaussian_random_cut());
  mode[6] += (fluct[2] = rootrho_gauss * lb_phi[6] * gaussian_random_cut());
  mode[7] += (fluct[3] = rootrho_gauss * lb_phi[7] * gaussian_random_cut());
  mode[8] += (fluct[4] = rootrho_gauss * lb_phi[8] * gaussian_random_cut());
  mode[9] += (fluct[5] = rootrho_gauss * lb_phi[9] * gaussian_random_cut());

#ifndef OLD_FLUCT
  /* ghost modes */
  mode[10] += rootrho_gauss * lb_phi[10] * gaussian_random_cut();
  mode[11] += rootrho_gauss * lb_phi[11] * gaussian_random_cut();
  mode[12] += rootrho_gauss * lb_phi[12] * gaussian_random_cut();
  mode[13] += rootrho_gauss * lb_phi[13] * gaussian_random_cut();
  mode[14] += rootrho_gauss * lb_phi[14] * gaussian_random_cut();
  mode[15] += rootrho_gauss * lb_phi[15] * gaussian_random_cut();
  mode[16] += rootrho_gauss * lb_phi[16] * gaussian_random_cut();
  mode[17] += rootrho_gauss * lb_phi[17] * gaussian_random_cut();
  mode[18] += rootrho_gauss * lb_phi[18] * gaussian_random_cut();
#endif // OLD_FLUCT

#elif defined(FLATNOISE)
  double rootrho = sqrt(fabs(12.0 * (mode[0] + lbpar.rho[0] * h * h * h)));

  /* stress modes */
  mode[4] += (fluct[0] = rootrho * lb_phi[4] * (d_random() - 0.5));
  mode[5] += (fluct[1] = rootrho * lb_phi[5] * (d_random() - 0.5));
  mode[6] += (fluct[2] = rootrho * lb_phi[6] * (d_random() - 0.5));
  mode[7] += (fluct[3] = rootrho * lb_phi[7] * (d_random() - 0.5));
  mode[8] += (fluct[4] = rootrho * lb_phi[8] * (d_random() - 0.5));
  mode[9] += (fluct[5] = rootrho * lb_phi[9] * (d_random() - 0.5));

#ifndef OLD_FLUCT
  /* ghost modes */
  mode[10] += rootrho * lb_phi[10] * (d_random() - 0.5);
  mode[11] += rootrho * lb_phi[11] * (d_random() - 0.5);
  mode[12] += rootrho * lb_phi[12] * (d_random() - 0.5);
  mode[13] += rootrho * lb_phi[13] * (d_random() - 0.5);
  mode[14] += rootrho * lb_phi[14] * (d_random() - 0.5);
  mode[15] += rootrho * lb_phi[15] * (d_random() - 0.5);
  mode[16] += rootrho * lb_phi[16] * (d_random() - 0.5);
  mode[17] += rootrho * lb_phi[17] * (d_random() - 0.5);
  mode[18] += rootrho * lb_phi[18] * (d_random() - 0.5);
#endif // !OLD_FLUCT
#else  // GAUSSRANDOM
#error No noise type defined for the CPU LB
#endif // GAUSSRANDOM

#ifdef ADDITIONAL_CHECKS
  rancounter += 15;
#endif // ADDITIONAL_CHECKS

  return 0;
}

int lbadapt_apply_forces(double *mode, LB_FluidNode *lbfields, double h) {
  double rho, u[3], C[6], *f;

  f = lbfields->force;

  rho = mode[0] + lbpar.rho[0] * h * h * h;

  /* hydrodynamic momentum density is redefined when external forces present
   */
  u[0] = (mode[1] + 0.5 * f[0]) / rho;
  u[1] = (mode[2] + 0.5 * f[1]) / rho;
  u[2] = (mode[3] + 0.5 * f[2]) / rho;

  C[0] = (1. + gamma_bulk) * u[0] * f[0] +
         1. / 3. * (gamma_bulk - gamma_shear) * scalar(u, f);
  C[2] = (1. + gamma_bulk) * u[1] * f[1] +
         1. / 3. * (gamma_bulk - gamma_shear) * scalar(u, f);
  C[5] = (1. + gamma_bulk) * u[2] * f[2] +
         1. / 3. * (gamma_bulk - gamma_shear) * scalar(u, f);
  C[1] = 0.5 * (1. + gamma_shear) * (u[0] * f[1] + u[1] * f[0]);
  C[3] = 0.5 * (1. + gamma_shear) * (u[0] * f[2] + u[2] * f[0]);
  C[4] = 0.5 * (1. + gamma_shear) * (u[1] * f[2] + u[2] * f[1]);

  /* update momentum modes */
  mode[1] += f[0];
  mode[2] += f[1];
  mode[3] += f[2];

  /* update stress modes */
  mode[4] += C[0] + C[2] + C[5];
  mode[5] += C[0] - C[2];
  mode[6] += C[0] + C[2] - 2. * C[5];
  mode[7] += C[1];
  mode[8] += C[3];
  mode[9] += C[4];

/* reset force */
#ifdef EXTERNAL_FORCES
  // unit conversion: force density
  lbfields->force[0] = lbpar.ext_force[0] * SQR(h) * SQR(lbpar.tau);
  lbfields->force[1] = lbpar.ext_force[1] * SQR(h) * SQR(lbpar.tau);
  lbfields->force[2] = lbpar.ext_force[2] * SQR(h) * SQR(lbpar.tau);
#else  // EXTERNAL_FORCES
  lbfields->force[0] = 0.0;
  lbfields->force[1] = 0.0;
  lbfields->force[2] = 0.0;
  lbfields->has_force = 0;
#endif // EXTERNAL_FORCES

  return 0;
}

double lbadapt_backTransformation(double *m, int dir) {
  switch (dir) {
  case 0:
    return m[0] - m[4] + m[16];
  case 1:
    return m[0] + m[1] + m[5] + m[6] - m[17] - m[18] - 2. * (m[10] + m[16]);
  case 2:
    return m[0] - m[1] + m[5] + m[6] - m[17] - m[18] + 2. * (m[10] - m[16]);
  case 3:
    return m[0] + m[2] - m[5] + m[6] + m[17] - m[18] - 2. * (m[11] + m[16]);
  case 4:
    return m[0] - m[2] - m[5] + m[6] + m[17] - m[18] + 2. * (m[11] - m[16]);
  case 5:
    return m[0] + m[3] - 2. * (m[6] + m[12] + m[16] - m[18]);
  case 6:
    return m[0] - m[3] - 2. * (m[6] - m[12] + m[16] - m[18]);
  case 7:
    return m[0] + m[1] + m[2] + m[4] + 2. * m[6] + m[7] + m[10] + m[11] +
           m[13] + m[14] + m[16] + 2. * m[18];
  case 8:
    return m[0] - m[1] - m[2] + m[4] + 2. * m[6] + m[7] - m[10] - m[11] -
           m[13] - m[14] + m[16] + 2. * m[18];
  case 9:
    return m[0] + m[1] - m[2] + m[4] + 2. * m[6] - m[7] + m[10] - m[11] +
           m[13] - m[14] + m[16] + 2. * m[18];
  case 10:
    return m[0] - m[1] + m[2] + m[4] + 2. * m[6] - m[7] - m[10] + m[11] -
           m[13] + m[14] + m[16] + 2. * m[18];
  case 11:
    return m[0] + m[1] + m[3] + m[4] + m[5] - m[6] + m[8] + m[10] + m[12] -
           m[13] + m[15] + m[16] + m[17] - m[18];
  case 12:
    return m[0] - m[1] - m[3] + m[4] + m[5] - m[6] + m[8] - m[10] - m[12] +
           m[13] - m[15] + m[16] + m[17] - m[18];
  case 13:
    return m[0] + m[1] - m[3] + m[4] + m[5] - m[6] - m[8] + m[10] - m[12] -
           m[13] - m[15] + m[16] + m[17] - m[18];
  case 14:
    return m[0] - m[1] + m[3] + m[4] + m[5] - m[6] - m[8] - m[10] + m[12] +
           m[13] + m[15] + m[16] + m[17] - m[18];
  case 15:
    return m[0] + m[2] + m[3] + m[4] - m[5] - m[6] + m[9] + m[11] + m[12] -
           m[14] - m[15] + m[16] - m[17] - m[18];
  case 16:
    return m[0] - m[2] - m[3] + m[4] - m[5] - m[6] + m[9] - m[11] - m[12] +
           m[14] + m[15] + m[16] - m[17] - m[18];
  case 17:
    return m[0] + m[2] - m[3] + m[4] - m[5] - m[6] - m[9] + m[11] - m[12] -
           m[14] + m[15] + m[16] - m[17] - m[18];
  case 18:
    return m[0] - m[2] + m[3] + m[4] - m[5] - m[6] - m[9] - m[11] + m[12] +
           m[14] - m[15] + m[16] - m[17] - m[18];
  default:
    SC_ABORT_NOT_REACHED();
  }
}

int lbadapt_calc_n_from_modes_push(p8est_meshiter_t *mesh_iter) {
#ifdef D3Q19
  /* index of inverse velocity vector, 0 is inverse to itself. */
  // clang-format off
  int inv[] = {0,
               2,  1,  4,  3,  6,  5,
               8,  7, 10,  9, 12, 11, 14, 13, 16, 15, 18, 17};
  // clang-format on

  /**************************************************/
  /* process current cell (backtransformation only) */
  /**************************************************/
  /* containers for accessing data of current cell through p4est_mesh */
  lbadapt_payload_t *data, *currCellData;

  currCellData =
      &lbadapt_local_data[mesh_iter->current_level]
                         [p8est_meshiter_get_current_storage_id(mesh_iter)];
  double *m = currCellData->modes;
  double ghost_m[19];

  /* normalization factors enter in the back transformation */
  for (int i = 0; i < lbmodel.n_veloc; i++) {
    m[i] = (1. / d3q19_modebase[19][i]) * m[i];
  }

#ifndef OLD_FLUCT
  /* cell itself cannot be ghost cell */
  data = &lbadapt_local_data[mesh_iter->current_level]
                            [p8est_meshiter_get_current_storage_id(mesh_iter)];
  data->lbfluid[1][0] = (m[0] - m[4] + m[16]) * lbmodel.w[0];

  /*******************************/
  /* stream to surrounding cells */
  /*******************************/
  for (int dir = 1; dir < 19; ++dir) {
    // convert direction
    int direction = ci_to_p4est[(dir - 1)];
    // set neighboring cell information in iterator
    p8est_meshiter_set_neighbor_quad_info(mesh_iter, direction);

    if (mesh_iter->neighbor_qid != -1) {
      // if the neighboring cell is a ghost cell:
      // do not stream information into the ghost cell but perform the inverse
      // streaming step and stream information into the current cell
      if (mesh_iter->neighbor_is_ghost) {
        /* obtain and normalize ghost modes */
        data = &lbadapt_ghost_data[mesh_iter->current_level]
                                  [p8est_meshiter_get_neighbor_storage_id(
                                      mesh_iter)];
        for (int i = 0; i < lbmodel.n_veloc; ++i) {
          ghost_m[i] = data->modes[i];
          ghost_m[i] = (1. / d3q19_modebase[19][i]) * ghost_m[i];
        }

        currCellData->lbfluid[1][inv[dir]] =
            lbadapt_backTransformation(ghost_m, inv[dir]) * lbmodel.w[inv[dir]];
      } else {
        data = &lbadapt_local_data[mesh_iter->current_level]
                                  [p8est_meshiter_get_neighbor_storage_id(
                                      mesh_iter)];
        data->lbfluid[1][dir] =
            lbadapt_backTransformation(m, dir) * lbmodel.w[dir];
      }
    }
  }
#else //! OLD_FLUCT
#error OLD_FLUCT not implemented
#endif // !OLD_FLUCT
#endif // D3Q19
  return 0;
}

void lbadapt_collide(int level) {
  int status = 0;
  double h;
  h = (double)P8EST_QUADRANT_LEN(level) / (double)P8EST_ROOT_LEN;

  lbadapt_payload_t *data;
  p8est_meshiter_t *mesh_iter = p8est_meshiter_new_ext(
      p8est, lbadapt_ghost, lbadapt_mesh, level, P8EST_CONNECT_EDGE,
      P8EST_TRAVERSE_LOCAL, P8EST_TRAVERSE_REAL, P8EST_TRAVERSE_PARBOUNDINNER);

  while (status != P8EST_MESHITER_DONE) {
    status = p8est_meshiter_next(mesh_iter);
    if (status != P8EST_MESHITER_DONE) {
      if (!mesh_iter->current_is_ghost) {
        data = &lbadapt_local_data[level][p8est_meshiter_get_current_storage_id(
            mesh_iter)];
      } else {
        data = &lbadapt_ghost_data[level][p8est_meshiter_get_current_storage_id(
            mesh_iter)];
      }
#ifdef LB_BOUNDARIES
      if (!data->boundary)
#endif // LB_BOUNDARIES
      {
        /* place for storing modes */
        double *modes = data->modes;

        /* calculate modes locally */
        lbadapt_calc_modes(data->lbfluid, modes);

        /* deterministic collisions */
        lbadapt_relax_modes(modes, data->lbfields.force, h);

        /* fluctuating hydrodynamics */
        if (fluct)
          lbadapt_thermalize_modes(modes, h);

/* apply forces */
#ifdef EXTERNAL_FORCES
        lbadapt_apply_forces(modes, &data->lbfields, h);
#else  // EXTERNAL_FORCES
        if (data->lbfields.has_force)
          lbadapt_apply_forces(modes, &data->lbfields, h);
#endif // EXTERNAL_FORCES
      }
    }
  }
  p8est_meshiter_destroy(mesh_iter);
}

void lbadapt_populate_virtuals(int level) { ; }

void lbadapt_stream(int level) {
  int status = 0;
  lbadapt_payload_t *data;
  p8est_meshiter_t *mesh_iter = p8est_meshiter_new_ext(
      p8est, lbadapt_ghost, lbadapt_mesh, level, P8EST_CONNECT_EDGE,
      P8EST_TRAVERSE_LOCAL, P8EST_TRAVERSE_REALVIRTUAL,
      P8EST_TRAVERSE_PARBOUNDINNER);

  while (status != P8EST_MESHITER_DONE) {
    status = p8est_meshiter_next(mesh_iter);
    if (status != P8EST_MESHITER_DONE) {
      data =
          &lbadapt_local_data[level]
                             [p8est_meshiter_get_current_storage_id(mesh_iter)];
      if (!data->boundary) {
        lbadapt_calc_n_from_modes_push(mesh_iter);
      }
    }
  }
  p8est_meshiter_destroy(mesh_iter);
}

void lbadapt_bounce_back(int level) {
  int status = 0;
  lbadapt_payload_t *data, *currCellData;
  for (int level = 0; level < P8EST_MAXLEVEL; ++level) {
    double h;
    h = (double)P8EST_QUADRANT_LEN(level) / (double)P8EST_ROOT_LEN;
    p8est_meshiter_t *mesh_iter = p8est_meshiter_new_ext(
        p8est, lbadapt_ghost, lbadapt_mesh, level, P8EST_CONNECT_EDGE,
        P8EST_TRAVERSE_LOCAL, P8EST_TRAVERSE_REALVIRTUAL,
        P8EST_TRAVERSE_PARBOUNDINNER);

    while (status != P8EST_MESHITER_DONE) {
      status = p8est_meshiter_next(mesh_iter);
      if (status != P8EST_MESHITER_DONE) {
        currCellData =
            &lbadapt_local_data[level][p8est_meshiter_get_current_storage_id(
                mesh_iter)];

#ifdef D3Q19
#ifndef PULL
        double population_shift;

        /* vector of inverse c_i, 0 is inverse to itself. */
        // clang-format off
        int reverse[] = {0,
                         2,  1,  4,  3,  6,  5,
                         8,  7, 10,  9, 12, 11, 14, 13, 16, 15, 18, 17};
        // clang-format on

        // We cannot copy this with minimal invasiveness, because boundary cells
        // can
        // end in the ghost layer and p4est_iterate does not visit ghost cells.
        // Therefore we have to inspect in each cell, if it has neighbors that
        // are
        // part of the ghost layer.
        // Then there are 2 cases where we have to bounce back:
        // 1. Current cell itself is boundary cell
        //    In this case we perform the same algorithm as in the regular grid
        // 2. Current cell is fluid cell and has ghost neighbors that are
        // boundary
        //    cells.
        //    In this case we do the opposite of the above algorithm:
        //    Instead of bouncing back from the current to the neighboring cell
        //    we
        //    bounce back from the neighboring ghost cell into the current cell.
        for (int i = 0; i < 19; ++i) {
          // fetch neighboring quadrant's payload
          if (i == 0) {
            data = currCellData;
          } else {
            // convert direction
            int direction = ci_to_p4est[i - 1];
            // get neighboring cells
            p8est_meshiter_set_neighbor_quad_info(mesh_iter, direction);
            if (mesh_iter->neighbor_is_ghost) {
              data = &lbadapt_ghost_data[mesh_iter->current_level]
                                       [p8est_meshiter_get_neighbor_storage_id(
                                           mesh_iter)];
            } else {
              data = &lbadapt_local_data[mesh_iter->current_level]
                                       [p8est_meshiter_get_neighbor_storage_id(
                                           mesh_iter)];
            }
          }

          // case 1
          if (!mesh_iter->neighbor_is_ghost && currCellData->boundary) {
            // lbadapt_calc_modes(currCellData->lbfluid, modes);
            // calculate population shift (moving boundary)
            population_shift = 0;
            for (int l = 0; l < 3; l++) {
              population_shift -=
                  h * h * h * h * h * lbpar.rho[0] * 2 * lbmodel.c[i][l] *
                  lbmodel.w[i] *
                  lb_boundaries[currCellData->boundary - 1].velocity[l] /
                  lbmodel.c_sound_sq;
            }

            if (!data->boundary) {
              for (int l = 0; l < 3; l++) {
                lb_boundaries[currCellData->boundary - 1].force[l] +=
                    (2 * currCellData->lbfluid[1][i] + population_shift) *
                    lbmodel.c[i][l];
              }
              data->lbfluid[1][reverse[i]] =
                  currCellData->lbfluid[1][i] + population_shift;
            } else {
              // else bounce back
              data->lbfluid[1][reverse[i]] = currCellData->lbfluid[1][i] = 0.0;
            }
          }

          // case 2
          else if (mesh_iter->neighbor_is_ghost && data->boundary) {
            // lbadapt_calc_modes(data->lbfluid, ghost_modes);
            // calculate population shift (moving boundary)
            population_shift = 0;
            for (int l = 0; l < 3; l++) {
              population_shift -=
                  h * h * h * h * h * lbpar.rho[0] * 2 *
                  lbmodel.c[reverse[i]][l] * lbmodel.w[reverse[i]] *
                  lb_boundaries[data->boundary - 1].velocity[l] /
                  lbmodel.c_sound_sq;
            }

            if (!currCellData->boundary) {
              for (int l = 0; l < 3; l++) {
                lb_boundaries[data->boundary - 1].force[l] +=
                    (2 * data->lbfluid[1][reverse[i]] + population_shift) *
                    lbmodel.c[reverse[i]][l];
              }
              currCellData->lbfluid[1][i] =
                  data->lbfluid[1][reverse[i]] + population_shift;
            } else {
              // else bounce back
              currCellData->lbfluid[1][i] = data->lbfluid[1][reverse[i]] = 0.0;
            }
          }
        }
#else // !PULL
#error Bounce back boundary conditions are only implemented for PUSH scheme!
#endif // !PULL
#else  // D3Q19
#error Bounce back boundary conditions are only implemented for D3Q19!
#endif // D3Q19
      }
    }
    p8est_meshiter_destroy(mesh_iter);
  }
}

void lbadapt_update_populations_from_virtuals(int level) { ; }

void lbadapt_swap_pointers(int level) {
  int status = 0;
  lbadapt_payload_t *data;
  for (int level = 0; level < P8EST_MAXLEVEL; ++level) {
    p8est_meshiter_t *mesh_iter = p8est_meshiter_new_ext(
        p8est, lbadapt_ghost, lbadapt_mesh, level, P8EST_CONNECT_EDGE,
        P8EST_TRAVERSE_LOCAL, P8EST_TRAVERSE_REALVIRTUAL,
        P8EST_TRAVERSE_PARBOUNDINNER);

    while (status != P8EST_MESHITER_DONE) {
      status = p8est_meshiter_next(mesh_iter);
      if (status != P8EST_MESHITER_DONE) {
        if (!mesh_iter->current_is_ghost) {
          data =
              &lbadapt_local_data[level][p8est_meshiter_get_current_storage_id(
                  mesh_iter)];
        } else {
          data =
              &lbadapt_ghost_data[level][p8est_meshiter_get_current_storage_id(
                  mesh_iter)];
        }
        std::swap(data->lbfluid[0], data->lbfluid[1]);
      }
    }
    p8est_meshiter_destroy(mesh_iter);
  }
}

/*** ITERATOR CALLBACKS ***/
void lbadapt_get_boundary_status(p8est_iter_volume_info_t *info,
                                 void *user_data) {
  p8est_quadrant_t *q = info->quad;         /* get current global cell id */
  p4est_topidx_t which_tree = info->treeid; /* get current tree id */
  lbadapt_payload_t *data =
      (lbadapt_payload_t *)q->p.user_data; /* payload of cell */

  double midpoint[3];
  lbadapt_get_midpoint(p8est, which_tree, q, midpoint);

  data->boundary = lbadapt_is_boundary(midpoint);
}

void lbadapt_get_boundary_values(p8est_iter_volume_info_t *info,
                                 void *user_data) {
  sc_array_t *bnd_vals = (sc_array_t *)user_data; /* passed array to fill */
  p8est_quadrant_t *q = info->quad;         /* get current global cell id */
  p4est_topidx_t which_tree = info->treeid; /* get current tree id */
  p4est_locidx_t local_id = info->quadid;   /* get cell id w.r.t. tree-id */
  p8est_tree_t *tree;
  lbadapt_payload_t *data =
      (lbadapt_payload_t *)q->p.user_data; /* payload of cell */

  p4est_locidx_t arrayoffset;

  tree = p8est_tree_array_index(p8est->trees, which_tree);
  local_id += tree->quadrants_offset; /* now the id is relative to the MPI
                                         process */
  arrayoffset = local_id;

  /* just grab the value of each cell and pass it into solution vector */
  double bnd = data->boundary;
  double *bnd_ptr = (double *)sc_array_index(bnd_vals, arrayoffset);
  *bnd_ptr = bnd;
}

void lbadapt_get_boundary_values_dirty(p8est_iter_volume_info_t *info,
                                       void *user_data) {
  double *bnd_vals = (double *)user_data;   /* passed array to fill */
  p8est_quadrant_t *q = info->quad;         /* get current global cell id */
  p4est_topidx_t which_tree = info->treeid; /* get current tree id */
  p4est_locidx_t local_id = info->quadid;   /* get cell id w.r.t. tree-id */
  p8est_tree_t *tree;
  lbadapt_payload_t *data =
      (lbadapt_payload_t *)q->p.user_data; /* payload of cell */

  p4est_locidx_t arrayoffset;

  tree = p8est_tree_array_index(p8est->trees, which_tree);
  local_id += tree->quadrants_offset; /* now the id is relative to the MPI
                                         process */
  arrayoffset = local_id;

  /* just grab the value of each cell and pass it into solution vector */
  int bnd = data->boundary;
  bnd_vals[arrayoffset] = bnd;
}

void lbadapt_get_density_values(p8est_iter_volume_info_t *info,
                                void *user_data) {
  sc_array_t *dens_vals = (sc_array_t *)user_data; /* passed array to fill */
  p8est_quadrant_t *q = info->quad;         /* get current global cell id */
  p4est_topidx_t which_tree = info->treeid; /* get current tree id */
  p4est_locidx_t local_id = info->quadid;   /* get cell id w.r.t. tree-id */
  p8est_tree_t *tree;
  lbadapt_payload_t *data =
      (lbadapt_payload_t *)q->p.user_data; /* payload of cell */
  double h;                                /* local meshwidth */
  h = (double)P8EST_QUADRANT_LEN(q->level) / (double)P8EST_ROOT_LEN;

  double dens;
  p4est_locidx_t arrayoffset;

  tree = p8est_tree_array_index(p8est->trees, which_tree);
  local_id += tree->quadrants_offset; /* now the id is relative to the MPI
                                         process */
  arrayoffset =
      local_id; /* each local quadrant has 2^d (P4EST_CHILDREN) values in
                   u_interp */

  /* just grab the value of each cell and pass it into solution vector */
  double avg_rho = lbpar.rho[0] * h * h * h;

  if (data->boundary) {
    dens = 0;
  } else {
    // clang-format off
    dens = avg_rho
         + data->lbfluid[0][ 0] + data->lbfluid[0][ 1] + data->lbfluid[0][ 2]
         + data->lbfluid[0][ 3] + data->lbfluid[0][ 4] + data->lbfluid[0][ 5]
         + data->lbfluid[0][ 6] + data->lbfluid[0][ 7] + data->lbfluid[0][ 8]
         + data->lbfluid[0][ 9] + data->lbfluid[0][10] + data->lbfluid[0][11]
         + data->lbfluid[0][12] + data->lbfluid[0][13] + data->lbfluid[0][14]
         + data->lbfluid[0][15] + data->lbfluid[0][16] + data->lbfluid[0][17]
         + data->lbfluid[0][18];
    // clang-format on
  }
  double *dens_ptr = (double *)sc_array_index_int(dens_vals, arrayoffset);
  *dens_ptr = dens;
}

void lbadapt_get_density_values_dirty(p8est_iter_volume_info_t *info,
                                      void *user_data) {
  double *dens_vals = (double *)user_data;  /* passed array to fill */
  p8est_quadrant_t *q = info->quad;         /* get current global cell id */
  p4est_topidx_t which_tree = info->treeid; /* get current tree id */
  p4est_locidx_t local_id = info->quadid;   /* get cell id w.r.t. tree-id */
  p8est_tree_t *tree;
  lbadapt_payload_t *data =
      (lbadapt_payload_t *)q->p.user_data; /* payload of cell */
  double h;                                /* local meshwidth */
  h = (double)P8EST_QUADRANT_LEN(q->level) / (double)P8EST_ROOT_LEN;

  double dens;
  p4est_locidx_t arrayoffset;

  tree = p8est_tree_array_index(p8est->trees, which_tree);
  local_id += tree->quadrants_offset; /* now the id is relative to the MPI
                                         process */
  arrayoffset =
      local_id; /* each local quadrant has 2^d (P4EST_CHILDREN) values in
                   u_interp */

  /* just grab the value of each cell and pass it into solution vector */
  double avg_rho = lbpar.rho[0] * h * h * h;

  if (data->boundary) {
    dens = 0;
  } else {
    // clang-format off
    dens = avg_rho
         + data->lbfluid[0][ 0] + data->lbfluid[0][ 1] + data->lbfluid[0][ 2]
         + data->lbfluid[0][ 3] + data->lbfluid[0][ 4] + data->lbfluid[0][ 5]
         + data->lbfluid[0][ 6] + data->lbfluid[0][ 7] + data->lbfluid[0][ 8]
         + data->lbfluid[0][ 9] + data->lbfluid[0][10] + data->lbfluid[0][11]
         + data->lbfluid[0][12] + data->lbfluid[0][13] + data->lbfluid[0][14]
         + data->lbfluid[0][15] + data->lbfluid[0][16] + data->lbfluid[0][17]
         + data->lbfluid[0][18];
    // clang-format on
  }
  dens_vals[arrayoffset] = dens;
}

void lbadapt_get_velocity_values(p8est_iter_volume_info_t *info,
                                 void *user_data) {
  sc_array_t *veloc_vals = (sc_array_t *)user_data; /* passed array to fill */
  p8est_quadrant_t *q = info->quad;         /* get current global cell id */
  p4est_topidx_t which_tree = info->treeid; /* get current tree id */
  p4est_locidx_t local_id = info->quadid;   /* get cell id w.r.t. tree-id */
  p8est_tree_t *tree;
  lbadapt_payload_t *data =
      (lbadapt_payload_t *)q->p.user_data; /* payload of cell */
  double h;                                /* local meshwidth */
  h = (double)P8EST_QUADRANT_LEN(q->level) / (double)P8EST_ROOT_LEN;

  /* calculate position to write to */
  p4est_locidx_t arrayoffset;
  tree = p8est_tree_array_index(p8est->trees, which_tree);
  local_id += tree->quadrants_offset; /* now the id is relative to the MPI
                                         process */
  arrayoffset =
      P8EST_DIM * local_id; /* each local quadrant has 3 values in u_interp */

  /* calculate values to write */
  double rho;
  double j[3];
  lbadapt_calc_local_fields(data->modes, data->lbfields.force, data->boundary,
                            data->lbfields.has_force, h, &rho, j, NULL);

  double *veloc_ptr = (double *)sc_array_index_int(veloc_vals, arrayoffset);

  /* pass it into solution vector */
  std::memcpy(veloc_ptr, j, 3 * sizeof(double));
}

void lbadapt_get_velocity_values_dirty(p8est_iter_volume_info_t *info,
                                       void *user_data) {
  double *veloc_vals = (double *)user_data; /* passed array to fill */
  p8est_quadrant_t *q = info->quad;         /* get current global cell id */
  p4est_topidx_t which_tree = info->treeid; /* get current tree id */
  p4est_locidx_t local_id = info->quadid;   /* get cell id w.r.t. tree-id */
  p8est_tree_t *tree;
  lbadapt_payload_t *data =
      (lbadapt_payload_t *)q->p.user_data; /* payload of cell */
  double h;                                /* local meshwidth */
  h = (double)P8EST_QUADRANT_LEN(q->level) / (double)P8EST_ROOT_LEN;

  /* calculate position to write to */
  p4est_locidx_t arrayoffset;
  tree = p8est_tree_array_index(p8est->trees, which_tree);
  local_id += tree->quadrants_offset; /* now the id is relative to the MPI
                                         process */
  arrayoffset = P8EST_DIM * local_id; /* each local quadrant has 3 values in */

  /* calculate values to write */
  double rho;
  double j[3];
  lbadapt_calc_local_fields(data->modes, data->lbfields.force, data->boundary,
                            data->lbfields.has_force, h, &rho, j, NULL);

  /* pass it into solution vector */
  // clang-format off
  veloc_vals[arrayoffset]     = j[0] / rho * h / lbpar.tau;
  veloc_vals[arrayoffset + 1] = j[1] / rho * h / lbpar.tau;
  veloc_vals[arrayoffset + 2] = j[2] / rho * h / lbpar.tau;
  // clang-format on
}

void lbadapt_set_recalc_fields(p8est_iter_volume_info_t *info,
                               void *user_data) {
  p8est_quadrant_t *q = info->quad;
  lbadapt_payload_t *data = (lbadapt_payload_t *)q->p.user_data;

  data->lbfields.recalc_fields = 1;
}

void lbadapt_calc_local_rho(p8est_iter_volume_info_t *info, void *user_data) {
  double *rho = (double *)user_data; /* passed double to fill */
  p8est_quadrant_t *q = info->quad;  /* get current global cell id */
  lbadapt_payload_t *data =
      (lbadapt_payload_t *)q->p.user_data; /* payload of cell */
  double h;                                /* local meshwidth */
  h = (double)P8EST_QUADRANT_LEN(q->level) / (double)P8EST_ROOT_LEN;

#ifndef D3Q19
#error Only D3Q19 is implemened!
#endif // D3Q19

  // unit conversion: mass density
  if (!(lattice_switch & LATTICE_LB)) {
    runtimeErrorMsg() << "Error in lb_calc_local_rho in " << __FILE__
                      << __LINE__ << ": CPU LB not switched on.";
    *rho = 0;
    return;
  }

  double avg_rho = lbpar.rho[0] * h * h * h;

  // clang-format off
  *rho += avg_rho +
          data->lbfluid[0][ 0] + data->lbfluid[0][ 1] + data->lbfluid[0][ 2] +
          data->lbfluid[0][ 3] + data->lbfluid[0][ 4] + data->lbfluid[0][ 5] +
          data->lbfluid[0][ 6] + data->lbfluid[0][ 7] + data->lbfluid[0][ 8] +
          data->lbfluid[0][ 9] + data->lbfluid[0][10] + data->lbfluid[0][11] +
          data->lbfluid[0][12] + data->lbfluid[0][13] + data->lbfluid[0][14] +
          data->lbfluid[0][15] + data->lbfluid[0][16] + data->lbfluid[0][17] +
          data->lbfluid[0][18];
  // clang-format on
}

void lbadapt_calc_local_j(p8est_iter_volume_info_t *info, void *user_data) {
  double *momentum = (double *)user_data; /* passed array to fill */
  p8est_quadrant_t *q = info->quad;       /* get current global cell id */
  lbadapt_payload_t *data =
      (lbadapt_payload_t *)q->p.user_data; /* payload of cell */
  double h;                                /* local meshwidth */
  h = (double)P8EST_QUADRANT_LEN(q->level) / (double)P8EST_ROOT_LEN;

  double j[3];

#ifndef D3Q19
#error Only D3Q19 is implemened!
#endif // D3Q19
  if (!(lattice_switch & LATTICE_LB)) {
    runtimeErrorMsg() << "Error in lb_calc_local_j in " << __FILE__ << __LINE__
                      << ": CPU LB not switched on.";
    j[0] = j[1] = j[2] = 0;
    return;
  }

  // clang-format off
  j[0] = data->lbfluid[0][ 1] - data->lbfluid[0][ 2] + data->lbfluid[0][ 7] -
         data->lbfluid[0][ 8] + data->lbfluid[0][ 9] - data->lbfluid[0][10] +
         data->lbfluid[0][11] - data->lbfluid[0][12] + data->lbfluid[0][13] -
         data->lbfluid[0][14];
  j[1] = data->lbfluid[0][ 3] - data->lbfluid[0][ 4] + data->lbfluid[0][ 7] -
         data->lbfluid[0][ 8] - data->lbfluid[0][ 9] + data->lbfluid[0][10] +
         data->lbfluid[0][15] - data->lbfluid[0][16] + data->lbfluid[0][17] -
         data->lbfluid[0][18];
  j[2] = data->lbfluid[0][ 5] - data->lbfluid[0][ 6] + data->lbfluid[0][11] -
         data->lbfluid[0][12] - data->lbfluid[0][13] + data->lbfluid[0][14] +
         data->lbfluid[0][15] - data->lbfluid[0][16] - data->lbfluid[0][17] +
         data->lbfluid[0][18];
  // clang-format on
  momentum[0] += j[0] + data->lbfields.force[0];
  momentum[1] += j[1] + data->lbfields.force[1];
  momentum[2] += j[2] + data->lbfields.force[2];

  momentum[0] *= h / lbpar.tau;
  momentum[1] *= h / lbpar.tau;
  momentum[2] *= h / lbpar.tau;
}

void lbadapt_calc_local_pi(p8est_iter_volume_info_t *info, void *user_data) {
  double *bnd_vals = (double *)user_data;   /* passed array to fill */
  p8est_quadrant_t *q = info->quad;         /* get current global cell id */
  p4est_topidx_t which_tree = info->treeid; /* get current tree id */
  p4est_locidx_t local_id = info->quadid;   /* get cell id w.r.t. tree-id */
  p8est_tree_t *tree;
  lbadapt_payload_t *data =
      (lbadapt_payload_t *)q->p.user_data; /* payload of cell */

  double bnd; /* local meshwidth */
  p4est_locidx_t arrayoffset;

  tree = p8est_tree_array_index(p8est->trees, which_tree);
  local_id += tree->quadrants_offset; /* now the id is relative to the MPI
                                         process */
  arrayoffset =
      local_id; /* each local quadrant has 2^d (P4EST_CHILDREN) values in
                   u_interp */

  /* just grab the u value of each cell and pass it into solution vector
   */
  bnd = data->boundary;
  bnd_vals[arrayoffset] = bnd;
}

void lbadapt_dump2file(p8est_iter_volume_info_t *info, void *user_data) {
  lbadapt_payload_t *data = (lbadapt_payload_t *)info->quad->p.user_data;
  p8est_quadrant_t *q = info->quad;

  std::string *filename = (std::string *)user_data;
  std::ofstream myfile;
  myfile.open(*filename, std::ofstream::out | std::ofstream::app);
  myfile << "id: " << info->quadid
         << "; coords: " << (q->x / (1 << (P8EST_MAXLEVEL - q->level))) << ", "
         << (q->y / (1 << (P8EST_MAXLEVEL - q->level))) << ", "
         << (q->z / (1 << (P8EST_MAXLEVEL - q->level)))
         << "; boundary: " << data->boundary << std::endl
         << " - distributions: pre streaming: ";
  for (int i = 0; i < 19; ++i) {
    myfile << data->lbfluid[0][i] << " - ";
  }
  myfile << std::endl << "post streaming: ";
  for (int i = 0; i < 19; ++i) {
    myfile << data->lbfluid[1][i] << " - ";
  }
  myfile << std::endl << "modes: ";
  for (int i = 0; i < 19; ++i) {
    myfile << data->modes[i] << " - ";
  }
  myfile << std::endl << std::endl;

  myfile.flush();
  myfile.close();
}

#endif // LB_ADAPTIVE
