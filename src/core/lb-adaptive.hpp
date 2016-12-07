/*
  Copyright (C) 2010,2011,2012,2013,2014,2015 The ESPResSo project
  Copyright (C) 2002,2003,2004,2005,2006,2007,2008,2009,2010
  Max-Planck-Institute for Polymer Research, Theory Group

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
/** \file lb-adaptive.hpp
 *
 * Adaptive Lattice Boltzmann Scheme.
 * Header file for \ref lb-adaptive.cpp.
 *
 */

#ifndef _LB_ADAPTIVE_H
#define _LB_ADAPTIVE_H

#ifdef LB_ADAPTIVE
/* p4est includes; opted to go for pure 3D */
#include <p8est.h>
#include <p8est_connectivity.h>
#include <p8est_extended.h>
#include <p8est_ghost.h>
#include <p8est_ghostvirt.h>
#include <p8est_iterate.h>
#include <p8est_mesh.h>
#include <p8est_meshiter.h>
#include <p8est_nodes.h>
#include <p8est_vtk.h>

#include "lb.hpp"
#include "lb-adaptive-gpu.hpp"

/* "global variables" */
extern p8est_t *p8est;
extern p8est_connectivity_t *conn;
extern p8est_ghost_t *lbadapt_ghost;
extern p8est_ghostvirt_t *lbadapt_ghost_virt;
extern p8est_mesh_t *lbadapt_mesh;
extern lbadapt_payload_t **lbadapt_local_data;
extern lbadapt_payload_t **lbadapt_ghost_data;
extern int coarsest_level_local;
extern int finest_level_local;
extern int coarsest_level_ghost;
extern int finest_level_ghost;
extern int finest_level_global;

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

/** setup function for lbadapt_payload_t by setting as many values as possible
 *  to 0
 */
void lbadapt_init();

/** function to deallocate fluid storage
 */
void lbadapt_release();

/** reinitialize lb-parameters from user input
 */
void lbadapt_reinit_parameters();

/** Init cell-local force values
 */
void lbadapt_reinit_force_per_cell();

/** (Re-)initialize the fluid according to the given value of rho
 */
void lbadapt_reinit_fluid_per_cell();

/** Get maxlevel of p4est
 */
int lbadapt_get_global_maxlevel();

#ifdef LB_ADAPTIVE_GPU
/** Populate the halos of patches of a specific level
 */
void lbadapt_patches_populate_halos(int level);
#endif // LB_ADAPTIVE_GPU

/** interpolating function
 *
 * \param [in]      p8est        The forest
 * \param [in]      which_tree   The tree in the forest \a q.
 * \param [in]      num_outgoing The number of quadrants being replaced.
 *                               1 for refinement, 8 for coarsening.
 * \param [in]      outgoing     The actual quadrants that will be replaced.
 * \param [in]      num_incoming The number of quadarants that will be added.
 * \param [in][out] incoming     Quadrants whose data needs to be initialized.
 */
void lbadapt_replace_quads(p8est_t *p8est, p4est_topidx_t which_tree,
                           int num_outgoing, p8est_quadrant_t *outgoing[],
                           int num_incoming, p8est_quadrant_t *incoming[]);

/*** LOAD BALANCING ***/
/** Weighting function for p4est_partition
 *
 * \param [in] p8est       The forest.
 * \param [in] which_tree  The tree in the forest containing \a q.
 * \param [in] quadrant    The Quadrant.
 * @returns quadrants weight according to subcycling
 */
int lbadapt_partition_weight(p8est_t *p8est, p4est_topidx_t which_tree,
                             p8est_quadrant_t *q);
/*** REFINEMENT ***/
/** Refinement function that refines all cells
 *
 * \param [in] p8est       The forest.
 * \param [in] which_tree  The tree in the forest containing \a q.
 * \param [in] quadrant    The Quadrant.
 */
int refine_uniform(p8est_t *p8est, p4est_topidx_t which_tree,
                   p8est_quadrant_t *quadrant);

/** Refinement function that refines all cells with probability 0.55
 *
 * \param [in] p8est       The forest.
 * \param [in] which_tree  The tree in the forest containing \a q.
 * \param [in] quadrant    The Quadrant.
 */
int refine_random(p8est_t *p8est, p4est_topidx_t which_tree,
                  p8est_quadrant_t *quadrant);

/** Refinement function that refines all cells for whichs anchor point holds
 * 0.25 <= z < 0.75
 *
 * \param [in] p8est       The forest.
 * \param [in] which_tree  The tree in the forest containing \a q.
 * \param [in] quadrant    The Quadrant.
 */
int refine_regional(p8est_t *p8est, p4est_topidx_t which_tree,
                    p8est_quadrant_t *q);

/** Refinement function that refines all cells whose midpoint is closer to a
 * boundary than half the cells side length.
 *
 * \param [in] p8est       The forest.
 * \param [in] which_tree  The tree in the forest containing \a q.
 * \param [in] quadrant    The Quadrant.
 */
int refine_geometric(p8est_t *p8est, p4est_topidx_t which_tree,
                     p8est_quadrant_t *q);

/*** HELPER FUNCTIONS ***/
/* Geometry */
/** Get the coordinates of the midpoint of a quadrant.
 *
 * \param [in]  p8est    the forest
 * \param [in]  which_tree the tree in the forest containing \a q
 * \param [in]  q      the quadrant
 * \param [out] xyz    the coordinates of the midpoint of \a q
 */
void lbadapt_get_midpoint(p8est_t *p8est, p4est_topidx_t which_tree,
                          p8est_quadrant_t *q, lb_float xyz[3]);

/** Get the coordinates of the midpoint of a quadrant
 *
 * \param [in]  mesh_iter  A mesh-based iterator.
 * \param [out] xyz        The coordinates of the the midpoint of the current
 *                         quadrant that mesh_iter is pointing to.
 */
void lbadapt_get_midpoint(p8est_meshiter_t *mesh_iter, lb_float xyz[3]);

/** Get the coordinates of the front lower left corner of a quadrant.
 *
 * \param [in]  p8est    the forest
 * \param [in]  which_tree the tree in the forest containing \a q
 * \param [in]  q      the quadrant
 * \param [out] xyz    the coordinates of the midpoint of \a q
 */
void lbadapt_get_front_lower_left(p8est_t *p8est, p4est_topidx_t which_tree,
                                  p8est_quadrant_t *q, double xyz[3]);

/** Get the coordinates of the front lower left corner of a quadrant
 *
 * \param [in]  mesh_iter  A mesh-based iterator.
 * \param [out] xyz        The coordinates of the the front lower left corner
 *                         of the current quadrant that mesh_iter is pointing
 *                         to.
 */
void lbadapt_get_front_lower_left(p8est_meshiter_t *mesh_iter, lb_float xyz[3]);

/* LBM */
/** Calculate equilibrium distribution from given fluid parameters
 *
 * \param [in][out] datafield  The fluid node that shall be written.
 * \param [in]      rho        The fluids density.
 * \param [in]      j          The fluids velocity.
 * \param [in]      pi         The fluids stress tensor.
 * \param [in]      h          The local mesh-width.
 */
int lbadapt_calc_n_from_rho_j_pi(lb_float datafield[2][19], lb_float rho,
                                 lb_float *j, lb_float *pi, lb_float h);

/** Calculate modes for MRT scheme
 *
 * \param [in]      populations The population vector.
 * \param     [out] mode        The resulting modes to be relaxed in a later
 * step.
 */
int lbadapt_calc_modes(lb_float populations[2][19], lb_float *mode);

/** Perform MRT Relaxation step
 *
 * \param [in][out] mode  kinematic modes of the fluid.
 * \param [in]      force Force that is applied on the fluid.
 * \param [in]      h     Meshwidth of current cell
 */
int lbadapt_relax_modes(lb_float *mode, lb_float *force, lb_float h);

/** Thermalize modes
 *
 * \param [in][out] mode  The modes to be thermalized.
 */
int lbadapt_relax_modes(lb_float *mode);

/** Apply force on fluid.
 *
 * \param [in][out] mode  The modes that the force is applied on.
 * \param [in]      force The force that is applied.
 * \param [in]      h     The local mesh width.
 */
int lbadapt_apply_forces(lb_float *mode, lb_float *force, lb_float h);

/** Transfer modes back to populations
 *
 * \param     [out] populations  The resulting particle densities.
 * \param [in]      m            The modes.
 */
int lbadapt_calc_pop_from_modes(lb_float *populations, lb_float *m);

/** collision
 * CAUTION: sync ghost data after collision
 *
 * \param [in] level   The level on which to perform the collision step
 */
void lbadapt_collide(int level);

/** Populate virtual cells with post-collision values from their respective
 * father cell
 *
 * \param [in] level   The level of the real cells whose virtual subcells are
 *                     populated.
 */
void lbadapt_populate_virtuals(int level);

/** streaming
 * CAUTION: sync ghost data before streaming
 *
 * \param [in] level   The level on which to perform the streaming step
 */
void lbadapt_stream(int level);

/** bounce back
 * CAUTION: sync ghost data before streaming
 *
 * \param [in] level   The level on which to perform the bounce-back step
 */
void lbadapt_bounce_back(int level);

/** Update population of real cells from streaming steps from neighboring
 * quadrants.
 *
 * \param [in] level   The level of the real cells whose populations are updated
 *                     from their respective virtual subcells.
 */
void lbadapt_update_populations_from_virtuals(int level);

/** swap pre- and poststreaming pointers
 *
 * \param [in] level   The level on which to swap lbfluid pointers
 */
void lbadapt_swap_pointers(int level);

/** Obtain boundary information of each quadrant for vtk output
 */
void lbadapt_get_boundary_values(sc_array_t *boundary_values);

/** Obtain density values of each quadrant for vtk output
 */
void lbadapt_get_density_values(sc_array_t *density_values);

/** Obtain velocity values of each quadrant for vtk output
 */
void lbadapt_get_velocity_values(sc_array_t *velocity_values);

/** Init boudary flag of each quadrant.
 */
void lbadapt_get_boundary_status();

/** Calculate local density from pre-collision moments
 *
 * \param [in]  mesh_iter    mesh-based iterator
 * \param [out] rho          density
 */
void lbadapt_calc_local_rho(p8est_meshiter_t *mesh_iter, lb_float *rho);

/** Calculate local fluid velocity from pre-collision moments
 *
 * \param [in]  mesh_iter    mesh-based iterator
 * \param [out] j            velocity
 */
void lbadapt_calc_local_j(p8est_meshiter_t *mesh_iter, lb_float *j);

/*** ITERATION CALLBACKS ***/
void lbadapt_set_recalc_fields(p8est_iter_volume_info_t *info, void *user_data);

void lbadapt_calc_local_rho(p8est_iter_volume_info_t *info, void *user_data);

void lbadapt_calc_local_j(p8est_iter_volume_info_t *info, void *user_data);

void lbadapt_calc_local_pi(p8est_iter_volume_info_t *info, void *user_data);

void lbadapt_dump2file(p8est_iter_volume_info_t *info, void *user_data);

#endif // LB_ADAPTIVE

#endif // _LB_ADAPTIVE_H