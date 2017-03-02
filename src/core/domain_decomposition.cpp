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
/** \file domain_decomposition.cpp
 *
 *  This file contains everything related to the cell system: domain decomposition.
 *  See also \ref domain_decomposition.hpp
 */

#include "domain_decomposition.hpp"
#include "lees_edwards_domain_decomposition.hpp"
#include "lees_edwards_comms_manager.hpp"
#include "lees_edwards.hpp"
#include "errorhandling.hpp"
#include "forces.hpp"
#include "pressure.hpp"
#include "energy_inline.hpp"
#include "constraint.hpp"
#include "initialize.hpp"
#include "external_potential.hpp"

/************************************************/
/** \name Defines */
/************************************************/
/*@{*/

/** half the number of cell neighbors in 3 Dimensions. */
#define CELLS_MAX_NEIGHBORS 14

/*@}*/

/************************************************/
/** \name Variables */
/************************************************/
/*@{*/

#ifdef LEES_EDWARDS
le_dd_comms_manager le_mgr;
#endif
DomainDecomposition dd = { 1, {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0}, NULL };

int max_num_cells = CELLS_MAX_NUM_CELLS;
int min_num_cells = 1;
double max_skin   = 0.0;

/*@}*/

/************************************************************/
/** \name Private Functions */
/************************************************************/
/*@{*/

/** Convenient replace for loops over all cells. */
#define DD_CELLS_LOOP(m,n,o) \
  for(o=0; o<dd.ghost_cell_grid[2]; o++) \
    for(n=0; n<dd.ghost_cell_grid[1]; n++) \
      for(m=0; m<dd.ghost_cell_grid[0]; m++)

/** Convenient replace for loops over Local cells. */
#define DD_LOCAL_CELLS_LOOP(m,n,o) \
  for(o=1; o<dd.cell_grid[2]+1; o++) \
    for(n=1; n<dd.cell_grid[1]+1; n++) \
      for(m=1; m<dd.cell_grid[0]+1; m++)

/** Convenient replace for inner cell check. usage: if(DD_IS_LOCAL_CELL(m,n,o)) {...} */
#ifdef LEES_EDWARDS
#define DD_IS_LOCAL_CELL(m,n,o) \
  ( m > 0 && m <= dd.cell_grid[0] && \
    n > 0 && n <= dd.cell_grid[1] && \
    o > 0 && o <= dd.cell_grid[2] )
#else
#define DD_IS_LOCAL_CELL(m,n,o) \
  ( m > 0 && m < dd.ghost_cell_grid[0] - 1 && \
    n > 0 && n < dd.ghost_cell_grid[1] - 1 && \
    o > 0 && o < dd.ghost_cell_grid[2] - 1 )
#endif

/** Convenient replace for ghost cell check. usage: if(DD_IS_GHOST_CELL(m,n,o)) {...} */
#ifdef LEES_EDWARDS
#define DD_IS_GHOST_CELL(m,n,o) \
  ( m == 0 ||  m == dd.ghost_cell_grid[0] - 1 || \
    n == 0 || (n == dd.ghost_cell_grid[1] - 1 || n == dd.ghost_cell_grid[1] - 2) || \
    o == 0 ||  o == dd.ghost_cell_grid[2] - 1 ) 
#else
#define DD_IS_GHOST_CELL(m,n,o) \
  ( m == 0 || m == dd.ghost_cell_grid[0] - 1 || \
    n == 0 || n >= dd.ghost_cell_grid[1] - 1 || \
    o == 0 || o == dd.ghost_cell_grid[2] - 1 )
#endif

/** Calculate cell grid dimensions, cell sizes and number of cells.
 *  Calculates the cell grid, based on \ref local_box_l and \ref
 *  max_range. If the number of cells is larger than \ref
 *  max_num_cells, it increases max_range until the number of cells is
 *  smaller or equal \ref max_num_cells. It sets: \ref
 *  DomainDecomposition::cell_grid, \ref
 *  DomainDecomposition::ghost_cell_grid, \ref
 *  DomainDecomposition::cell_size, \ref
 *  DomainDecomposition::inv_cell_size, and \ref n_cells.
 */
void dd_create_cell_grid()
{
  int i,n_local_cells,new_cells,min_ind;
  double cell_range[3], min_size, scale, volume;
  CELL_TRACE(fprintf(stderr, "%d: dd_create_cell_grid: max_range %f\n",this_node,max_range));
  CELL_TRACE(fprintf(stderr, "%d: dd_create_cell_grid: local_box %f-%f, %f-%f, %f-%f,\n",this_node,my_left[0],my_right[0],my_left[1],my_right[1],my_left[2],my_right[2]));
  
  /* initialize */
  cell_range[0]=cell_range[1]=cell_range[2] = max_range;

  if (max_range < ROUND_ERROR_PREC*box_l[0]) {
    /* this is the initialization case */
#ifdef LEES_EDWARDS
    dd.cell_grid[0] = 2;
    dd.cell_grid[1] = 1;
    dd.cell_grid[2] = 1;
    n_local_cells   = 2;
#else
    n_local_cells = dd.cell_grid[0] = dd.cell_grid[1] = dd.cell_grid[2]=1;
#endif
  }
  else {
    /* Calculate initial cell grid */
    volume = local_box_l[0];
    for(i=1;i<3;i++) volume *= local_box_l[i];
    scale = pow(max_num_cells/volume, 1./3.);
    for(i=0;i<3;i++) {
      /* this is at least 1 */
      dd.cell_grid[i] = (int)ceil(local_box_l[i]*scale);
      cell_range[i] = local_box_l[i]/dd.cell_grid[i];

      if ( cell_range[i] < max_range ) {
	/* ok, too many cells for this direction, set to minimum */
	dd.cell_grid[i] = (int)floor(local_box_l[i]/max_range);
	if ( dd.cell_grid[i] < 1 ) {
	  runtimeErrorMsg() << "interaction range " << max_range << " in direction "
	      << i << " is larger than the local box size " << local_box_l[i];
	  dd.cell_grid[i] = 1;
	}
#ifdef LEES_EDWARDS
        if ( (i == 0) && (dd.cell_grid[0] < 2) ) {
	  runtimeErrorMsg() << "interaction range " << max_range << " in direction "
	      << i << " is larger than half the local box size " << local_box_l[i] << "/2";
	  dd.cell_grid[0] = 2;
        }
#endif
	cell_range[i] = local_box_l[i]/dd.cell_grid[i];
      }
    }

    /* It may be necessary to asymmetrically assign the scaling to the coordinates, which the above approach will not do.
       For a symmetric box, it gives a symmetric result. Here we correct that. */
    for (;;) {

      n_local_cells = dd.cell_grid[0] * dd.cell_grid[1] * dd.cell_grid[2];

      /* done */
      if (n_local_cells <= max_num_cells)
          break;

      /* find coordinate with the smallest cell range */
      min_ind = 0;
      min_size = cell_range[0];

#ifdef LEES_EDWARDS
      for (i = 2; i >= 1; i--) {/*preferably have thin slices in z or y... this is more efficient for Lees Edwards*/
#else
      for (i = 1; i < 3; i++) {
#endif       
          if (dd.cell_grid[i] > 1 && cell_range[i] < min_size) {
                min_ind = i;
                min_size = cell_range[i];
              }
      }
      CELL_TRACE(fprintf(stderr, "%d: minimal coordinate %d, size %f, grid %d\n", this_node,min_ind, min_size, dd.cell_grid[min_ind]));

      dd.cell_grid[min_ind]--;
      cell_range[min_ind] = local_box_l[min_ind]/dd.cell_grid[min_ind];
    }
    CELL_TRACE(fprintf(stderr, "%d: final %d %d %d\n", this_node, dd.cell_grid[0], dd.cell_grid[1], dd.cell_grid[2]));

    /* sanity check */
    if (n_local_cells < min_num_cells) {
        runtimeErrorMsg() << "number of cells "<< n_local_cells << " is smaller than minimum " << min_num_cells <<
               " (interaction range too large or min_num_cells too large)";
    }
  }

  /* quit program if unsuccesful */
  if(n_local_cells > max_num_cells) {
      runtimeErrorMsg() << "no suitable cell grid found ";
  }

  /* now set all dependent variables */
  new_cells=1;
  for(i=0;i<3;i++) {
    dd.ghost_cell_grid[i] = dd.cell_grid[i]+2;
#ifdef LEES_EDWARDS
    //Hack alert: only the boundary y-layers actually need the extra-thick ghost cell grid,
    //so some memory (and copies) are wasted in the name of simpler code.
    if( i == 0 ){dd.ghost_cell_grid[i]++;}  
#endif
    new_cells            *= dd.ghost_cell_grid[i];
    dd.cell_size[i]       = local_box_l[i]/(double)dd.cell_grid[i];
    dd.inv_cell_size[i]   = 1.0 / dd.cell_size[i];
  }
  max_skin = std::min(std::min(dd.cell_size[0],dd.cell_size[1]),dd.cell_size[2]) - max_cut;

  /* allocate cell array and cell pointer arrays */
  realloc_cells(new_cells);
  realloc_cellplist(&local_cells, local_cells.n = n_local_cells);
  realloc_cellplist(&ghost_cells, ghost_cells.n = new_cells-n_local_cells);

  CELL_TRACE(fprintf(stderr, "%d: dd_create_cell_grid, n_cells=%d, local_cells.n=%d, ghost_cells.n=%d, dd.ghost_cell_grid=(%d,%d,%d)\n", this_node, n_cells,local_cells.n,ghost_cells.n,dd.ghost_cell_grid[0],dd.ghost_cell_grid[1],dd.ghost_cell_grid[2]));
}

/** Fill local_cells list and ghost_cells list for use with domain
    decomposition.  \ref cells::cells is assumed to be a 3d grid with size
    \ref DomainDecomposition::ghost_cell_grid . */
void dd_mark_cells()
{
  int m,n,o,cnt_c=0,cnt_l=0,cnt_g=0;
  
  DD_CELLS_LOOP(m,n,o) {

#ifdef LEES_EDWARDS
    /* convenient for LE if a cell knows where it is*/
    cells[cnt_c].myIndex[0] = m;
    cells[cnt_c].myIndex[1] = n;
    cells[cnt_c].myIndex[2] = o;
#endif      

    if(DD_IS_LOCAL_CELL(m,n,o)) local_cells.cell[cnt_l++] = &cells[cnt_c++]; 
    else                        ghost_cells.cell[cnt_g++] = &cells[cnt_c++];
  } 

}

/** Fill a communication cell pointer list. Fill the cell pointers of
    all cells which are inside a rectangular subgrid of the 3D cell
    grid (\ref DomainDecomposition::ghost_cell_grid) starting from the
    lower left corner lc up to the high top corner hc. The cell
    pointer list part_lists must already be large enough.
    \param part_lists  List of cell pointers to store the result.
    \param lc          lower left corner of the subgrid.
    \param hc          high up corner of the subgrid.
 */
int dd_fill_comm_cell_lists(Cell **part_lists, int lc[3], int hc[3])
{
  int i,m,n,o,c=0;
  /* sanity check */
  for(i=0; i<3; i++) {
    if(lc[i]<0 || lc[i] >= dd.ghost_cell_grid[i]) return 0;
    if(hc[i]<0 || hc[i] >= dd.ghost_cell_grid[i]) return 0;
    if(lc[i] > hc[i]) return 0;
  }

  for(o=lc[0]; o<=hc[0]; o++) 
    for(n=lc[1]; n<=hc[1]; n++) 
      for(m=lc[2]; m<=hc[2]; m++) {
        i = get_linear_index(o,n,m,dd.ghost_cell_grid);
        CELL_TRACE(fprintf(stderr,"%d: dd_fill_comm_cell_list: add cell %d\n",this_node,i));
        part_lists[c] = &cells[i];
        c++;
      }
  return c;
}


/** Determine lc and hc as lower and upper corner of the area to send.
 * \param disp the displacement vector in {-1, 0, 1}^3
 * \param recv 0 or 1 indicating a send (0) or receive (1) operation
 * \param lc out lower corner of communication area, inclusive
 * \param hc out high corner of communication area, inclusive
 */
static void dd_determine_send_receive_cells(const int disp[3], int recv, int lc[3], int hc[3])
{
  for (int i = 0; i < 3; ++i) {
    lc[i] = disp[i] <= 0? 1: dd.cell_grid[i];
    hc[i] = disp[i] < 0? 1: dd.cell_grid[i];

    // The receive area is actually in the ghost layer
    // so shift the corresponding indices.
    if (recv) {
      if (disp[i] > 0)
        lc[i] = hc[i] = dd.cell_grid[i] + 1;
      else if (disp[i] < 0)
        lc[i] = hc[i] = 0;
    }
  }
}

/** Count the number of cells in a cell range given by lc and hc, both
 * inclusive!
 */
static int dd_lc_hc_count_ncells(const int lc[3], const int hc[3])
{
    int ncells = 1;
    for (int d = 0; d < 3; ++d)
      ncells *= hc[d] - lc[d] + 1;
    return ncells;
}


/** Returns an integer unique for a given displacement from {-1, 0, 1}^3.
 * Is the same for send and receive site, i.e. for sending with displacement d
 * and receiving with -d this functions returns the same tag.
 * \param recv 0 if send operation 1 if receive
 * \param disp displacement vector from {-1, 0, 1}^3
 * \return unique number for a displacement vector
 */
static int async_comm_get_tag(int recv, const int disp[3])
{
  int tag = 0;
  for (int i = 0; i < 3; ++i)
    tag = tag * 10 + (disp[i] * (recv? -1: 1) + 1);
  return tag;
}

/** Create communicators for cell structure domain decomposition. (see \ref GhostCommunicator)
 * Works ONLY for FULLY periodic systems.
 */
void  dd_prepare_comm(GhostCommunicator *comm, int data_parts)
{
  const int nneigh = 26;
  const static int disps[nneigh][3] = {
      {-1, -1, -1},
      {-1, -1,  0},
      {-1, -1,  1},
      {-1,  0, -1},
      {-1,  0,  0},
      {-1,  0,  1},
      {-1,  1, -1},
      {-1,  1,  0},
      {-1,  1,  1},
      { 0, -1, -1},
      { 0, -1,  0},
      { 0, -1,  1},
      { 0,  0, -1},
      //{ 0,  0,  0}, Actually not a boundary so nothing to send, here.
      { 0,  0,  1},
      { 0,  1, -1},
      { 0,  1,  0},
      { 0,  1,  1},
      { 1, -1, -1},
      { 1, -1,  0},
      { 1, -1,  1},
      { 1,  0, -1},
      { 1,  0,  0},
      { 1,  0,  1},
      { 1,  1, -1},
      { 1,  1,  0},
      { 1,  1,  1},
  };

  // Prepare communicator
  CELL_TRACE(fprintf(stderr,"%d Create Communicator: prep_comm data_parts %d num %d\n", this_node, data_parts, 2 * nneigh));
  prepare_comm(comm, data_parts, 2 * nneigh, true); // Send and receive per neighbor

  // Prepare communications
  for (int i = 0; i < nneigh; ++i) {
    int node = async_grid_get_neighbor_rank(disps[i]);

    // Send receive loop
    for (int sr = 0; sr <= 1; ++sr) {
      int lc[3], hc[3], ncells;
      dd_determine_send_receive_cells(disps[i], sr, lc, hc);
      ncells = dd_lc_hc_count_ncells(lc, hc);

      comm->comm[2*i+sr].type = sr == 0? GHOST_SEND: GHOST_RECV;
      comm->comm[2*i+sr].node = node;
      comm->comm[2*i+sr].n_part_lists = ncells;
      comm->comm[2*i+sr].part_lists = (ParticleList **) Utils::malloc(ncells * sizeof(ParticleList *));
      comm->comm[2*i+sr].tag = async_comm_get_tag(sr, disps[i]);

      int nc = dd_fill_comm_cell_lists(comm->comm[2*i+sr].part_lists, lc, hc);
      // Sanity check
      if (nc != ncells) {
        fprintf(stderr, "[Node %i] dd_prepare_comm: Wrote %i cells but expected %i. UB :(\n", this_node, nc, ncells);
        fprintf(stderr, "Diagnostic: lc = %i,%i,%i, hc = %i,%i,%i, disp = %i,%i,%i, node = %i\n", lc[0], lc[1], lc[2], hc[0], hc[1], hc[2], disps[i][0], disps[i][1], disps[i][2], node);
        errexit();
      }

      // Set the periodic shift (only relevant for sender and only if requested)
      // in the relevant directions
      for (int d = 0; d < 3; ++d)
        if (sr == 0 && (data_parts & GHOSTTRANS_POSSHFTD) && async_grid_is_node_on_boundary(disps[i], d))
          comm->comm[2*i+sr].shift[d] = -disps[i][d] * box_l[d];
    }
  }
}

/** Exchange GHOST_SEND and GHOST_RECV Communicator types.
 */
void dd_revert_comm_order(GhostCommunicator *comm)
{
  int i;

  CELL_TRACE(fprintf(stderr,"%d: dd_revert_comm_order: anz comm: %d\n",this_node,comm->num));

  /* exchange SEND/RECV */
  for(i = 0; i < comm->num; i++) {
    if (comm->comm[i].type == GHOST_SEND)
      comm->comm[i].type = GHOST_RECV;
    else if (comm->comm[i].type == GHOST_RECV)
      comm->comm[i].type = GHOST_SEND;
  }
}

/** Signum function
 */
static inline double sign(double n)
{
  if (n > 0.0)
    return 1.0;
  else if (n < 0.0)
    return -1.0;
  else
    return 0.0;
}

/** Rescale a communicator to shift by the current box_l.
 */
static void dd_comm_rescale_shift(GhostCommunicator *gc)
{
  // No need to recalc the shifts using the Cartesian process displacements.
  // Just use the signum and multiply it to the new box_l.
  for (int i = 0; i < gc->num; ++i) {
    double *shift = gc->comm[i].shift;
    for (int d = 0; d < 3; ++d)
      shift[d] = sign(shift[d]) * box_l[d];
  }
}

/** update the 'shift' member of those GhostCommunicators, which use
    that value to speed up the folding process of its ghost members
    (see \ref dd_prepare_comm for the original), i.e. all which have
    GHOSTTRANS_POSSHFTD or'd into 'data_parts' upon execution of \ref
    dd_prepare_comm. */
void dd_update_communicators_w_boxl()
{
  dd_comm_rescale_shift(&cell_structure.exchange_ghosts_comm);
  dd_comm_rescale_shift(&cell_structure.update_ghost_pos_comm);
}

/** Init cell interactions for cell system domain decomposition.
 * initializes the interacting neighbor cell list of a cell The
 * created list of interacting neighbor cells is used by the verlet
 * algorithm (see verlet.cpp) to build the verlet lists.
 */
void dd_init_cell_interactions()
{
  int m,n,o,p,q,r,ind1,ind2,c_cnt=0,n_cnt;
 
  /* initialize cell neighbor structures */
  dd.cell_inter = (IA_Neighbor_List *) Utils::realloc(dd.cell_inter,local_cells.n*sizeof(IA_Neighbor_List));
  for(m=0; m<local_cells.n; m++) { 
    dd.cell_inter[m].nList = NULL; 
    dd.cell_inter[m].n_neighbors=0; 
  }

  /* loop all local cells */
  DD_LOCAL_CELLS_LOOP(m,n,o) {
    dd.cell_inter[c_cnt].nList = (IA_Neighbor *) Utils::realloc(dd.cell_inter[c_cnt].nList, CELLS_MAX_NEIGHBORS*sizeof(IA_Neighbor));

    n_cnt=0;
    ind1 = get_linear_index(m,n,o,dd.ghost_cell_grid);
    /* loop all neighbor cells */
    for(p=o-1; p<=o+1; p++)        
      for(q=n-1; q<=n+1; q++)
        for(r=m-1; r<=m+1; r++) {
          ind2 = get_linear_index(r,q,p,dd.ghost_cell_grid);
          if(ind2 >= ind1) {
            dd.cell_inter[c_cnt].nList[n_cnt].cell_ind = ind2;
            dd.cell_inter[c_cnt].nList[n_cnt].pList    = &cells[ind2];
            init_pairList(&dd.cell_inter[c_cnt].nList[n_cnt].vList);
#ifdef CELL_DEBUG
    dd.cell_inter[c_cnt].nList[n_cnt].my_pos[0] = my_left[0] + r * dd.cell_size[0];
    dd.cell_inter[c_cnt].nList[n_cnt].my_pos[1] = my_left[1] + q * dd.cell_size[1];
    dd.cell_inter[c_cnt].nList[n_cnt].my_pos[2] = my_left[2] + p * dd.cell_size[2];
#endif
            n_cnt++;
          }
        }


    dd.cell_inter[c_cnt].n_neighbors = n_cnt; 
    c_cnt++;
  }

#ifdef CELL_DEBUG
  FILE *cells_fp;
  char cLogName[64];
  int  c,nn,this_n;
  double myPos[3];
  sprintf(cLogName, "cells_map%i.dat", this_node);
  cells_fp = fopen(cLogName,"w");


  for(c=0;c<c_cnt;c++){
     myPos[0] = my_left[0] + dd.cell_size[0] * ( 1 + c % dd.cell_grid[0] );  
     myPos[1] = my_left[1] + dd.cell_size[1] * ( 1 + (c / dd.cell_grid[0]) % dd.cell_grid[1]);  
     myPos[2] = my_left[2] + dd.cell_size[2] * ( 1 + (c / (dd.cell_grid[0] * dd.cell_grid[1])));  

     for(nn=0;nn<dd.cell_inter[c].n_neighbors;nn++){
        
        this_n = dd.cell_inter[c].nList[nn].cell_ind;


        fprintf(cells_fp,"%i %i %f %f %f %f %f %f\n",c,nn,
            myPos[0], myPos[1], myPos[2], 
            dd.cell_inter[c].nList[nn].my_pos[0], 
            dd.cell_inter[c].nList[nn].my_pos[1], 
            dd.cell_inter[c].nList[nn].my_pos[2]);
          
     }
  }  
  fclose(cells_fp);
#endif

}

/*************************************************/

/** Returns pointer to the cell which corresponds to the position if
    the position is in the nodes spatial domain otherwise a NULL
    pointer. */
Cell *dd_save_position_to_cell(double pos[3]) 
{
  int i,cpos[3];
  double lpos;

  for(i=0;i<3;i++) {
    lpos = pos[i] - my_left[i];

    cpos[i] = (int)(lpos*dd.inv_cell_size[i])+1;
    
    /* particles outside our box. Still take them if
       VERY close or nonperiodic boundary */
    if (cpos[i] < 1) {
      if (lpos > -ROUND_ERROR_PREC*box_l[i]
          || (!PERIODIC(i) && boundary[2*i])
          )
        cpos[i] = 1;
      else
        return NULL;
    }
    else if (cpos[i] > dd.cell_grid[i]) {
      if (lpos < local_box_l[i] + ROUND_ERROR_PREC*box_l[i]
          || (!PERIODIC(i) && boundary[2*i+1])
          )
        cpos[i] = dd.cell_grid[i];
      else
        return NULL;
    }
  }
  i = get_linear_index(cpos[0],cpos[1],cpos[2], dd.ghost_cell_grid); 
  return &(cells[i]);  
}

Cell *dd_position_to_cell(double pos[3])
{
  int i,cpos[3];
  double lpos;

  for(i=0;i<3;i++) {
    lpos = pos[i] - my_left[i];

    cpos[i] = (int)(lpos*dd.inv_cell_size[i])+1;

    if (cpos[i] < 1) {
      cpos[i] = 1;
#ifdef ADDITIONAL_CHECKS
      if (PERIODIC(i) && lpos < -ROUND_ERROR_PREC*box_l[i]) {
	runtimeErrorMsg() << "particle @ (" << pos[0] << ", " << pos[1] << ", " << pos[2] << ") is outside of the allowed cell grid";
      }
#endif
    }
    else if (cpos[i] > dd.cell_grid[i]) {
      cpos[i] = dd.cell_grid[i];
#ifdef ADDITIONAL_CHECKS
      if (PERIODIC(i) && lpos > local_box_l[i] + ROUND_ERROR_PREC*box_l[i]) {
	runtimeErrorMsg() << "particle @ (" << pos[0] << ", " << pos[1] << ", " << pos[2] << ") is outside of the allowed cell grid";
      }
#endif
    }
  }
  i = get_linear_index(cpos[0],cpos[1],cpos[2], dd.ghost_cell_grid);  
  return &cells[i];
}

void dd_position_to_cell_indices(double pos[3],int* idx)
{
  int i;
  double lpos;

  for(i=0;i<3;i++) {
    lpos = pos[i] - my_left[i];

    idx[i] = (int)(lpos*dd.inv_cell_size[i])+1;

    if (idx[i] < 1) {
      idx[i] = 1;
    }
    else if (idx[i] > dd.cell_grid[i]) {
      idx[i] = dd.cell_grid[i];
    }
  }
}

/*************************************************/

/** Append the particles in pl to \ref local_cells and update \ref local_particles.  
    @return 0 if all particles in pl reside in the nodes domain otherwise 1.*/
int dd_append_particles(ParticleList *pl, int fold_dir)
{
  int p, dir, c, cpos[3], flag=0, fold_coord=fold_dir/2;

  CELL_TRACE(fprintf(stderr, "%d: dd_append_particles %d\n", this_node, pl->n));

  for(p=0; p<pl->n; p++) {
    if(boundary[fold_dir] != 0){
      fold_coordinate(pl->part[p].r.p, pl->part[p].m.v, pl->part[p].l.i, fold_coord);
    }    

    for(dir=0;dir<3;dir++) {
      cpos[dir] = (int)((pl->part[p].r.p[dir]-my_left[dir])*dd.inv_cell_size[dir])+1;

      if (cpos[dir] < 1) { 
        cpos[dir] = 1;
        if( PERIODIC(dir) )
          {
            flag=1;
            CELL_TRACE(if(fold_coord==2){fprintf(stderr, "%d: dd_append_particles: particle %d (%f,%f,%f) not inside node domain.\n", this_node,pl->part[p].p.identity,pl->part[p].r.p[0],pl->part[p].r.p[1],pl->part[p].r.p[2]);});
          }
      }
      else if (cpos[dir] > dd.cell_grid[dir]) {
        cpos[dir] = dd.cell_grid[dir];
        if( PERIODIC(dir) )
          {
            flag=1;
            CELL_TRACE(if(fold_coord==2){fprintf(stderr, "%d: dd_append_particles: particle %d (%f,%f,%f) not inside node domain.\n", this_node,pl->part[p].p.identity,pl->part[p].r.p[0],pl->part[p].r.p[1],pl->part[p].r.p[2]);});
          }
      }
    }
    c = get_linear_index(cpos[0],cpos[1],cpos[2], dd.ghost_cell_grid);
    CELL_TRACE(fprintf(stderr,"%d: dd_append_particles: Appen Part id=%d to cell %d\n",this_node,pl->part[p].p.identity,c));
    append_indexed_particle(&cells[c],&pl->part[p]);
  }
  return flag;
}
 
/*@}*/

/************************************************************/
/* Public Functions */
/************************************************************/

void dd_on_geometry_change(int flags) {

  /* Realignment of comms along the periodic y-direction is needed */
  if (flags & CELL_FLAG_LEES_EDWARDS) {
    CELL_TRACE(fprintf(stderr,"%d: dd_on_geometry_change responding to Lees-Edwards offset change.\n", this_node);)

#ifdef LEES_EDWARDS
    le_mgr.update_on_le_offset_change();
    
    le_dd_dynamic_update_comm(&le_mgr, &cell_structure.ghost_cells_comm,
                                        GHOSTTRANS_PARTNUM,
                                        LE_COMM_FORWARDS);
    le_dd_dynamic_update_comm(&le_mgr, &cell_structure.exchange_ghosts_comm,
                                      (GHOSTTRANS_PROPRTS | GHOSTTRANS_POSITION | GHOSTTRANS_POSSHFTD),
                                        LE_COMM_FORWARDS);
    le_dd_dynamic_update_comm(&le_mgr, &cell_structure.update_ghost_pos_comm,
                                      (GHOSTTRANS_POSITION | GHOSTTRANS_POSSHFTD),
                                        LE_COMM_FORWARDS);
    le_dd_dynamic_update_comm(&le_mgr, &cell_structure.collect_ghost_force_comm,
                                        GHOSTTRANS_FORCE,
                                        LE_COMM_BACKWARDS);
#ifdef LB
    le_dd_dynamic_update_comm(&cell_structure.ghost_lbcoupling_comm, GHOSTTRANS_COUPLING);
#endif

#endif

  }

  /* check that the CPU domains are still sufficiently large. */
  for (int i = 0; i < 3; i++)
    if (local_box_l[i] < max_range) {
        runtimeErrorMsg() <<"box_l in direction " << i << " is too small";
    }

  /* A full resorting is necessary if the grid has changed. We simply
     don't have anything fast for this case. Probably also not
     necessary. */
  if (flags & CELL_FLAG_GRIDCHANGED) {
    CELL_TRACE(fprintf(stderr,"%d: dd_on_geometry_change full redo\n",
		       this_node));
    cells_re_init(CELL_STRUCTURE_CURRENT);
    return;
  }

  /* otherwise, re-set our geometrical dimensions which have changed
     (in addition to the general ones that \ref grid_changed_box_l
     takes care of) */
  for(int i=0; i<3; i++) {
    dd.cell_size[i]       = local_box_l[i]/(double)dd.cell_grid[i];
    dd.inv_cell_size[i]   = 1.0 / dd.cell_size[i];
  }

  double min_cell_size = std::min(std::min(dd.cell_size[0],dd.cell_size[1]),dd.cell_size[2]);
  max_skin = min_cell_size - max_cut;

  CELL_TRACE(fprintf(stderr, "%d: dd_on_geometry_change: max_range = %f, min_cell_size = %f, max_skin = %f\n", this_node, max_range, min_cell_size, max_skin));
  
  if (max_range > min_cell_size) {
    /* if new box length leads to too small cells, redo cell structure
       using smaller number of cells. */
    cells_re_init(CELL_STRUCTURE_DOMDEC);
    return;
  }

  /* If we are not in a hurry, check if we can maybe optimize the cell
     system by using smaller cells. */
  if (!(flags & CELL_FLAG_FAST)) {
    int i;
    for(i=0; i<3; i++) {
      int poss_size = (int)floor(local_box_l[i]/max_range);
      if (poss_size > dd.cell_grid[i])
	break;
    }
    if (i < 3) {
      /* new range/box length allow smaller cells, redo cell structure,
	 possibly using smaller number of cells. */
      cells_re_init(CELL_STRUCTURE_DOMDEC);
      return;
    }
  }
#ifdef LEES_EDWARDS
  le_dd_update_communicators_w_boxl(&le_mgr);
#else
  dd_update_communicators_w_boxl();
#endif
  /* tell other algorithms that the box length might have changed. */
  on_boxl_change();
}

/************************************************************/
void dd_topology_init(CellPList *old)
{
  int c,p,np;
  int exchange_data, update_data;
  Particle *part;

  CELL_TRACE(fprintf(stderr, "%d: dd_topology_init: Number of recieved cells=%d\n", this_node, old->n));

  /** broadcast the flag for using verlet list */
  MPI_Bcast(&dd.use_vList, 1, MPI_INT, 0, comm_cart);
 
  cell_structure.type             = CELL_STRUCTURE_DOMDEC;
  cell_structure.position_to_node = map_position_node_array;
  cell_structure.position_to_cell = dd_position_to_cell;

  /* set up new domain decomposition cell structure */
  dd_create_cell_grid();
  /* mark cells */
  dd_mark_cells();

  /* create communicators */
#ifdef LEES_EDWARDS
  le_mgr.init(my_neighbor_count);
  le_dd_prepare_comm(&le_mgr, &cell_structure.ghost_cells_comm, GHOSTTRANS_PARTNUM);
#else
  dd_prepare_comm(&cell_structure.ghost_cells_comm, GHOSTTRANS_PARTNUM);
#endif

  exchange_data = (GHOSTTRANS_PROPRTS | GHOSTTRANS_POSITION | GHOSTTRANS_POSSHFTD);
  update_data   = (GHOSTTRANS_POSITION | GHOSTTRANS_POSSHFTD);

#ifdef LEES_EDWARDS
  le_dd_prepare_comm(&le_mgr, &cell_structure.exchange_ghosts_comm, exchange_data);
  le_dd_prepare_comm(&le_mgr, &cell_structure.update_ghost_pos_comm, update_data);
  le_dd_prepare_comm(&le_mgr, &cell_structure.collect_ghost_force_comm, GHOSTTRANS_FORCE);
#else
  dd_prepare_comm(&cell_structure.exchange_ghosts_comm,  exchange_data);
  dd_prepare_comm(&cell_structure.update_ghost_pos_comm, update_data);
  dd_prepare_comm(&cell_structure.collect_ghost_force_comm, GHOSTTRANS_FORCE);
#endif

  /* collect forces has to be done in reverted order! */
  dd_revert_comm_order(&cell_structure.collect_ghost_force_comm);

#ifdef LB
  dd_prepare_comm(&cell_structure.ghost_lbcoupling_comm, GHOSTTRANS_COUPLING) ;
#endif

#ifdef IMMERSED_BOUNDARY
  // Immersed boundary needs to communicate the forces from but also to the ghosts
  // This is different than usual collect_ghost_force_comm (not in reverse order)
  // Therefore we need our own communicator
  dd_prepare_comm(&cell_structure.ibm_ghost_force_comm, GHOSTTRANS_FORCE);
#endif

#ifdef ENGINE
  dd_prepare_comm(&cell_structure.ghost_swimming_comm, GHOSTTRANS_SWIMMING) ;
#endif

  /* initialize cell neighbor structures */
#ifdef LEES_EDWARDS
  le_dd_init_cell_interactions();
#else
  dd_init_cell_interactions();
#endif

  /* copy particles */
  for (c = 0; c < old->n; c++) {
    part = old->cell[c]->part;
    np   = old->cell[c]->n;
    for (p = 0; p < np; p++) {
      Cell *nc = dd_save_position_to_cell(part[p].r.p);
      /* particle does not belong to this node. Just stow away
         somewhere for the moment */
      if (nc == NULL)
        nc = local_cells.cell[0];
      append_unindexed_particle(nc, &part[p]);
    }
  }
  for(c=0; c<local_cells.n; c++) {
    update_local_particles(local_cells.cell[c]);
  }
  CELL_TRACE(fprintf(stderr,"%d: dd_topology_init: done\n",this_node));
}

/************************************************************/
void dd_topology_release()
{
  int i,j;
  CELL_TRACE(fprintf(stderr,"%d: dd_topology_release:\n",this_node));
  /* release cell interactions */
  for(i=0; i<local_cells.n; i++) {
    for(j=0; j<dd.cell_inter[i].n_neighbors; j++) 
      free_pairList(&dd.cell_inter[i].nList[j].vList);
    dd.cell_inter[i].nList = (IA_Neighbor *) Utils::realloc(dd.cell_inter[i].nList,0);
  }
  dd.cell_inter = (IA_Neighbor_List *) Utils::realloc(dd.cell_inter,0);
  /* free ghost cell pointer list */
  realloc_cellplist(&ghost_cells, ghost_cells.n = 0);
  /* free ghost communicators */
  free_comm(&cell_structure.ghost_cells_comm);
  free_comm(&cell_structure.exchange_ghosts_comm);
  free_comm(&cell_structure.update_ghost_pos_comm);
  free_comm(&cell_structure.collect_ghost_force_comm);
#ifdef LB
  free_comm(&cell_structure.ghost_lbcoupling_comm);
#endif
#ifdef ENGINE
  free_comm(&cell_structure.ghost_swimming_comm);
#endif
#ifdef IMMERSED_BOUNDARY
  free_comm(&cell_structure.ibm_ghost_force_comm);
#endif
}

/** Returns -1 if p if in [-inf, a), 0 if p in [a, b) and 1 else.
 * Comparisons are done w.r.t errmargin. If you want exact comparisons,
 * pass errmargin = 0.
 */
static int bin_between(double p, double a, double b, double errmargin = 0.0)
{
  if (p - a < -errmargin)
    return -1;
  else if (p - b >= errmargin)
    return 1;
  else
    return 0;
}


/** Fill the send buffers with particles that left the subdomain.
 * \param sendbuf send buffers (one for each neighbor) for particle data
 * \param sendbuf_dyn send buffers (one for each neighbor) for dynamic particle data
 * Be sure that all buffers are empty before calling this function.
 */
static void dd_async_exchange_fill_sendbufs(ParticleList sendbuf[26], std::vector<int> sendbuf_dyn[26])
{
  const double errmargin[3] = {
    0.5 * ROUND_ERROR_PREC * box_l[0],
    0.5 * ROUND_ERROR_PREC * box_l[1],
    0.5 * ROUND_ERROR_PREC * box_l[2],
  };
  int disp[3];

  for(int c = 0; c < local_cells.n; c++) {
    ParticleList *cell = local_cells.cell[c];
    for (int p = 0; p < cell->n; p++) {
      Particle *part = &cell->part[p];

      for (int d = 0; d < 3; ++d)
        disp[d] = bin_between(part->r.p[d], my_left[d], my_right[d], errmargin[d]);

      if (disp[0] != 0 || disp[1] != 0 || disp[2] != 0) {
        int li = async_grid_get_neighbor_index(disp);
        // Dynamic data (bonds and exclusions)
        sendbuf_dyn[li].insert(sendbuf_dyn[li].end(), part->bl.e, part->bl.e + part->bl.n);
#ifdef EXCLUSIONS
        sendbuf_dyn[li].insert(sendbuf_dyn[li].end(), part->el.e, part->el.e + part->el.n);
#endif
        // Particle data
        int pid = part->p.identity;
        move_indexed_particle(&sendbuf[li], cell, p);
        local_particles[pid] = NULL;
        if (p < cell->n) p--;
      }
    }
  }
}

/** Resorts particles within the subdomain.
 * ONLY call this if all particles in local_cells belong to this subdomain.
 * I.e. after dd_async_exchange_fill_sendbufs.
 */
static void dd_resort_particles()
{
  for(int c = 0; c < local_cells.n; c++) {
    ParticleList *cell = local_cells.cell[c];
    for (int p = 0; p < cell->n; p++) {
      Particle *part = &cell->part[p];
      ParticleList *sort_cell = dd_save_position_to_cell(part->r.p);
      if (sort_cell == NULL) {
        fprintf(stderr, "[%i] dd_exchange_and_sort_particles: Particle %i (%lf, %lf, %lf) not inside subdomain\n", this_node, part->p.identity, part->r.p[0], part->r.p[1], part->r.p[2]);
        errexit();
      } else if (sort_cell != cell) {
        move_indexed_particle(sort_cell, cell, p);
        if(p < cell->n) p--;
      }
    }
  }
}


/** Inserts particles from a ParticleList and calculate the size of the
 * dynamically allocated data of the particles.
 * Returns 1 if recvbuf contains oob particles.
 */
static int dd_async_exchange_insert_particles(ParticleList *recvbuf, int *dynsiz)
{
  *dynsiz = 0;

  update_local_particles(recvbuf);

  for (int p = 0; p < recvbuf->n; ++p) {
    fold_position(recvbuf->part[p].r.p, recvbuf->part[p].l.i);

    *dynsiz += recvbuf->part[p].bl.n;
#ifdef EXCLUSIONS
    *dynsiz += recvbuf->part[p].el.n;
#endif
  }
  // Fold direction of dd_append_particles unused.
  return dd_append_particles(recvbuf, 0);
}


/** Inserts the dynamic particle data from dynrecv to the particles from recvbuf.
 */
static void dd_async_exchange_insert_dyndata(ParticleList *recvbuf, std::vector<int> &dynrecv)
{
  int read = 0;

  for (int pc = 0; pc < recvbuf->n; pc++) {
    // Use local_particles to find the correct particle address since the
    // particles from recvbuf have already been copied by dd_append_particles
    // in dd_async_exchange_insert_particles.
    Particle *p = local_particles[recvbuf->part[pc].p.identity];
    if (p->bl.n > 0) {
      alloc_intlist(&p->bl, p->bl.n);
      memmove(p->bl.e, &dynrecv[read], p->bl.n * sizeof(int));
      read += p->bl.n;
    } else {
      p->bl.e = NULL;
    }
#ifdef EXCLUSIONS
    if (p->el.n > 0) {
      alloc_intlist(&p->el, p->el.n);
      memmove(p->el.e, &dynrecv[read], p->el.n*sizeof(int));
      read += p->el.n;
    }
    else {
      p->el.e = NULL;
    }
#endif
  }
}


/************************************************************/
void  dd_async_exchange_and_sort_particles(int global_flag)
{
  const int nneigh = 26;
  int nexchanges = 0;
  int oob_particles_exist = 1; // Boolean
  int neighrank[nneigh], neighdisp[nneigh][3];
  ParticleList sendbuf[nneigh], recvbuf[nneigh];
  std::vector<int> sendbuf_dyn[nneigh], recvbuf_dyn[nneigh];
  std::vector<MPI_Request> sreq(3 * nneigh, MPI_REQUEST_NULL);
  std::vector<MPI_Request> rreq(nneigh, MPI_REQUEST_NULL);
  std::vector<int> nrecvpart(nneigh, 0);

  async_grid_get_neighbor_ranks(neighrank);

  for (int i = 0; i < nneigh; ++i)
    async_grid_get_displacement_of_neighbor_index(i, neighdisp[i]);

  while (oob_particles_exist) {
    oob_particles_exist = 0;

    for (int i = 0; i < nneigh; ++i) {
      init_particlelist(&sendbuf[i]);
      init_particlelist(&recvbuf[i]);
      sendbuf_dyn[i].clear(); // Important if nexchanges > 1
      MPI_Irecv(&nrecvpart[i], 1, MPI_INT, neighrank[i], async_comm_get_tag(1, neighdisp[i]), comm_cart, &rreq[i]);
    }

    dd_async_exchange_fill_sendbufs(sendbuf, sendbuf_dyn);

    // Send particle lists
    for (int i = 0; i < nneigh; ++i) {
      int tag = async_comm_get_tag(0, neighdisp[i]);
      // If we didn't send the length here, we would need a MPI_Iprobe loop for reception
      MPI_Isend(&sendbuf[i].n, 1, MPI_INT, neighrank[i], tag, comm_cart, &sreq[3*i]);
      MPI_Isend(sendbuf[i].part, sendbuf[i].n * sizeof(Particle), MPI_BYTE, neighrank[i], tag, comm_cart, &sreq[3*i+1]);
      if (sendbuf_dyn[i].size() > 0)
        MPI_Isend(sendbuf_dyn[i].data(), sendbuf_dyn[i].size(), MPI_INT, neighrank[i], tag, comm_cart, &sreq[3*i+2]);
    }

    // Only resort local particles in the first iteration
    if (nexchanges == 0)
      dd_resort_particles();

    // Receive all data
    // Successive IRecvs for the same (source, tag) pair replace the receive
    // request in rreq. MPI ensures ordered communication for the same (source,
    // tag) pairs. The vector "recvs" captures the reception state of a (source,
    // tag) pair by its index in the receive buffer.
    MPI_Status status;
    std::vector<int> recvs(nneigh, 0);
    int recvidx, tag, source;
    while (true) {
      MPI_Waitany(nneigh, rreq.data(), &recvidx, &status);
      if (recvidx == MPI_UNDEFINED)
        break;

      source = status.MPI_SOURCE; // == neighrank[recvidx]
      tag = status.MPI_TAG;

      if (recvs[recvidx] == 0) {
        // Size received
        realloc_particlelist(&recvbuf[recvidx], nrecvpart[recvidx]);
        MPI_Irecv(recvbuf[recvidx].part, nrecvpart[recvidx] * sizeof(Particle), MPI_BYTE, source, tag, comm_cart, &rreq[recvidx]);
      } else if (recvs[recvidx] == 1) {
        // Particles received
        recvbuf[recvidx].n = nrecvpart[recvidx];
        int dyndatasiz;
        oob_particles_exist |= dd_async_exchange_insert_particles(&recvbuf[recvidx], &dyndatasiz);
        if (dyndatasiz > 0) {
          recvbuf_dyn[recvidx].resize(dyndatasiz);
          MPI_Irecv(recvbuf_dyn[recvidx].data(), dyndatasiz, MPI_INT, source, tag, comm_cart, &rreq[recvidx]);
        }
      } else {
        dd_async_exchange_insert_dyndata(&recvbuf[recvidx], recvbuf_dyn[recvidx]);
      }
      recvs[recvidx]++;
    }

    MPI_Waitall(3 * nneigh, sreq.data(), MPI_STATUS_IGNORE);

    // Remove particles from this nodes local list
    for (int i = 0; i < nneigh; ++i) {
      for (int p = 0; p < sendbuf[i].n; p++) {
        free_particle(&sendbuf[i].part[p]);
      }
      realloc_particlelist(&sendbuf[i], 0);
      realloc_particlelist(&recvbuf[i], 0);
    }

    if (!global_flag && oob_particles_exist) {
      fprintf(stderr, "[Rank %i] OOB particle received but no global exchange.\n", this_node);
      errexit();
    }
    MPI_Allreduce(MPI_IN_PLACE, &oob_particles_exist, 1, MPI_INT, MPI_MAX, comm_cart);

    nexchanges++;
  }

#ifdef ADDITIONAL_CHECKS
  check_particle_consistency();
#endif

  CELL_TRACE(fprintf(stderr,"%d: dd_exchange_and_sort_particles finished\n",this_node));
}

void  dd_exchange_and_sort_particles(int global_flag)
{
  dd_async_exchange_and_sort_particles(global_flag);
}

/*************************************************/

int calc_processor_min_num_cells()
{
  int i, min = 1;
  /* the minimal number of cells can be lower if there are at least two nodes serving a direction,
     since this also ensures that the cell size is at most half the box length. However, if there is
     only one processor for a direction, there have to be at least two cells for this direction. */
  for (i = 0; i < 3; i++) 
    if (node_grid[i] == 1) 
      min *= 2;
  return min;
}

void calc_link_cell()
{
  int c, np1, n, np2, i ,j, j_start;
  Cell *cell;
  IA_Neighbor *neighbor;
  Particle *p1, *p2;
  double dist2, vec21[3];

  /* Loop local cells */
  for (c = 0; c < local_cells.n; c++) {

    cell = local_cells.cell[c];
    p1   = cell->part;
    np1  = cell->n;

    /* Loop cell neighbors */
    for (n = 0; n < dd.cell_inter[c].n_neighbors; n++) {
      neighbor = &dd.cell_inter[c].nList[n];
      p2  = neighbor->pList->part;
      np2 = neighbor->pList->n;
      /* Loop cell particles */
      for(i=0; i < np1; i++) {
	j_start = 0;
	/* Tasks within cell: bonded forces */
	if(n == 0) {
          add_single_particle_force(&p1[i]);
	  if (rebuild_verletlist)
	    memcpy(p1[i].l.p_old, p1[i].r.p, 3*sizeof(double));

	  j_start = i+1;
	}
	/* Loop neighbor cell particles */
	for(j = j_start; j < np2; j++) {
#ifdef EXCLUSIONS
          if(do_nonbonded(&p1[i], &p2[j]))
#endif
	    {
	      dist2 = distance2vec(p1[i].r.p, p2[j].r.p, vec21);
	      add_non_bonded_pair_force(&(p1[i]), &(p2[j]), vec21, sqrt(dist2), dist2);
	    }
	}
      }
    }
  }
  rebuild_verletlist = 0;
}

/************************************************************/

void calculate_link_cell_energies()
{
  int c, np1, np2, n, i, j, j_start;
  Cell *cell;
  IA_Neighbor *neighbor;
  Particle *p1, *p2;
  double dist2, vec21[3];

  CELL_TRACE(fprintf(stderr,"%d: calculate link-cell energies\n",this_node));

  /* Loop local cells */
  for (c = 0; c < local_cells.n; c++) {
    cell = local_cells.cell[c];
    p1   = cell->part;
    np1  = cell->n;
    /* calculate bonded interactions (loop local particles) */
    for(i = 0; i < np1; i++)  {
      add_single_particle_energy(&p1[i]);
      
      if (rebuild_verletlist)
        memcpy(p1[i].l.p_old, p1[i].r.p, 3*sizeof(double));
    }

    CELL_TRACE(fprintf(stderr,"%d: cell %d with %d neighbors\n",this_node,c, dd.cell_inter[c].n_neighbors));
    /* Loop cell neighbors */
    for (n = 0; n < dd.cell_inter[c].n_neighbors; n++) {
      neighbor = &dd.cell_inter[c].nList[n];
      p2  = neighbor->pList->part;
      np2 = neighbor->pList->n;
      /* Loop cell particles */
      for(i=0; i < np1; i++) {
	j_start = 0;
	if(n == 0) j_start = i+1;
	/* Loop neighbor cell particles */
	for(j = j_start; j < np2; j++) {	
#ifdef EXCLUSIONS
          if(do_nonbonded(&p1[i], &p2[j]))
#endif
	    {
	      dist2 = distance2vec(p1[i].r.p, p2[j].r.p, vec21);
	      add_non_bonded_pair_energy(&(p1[i]), &(p2[j]), vec21, sqrt(dist2), dist2);
	    }
	}
      }
    }
  }
  rebuild_verletlist = 0;
}

/************************************************************/

void calculate_link_cell_virials(int v_comp)
{
  int c, np1, np2, n, i, j, j_start;
  Cell *cell;
  IA_Neighbor *neighbor;
  Particle *p1, *p2;
  double dist2, vec21[3];

  CELL_TRACE(fprintf(stderr,"%d: calculate link-cell energies\n",this_node));

  /* Loop local cells */
  for (c = 0; c < local_cells.n; c++) {
    cell = local_cells.cell[c];
    p1   = cell->part;
    np1  = cell->n;
    /* calculate bonded interactions (loop local particles) */
    for(i = 0; i < np1; i++)  {
      add_kinetic_virials(&p1[i], v_comp);
      add_bonded_virials(&p1[i]);
#ifdef BOND_ANGLE_OLD
      add_three_body_bonded_stress(&p1[i]);
#endif
#ifdef BOND_ANGLE
      add_three_body_bonded_stress(&p1[i]);
#endif
      if (rebuild_verletlist)
        memcpy(p1[i].l.p_old, p1[i].r.p, 3*sizeof(double));
    }

    CELL_TRACE(fprintf(stderr,"%d: cell %d with %d neighbors\n",this_node,c, dd.cell_inter[c].n_neighbors));
    /* Loop cell neighbors */
    for (n = 0; n < dd.cell_inter[c].n_neighbors; n++) {
      neighbor = &dd.cell_inter[c].nList[n];
      p2  = neighbor->pList->part;
      np2 = neighbor->pList->n;
      /* Loop cell particles */
      for(i=0; i < np1; i++) {
	j_start = 0;
	if(n == 0) j_start = i+1;
	/* Loop neighbor cell particles */
	for(j = j_start; j < np2; j++) {	
#ifdef EXCLUSIONS
          if(do_nonbonded(&p1[i], &p2[j]))
#endif
	    {
	      dist2 = distance2vec(p1[i].r.p, p2[j].r.p, vec21);
	      add_non_bonded_pair_virials(&(p1[i]), &(p2[j]), vec21, sqrt(dist2), dist2);
	    }
	}
      }
    }
  }
  rebuild_verletlist = 0;
}

/************************************************************/
