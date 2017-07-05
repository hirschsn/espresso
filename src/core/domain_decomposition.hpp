/*
  Copyright (C) 2010,2011,2012,2013,2014,2015,2016 The ESPResSo project
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
#ifndef _DOMAIN_DECOMPOSITION_H
#define _DOMAIN_DECOMPOSITION_H

/** \file domain_decomposition.hpp
 *
 *  This file contains everything related to the cell system: domain decomposition.
 *
 *  The simulation box is split into spatial domains for each node
 *  according to a cartesian node grid (\ref node_grid).
 *
 *  The domain of a node is split into a 3D cell grid with dimension
 *  \ref DomainDecomposition::cell_grid. Together with one ghost cell
 *  layer on each side the overall dimension of the ghost cell grid is
 *  \ref DomainDecomposition::ghost_cell_grid. The domain
 *  decomposition enables one the use of the linked cell algorithm
 *  which is in turn used for setting up the verlet list for the
 *  system. You can see a 2D graphical representation of the linked
 *  cell grid below.
 *
 *  \image html  linked_cells.gif "Linked cells structure"
 *
 *  2D representation of a linked cell grid: cell_grid =
 *  {4,4}, ghost_cell_grid = {6,6}
 *
 * Each cell has 3^D neighbor cells (For cell 14 they are
 * marked). Since we deal with pair forces, it is sufficient to
 * calculate only half of the interactions (Newtons law: actio =
 * reactio). We have chosen the upper half e.g. all neighbor cells with
 * a higher linear index (For cell 14 they are marked in light
 * blue). Caution: This implementation needs double sided ghost
 * communication! For single sided ghost communication one would need
 * some ghost-ghost cell interaction as well, which we do not need!
 *
 *  For more information on cells,
 *  see \ref cells.hpp 
*/

#include "utils.hpp"
#include "cells.hpp"
#include "integrate.hpp"
#include "verlet.hpp"
#include "thermostat.hpp"

#ifdef DD_P4EST
#include <p4est_to_p8est.h>
#include <p8est_ghost.h>
#include <p8est_mesh.h>

// Structure that stores basic grid information
typedef struct {
  int64_t idx; // a unique index within all cells (as used by p4est for locals)
  int rank; // the rank of this cell (equals this_node for locals)
  int shell; // shell information (0: inner local cell, 1: boundary local cell, 2: ghost cell)
  int boundary; // Bit mask storing boundary info. MSB ... z_r,z_l,y_r,y_l,x_r,x_l LSB
                // Cells with shell-type 0 or those located within the domain are always 0
                // Cells with shell-type 1 store information about which face is a boundary
                // Cells with shell-type 2 are set if the are in the periodic halo
  int neighbor[26]; // unique index of the fullshell neighborhood cells (as in p4est)
  int coord[3]; // cartesian coordinates of the cell
  int p_cnt; // periodic count of this cell, local cells are always 0, for each new periodic occurence
             // the new occurence is increased by one
} local_shell_t;

// Structure to store communications
typedef struct {
  int rank; // Rank of the communication partner
  int cnt;  // number of cells to communicate
  int dir;  // Bitmask for communication direction. MSB ... z_r,z_l,y_r,y_l,x_r,x_l LSB
  std::vector<int> idx; // List of cell indexes
} comm_t;

#endif

/** Structure containing information about non bonded interactions
    with particles in a neighbor cell. */
typedef struct {
  /** Just for transparency the index of the neighbor cell. */
  int cell_ind;
  /** Pointer to particle list of neighbor cell. */
  ParticleList *pList;
  /** Verlet list for non bonded interactions of a cell with a neighbor cell. */
  PairList vList;

#ifdef CELL_DEBUG
  double my_pos[3];  /* position of the cell corner, only here for debug */
#endif

} IA_Neighbor;


typedef struct {
  /** number of interacting neighbor cells . 

      A word about the interacting neighbor cells:

      In a 3D lattice each cell has 27 neighbors (including
      itself!). Since we deal with pair forces, it is sufficient to
      calculate only half of the interactions (Newtons law: actio =
      reactio). For each cell 13+1=14 neighbors. This has only to be
      done for the inner cells. 

      Caution: This implementation needs double sided ghost
      communication! For single sided ghost communication one would
      need some ghost-ghost cell interaction as well, which we do not
      need! 

      It follows: inner cells: n_neighbors = 14
      ghost cells:             n_neighbors = 0
  */
  int n_neighbors;
  /** Interacting neighbor cell list  */
  IA_Neighbor *nList;
} IA_Neighbor_List;

#ifdef DD_P4EST
namespace std
{
  template<>
  struct default_delete<p4est_t>
  {
    void operator()(p4est_t *p) const { if (p != nullptr) p4est_destroy(p); }
  };
  template<>
  struct default_delete<p4est_ghost_t>
  {
    void operator()(p4est_ghost_t *p) const { if (p != nullptr) p4est_ghost_destroy(p); }
  };
  template<>
  struct default_delete<p4est_mesh_t>
  {
    void operator()(p4est_mesh_t *p) const { if (p != nullptr) p4est_mesh_destroy(p); }
  };
  template<>
  struct default_delete<p4est_connectivity_t>
  {
    void operator()(p4est_connectivity_t *p) const { if (p != nullptr) p4est_connectivity_destroy(p); }
  };
}

// Don't use it. Can lead to nasty bugs.
template <typename T>
struct castable_unique_ptr: public std::unique_ptr<T> {
  using Base = std::unique_ptr<T>;
  constexpr castable_unique_ptr(): Base() {}
  constexpr castable_unique_ptr(nullptr_t n): Base(n) {}
  castable_unique_ptr(T* p): Base(p) {}
  castable_unique_ptr(Base&& other): Base(std::move(other)) {}
  operator T*() const { return this->get(); }
  operator void *() const { return this->get(); }
};
#endif

/** Structure containing the information about the cell grid used for domain decomposition. */
typedef struct {
  /** flag for using Verlet List */
  int use_vList;
  /** linked cell grid in nodes spatial domain. */
  int cell_grid[3];
  /** linked cell grid with ghost frame. */
  int ghost_cell_grid[3];
  /** cell size. 
      Def: \verbatim cell_grid[i] = (int)(local_box_l[i]/max_range); \endverbatim */
  double cell_size[3];
  /** inverse cell size = \see DomainDecomposition::cell_size ^ -1. */
  double inv_cell_size[3];
  /** Array containing information about the interactions between the cells. */
  IA_Neighbor_List *cell_inter;
#ifdef DD_P4EST
  castable_unique_ptr<p4est_t> p4est;
  castable_unique_ptr<p4est_connectivity_t> p4est_conn;
  std::vector<local_shell_t> p4est_shell;
#endif
}  DomainDecomposition;

/************************************************************/
/** \name Exported Variables */
/************************************************************/
/*@{*/

/** Information about the domain decomposition. */
extern DomainDecomposition dd;

/** Maximal skin size. This is a global variable wwhich can be read
    out by the user via the TCL command setmd in order to optimize the
    cell grid */
extern double max_skin;

/** Maximal number of cells per node. In order to avoid memory
 *  problems due to the cell grid one has to specify the maximal
 *  number of \ref cells::cells . The corresponding callback function
 *  is \ref tclcallback_max_num_cells. If the number of cells \ref
 *  n_cells, is larger than max_num_cells the cell grid is
 *  reduced. max_num_cells has to be larger than 27, e.g one inner
 *  cell.  max_num_cells is initialized with the default value
 *  specified in \ref config.hpp as \ref CELLS_MAX_NUM_CELLS.
 */
extern int max_num_cells;

/** Minimal number of cells per node. This is mainly to avoid excessively large
    numbers of particles per cell, which will result in really large Verlet
    lists and eventually crash Espresso.
*/
extern int min_num_cells;

/*@}*/

/************************************************************/
/** \name Exported Functions */
/************************************************************/
/*@{*/

/** adjust the domain decomposition to a change in the geometry.
    Tries to speed up things if possible.

    @param flags a combination of \ref CELL_FLAG_FAST and \ref
    CELL_FLAG_GRIDCHANGED, see documentation of \ref
    cells_on_geometry_change.
*/
void dd_on_geometry_change(int flags);

/** Initialize the topology. The argument is a list of cell pointers,
    containing particles that have to be sorted into new cells. The
    particles might not belong to this node.  This procedure is used
    when particle data or cell structure has changed and the cell
    structure has to be reinitialized. This also includes setting up
    the cell_structure array.
    @param cl List of cell pointers with particles to be stored in the
    new cell system.
*/
void dd_topology_init(CellPList *cl);

/** Called when the current cell structure is invalidated because for
    example the box length has changed. This procedure may NOT destroy
    the old inner and ghost cells, but it should free all other
    organizational data. Note that parameters like the box length or
    the node_grid may already have changed. Therefore organizational
    data has to be stored independently from variables that may be
    changed from outside. */
void dd_topology_release();

/** Just resort the particles. Used during integration. The particles
    are stored in the cell structure.

    @param global_flag Use DD_GLOBAL_EXCHANGE for global exchange and
    DD_NEIGHBOR_EXCHANGE for neighbor exchange (recommended for use within
    Molecular dynamics, or any other integration scheme using only local
    particle moves) 
*/
void dd_exchange_and_sort_particles(int global_flag);

/** implements \ref CellStructure::position_to_cell. */
Cell *dd_position_to_cell(double pos[3]);

/** Get three cell indices (coordinates in cell gird) from particle position */
void dd_position_to_cell_indices(double pos[3],int* idx);

/** calculate physical (processor) minimal number of cells */
int calc_processor_min_num_cells();

/** Calculate nonbonded and bonded forces with link-cell 
    method (without Verlet lists)
*/
void calc_link_cell();

/** Nonbonded and bonded energy calculation using link-cell method */
void calculate_link_cell_energies();

/** Nonbonded and bonded virials calculation using link-cell method */
void calculate_link_cell_virials(int v_comp);

/** Fill a communication cell pointer list. Fill the cell pointers of
    all cells which are inside a rectangular subgrid of the 3D cell
    grid (\ref DomainDecomposition::ghost_cell_grid) starting from the
    lower left corner lc up to the high top corner hc. The cell
    pointer list part_lists must already be large enough.
    \param part_lists  List of cell pointers to store the result.
    \param lc          lower left corner of the subgrid.
    \param hc          high up corner of the subgrid.
 */
int dd_fill_comm_cell_lists(Cell **part_lists, int lc[3], int hc[3]);


/** Returns pointer to the cell which corresponds to the position if
    the position is in the nodes spatial domain otherwise a NULL
    pointer. */
Cell *dd_save_position_to_cell(double pos[3]);

/** Of every two communication rounds, set the first receivers to prefetch and poststore */
void dd_assign_prefetches(GhostCommunicator *comm);

void calc_link_cell_runtime(int nsteps, std::vector<double>& ts);
/*@}*/

#endif
