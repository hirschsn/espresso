/** \file fft.c
 *
 *  Routines, row decomposition, data structures and communication for the 3D-FFT. 
 *
  *  For more information about FFT usage, see \ref fft.h "fft.h".  
*/
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> 
#include <math.h>

#include <fftw.h>
#include <rfftw.h>

#include "communication.h"
#include "grid.h"
#include "debug.h"
#include "utils.h"
#include "fft.h"
#include "p3m.h"

/************************************************
 * DEFINES
 ************************************************/

/* MPI tags for the fft communications: */
/** Tag for communication in fft_init() */
#define REQ_FFT_INIT   300
/** Tag for communication in forw_grid_comm() */
#define REQ_FFT_FORW   301
/** Tag for communication in back_grid_comm() */
#define REQ_FFT_BACK   302


/************************************************
 * variables
 ************************************************/

fft_forw_plan fft_plan[4];

/** Information for Back FFTs (see fft_plan). */
fft_back_plan fft_back[4];

/** Maximal size of the communication buffers. */
static int max_comm_size=0;
/** Maximal local mesh size. */
static int max_mesh_size=0;
/** send buffer. */
static double *send_buf = NULL;
/** receive buffer. */
static double *recv_buf = NULL;
/** Buffer for receive data. */
static double *data_buf = NULL;
/** Complex data pointers. */
static fftw_complex *c_data;
static fftw_complex *c_data_buf;


/** \name Privat Functions */
/************************************************************/
/*@{*/

/** This ugly function does the bookkepping which nodes have to
 *  communicate to each other, when you change the node grid.
 *  Changing the domain decomposition requieres communication. This
 *  function finds (hopefully) the best way to do this. As input it
 *  needs the two grids (grid1, grid2) and a linear list (node_list1)
 *  with the node identities for grid1. The linear list (node_list2)
 *  for the second grid is calculated. For the communication group of
 *  the calling node it calculates a list (group) with the node
 *  identities and the positions (pos1, pos2) of that nodes in grid1
 *  and grid2. The return value is the size of the communication
 *  group. It gives -1 if the two grids do not fit to each other
 *  (grid1 and grid2 have to be component wise multiples of each
 *  other. see e.g. \ref calc_2d_grid in \ref grid.c for how to do
 *  this.).
 *
 * \param grid1       The node grid you start with (Input).
 * \param grid2       The node grid you want to have (Input).
 * \param node_list1  Linear node index list for grid1 (Input).
 * \param node_list2  Linear node index list for grid2 (Output).
 * \param group       communication group (node identity list) for the calling node  (Output).
 * \param pos        positions of the nodes in in grid2 (Output).
 * \param my_pos      position of this_node in  grid2.
 * \return Size of the communication group (Output of course!).  */
int find_comm_groups(int grid1[3], int grid2[3], int *node_list1, int *node_list2, 
		     int *group, int *pos, int *my_pos);


/** Calculate the local fft mesh.  Calculate the local mesh (loc_mesh)
 *  of a node at position (n_pos) in a node grid (n_grid) for a global
 *  mesh of size (mesh) and a mesh offset (mesh_off (in mesh units))
 *  and store also the first point (start) of the local mesh.
 *
 * \return size        number of mesh points in local mesh.
 * \param  n_pos[3]    Position of the node in n_grid.
 * \param  n_grid[3]   node grid.
 * \param  mesh[3]     global mesh dimensions.
 * \param  mesh_off[3] global mesh offset (see \ref p3m_struct).
 * \param  loc_mesh[3] local mesh dimension (output).
 * \param  start[3]    first point of local mesh in global mesh (output).
*/
int calc_local_mesh(int n_pos[3], int n_grid[3], int mesh[3], double mesh_off[3], 
		     int loc_mesh[3], int start[3]);

/** Calculate a send (or recv.) block for grid communication during a
 *  decomposition change.  Calculate the send block specification
 *  (block = lower left corner and upper right corner) which a node at
 *  position (pos1) in the actual node grid (grid1) has to send to
 *  another node at position (pos2) in the desired node grid
 *  (grid2). The global mesh, subject to communication, is specified
 *  via its size (mesh) and its mesh offset (mesh_off (in mesh
 *  units)).
 *
 *  For the calculation of a receive block you have to change the arguments in the following way:\\
 *  pos1  - position of receiving node in the desired node grid.\\
 *  grid1 - desired node grid.\\
 *  pos2  - position of the node you intend to receive the data from in the actual node grid.\\
 *  grid2 - actual node grid.  
 *
 *  \return     size of the send block.
 *  \param  pos1[3]     Position of send node in grid1.
 *  \param  grid1[3]    node grid 1.
 *  \param  pos2[3]     Position of recv node in grid2.
 *  \param  grid2[3]    node grid 2.
 *  \param  mesh[3]     global mesh dimensions.
 *  \param  mesh_off[3] global mesh offset (see \ref p3m_struct).
 *  \param  block[6]    send block specification.
*/
int calc_send_block(int pos1[3], int grid1[3], int pos2[3], int grid2[3], 
		    int mesh[3], double mesh_off[3], int block[6]);

/** communicate the grid data according to the given fft_forw_plan. 
 * \param plan communication plan (see \ref fft_forw_plan).
 * \param in   input mesh.
 * \param out  output mesh.
*/
void forw_grid_comm(fft_forw_plan plan, double *in, double *out);

/** communicate the grid data according to the given fft_forw_plan/fft_bakc_plan. 
 * \param plan_f communication plan (see \ref fft_forw_plan).
 * \param plan_b additional back plan (see \ref fft_back_plan).
 * \param in     input mesh.
 * \param out    output mesh.
*/
void back_grid_comm(fft_forw_plan plan_f, fft_back_plan plan_b, double *in, double *out);

/** Debug function to print fft_forw_plan structure. 
 * \param plan fft/communication plan (see \ref fft_forw_plan).
 */
void print_fft_plan(fft_forw_plan pl);
/** Debug function to print global fft mesh. 
    Print a globaly distributed mesh contained in data. Element size is element. 
 * \param plan     fft/communication plan (see \ref fft_forw_plan).
 * \param data     mesh data.
 * \param element  element size.
 * \param num      element index to print.
*/
void print_global_fft_mesh(fft_forw_plan plan, double *data, int element, int num);

/*@}*/
/************************************************************/

void fft_pre_init()
{
  int i;

  for(i=0;i<4;i++) {
    fft_plan[i].group = malloc(1*n_nodes*sizeof(int));
    fft_plan[i].send_block = NULL;
    fft_plan[i].send_size  = NULL;
    fft_plan[i].recv_block = NULL;
    fft_plan[i].recv_size  = NULL;
  }

}

int fft_init(double *data, int *ca_mesh_dim, int *ca_mesh_margin)
{
  int i,j;
  /* helpers */
  int mult[3];

  int n_grid[4][3]; /* The four node grids. */
  int my_pos[4][3]; /* The position of this_node in the node grids. */
  int *n_id[4];     /* linear node identity lists for the node grids. */
  int *n_pos[4];    /* positions of nodes in the node grids. */
  /* FFTW WISDOM stuff. */
  char wisdom_file_name[255];
  FILE *wisdom_file;
  fftw_status wisdom_status;

  FFT_TRACE(fprintf(stderr,"%d: fft_init():\n",this_node));

  for(i=0;i<4;i++) {
    n_id[i]  = malloc(1*n_nodes*sizeof(int));
    n_pos[i] = malloc(3*n_nodes*sizeof(int));
  }

  /* === node grids === */
  /* real space node grid (n_grid[0]) */
  for(i=0;i<3;i++) {
    n_grid[0][i] = node_grid[i];
    my_pos[0][i] = node_pos[i];
  }
  for(i=0;i<n_nodes;i++) {
    n_id[0][i] = i;
    get_grid_pos(i,&(n_pos[0][3*i+0]),&(n_pos[0][3*i+1]),&(n_pos[0][3*i+2]),
		 n_grid[0]);
  }
    
  /* FFT node grids (n_grid[1 - 3]) */
  calc_2d_grid(n_nodes,n_grid[1]);
  /* resort n_grid[1] dimensions if necessary */
  fft_plan[1].row_dir = map_3don2d_grid(n_grid[0], n_grid[1], mult);
  fft_plan[0].n_permute = 0;
  for(i=1;i<4;i++) fft_plan[i].n_permute = (fft_plan[1].row_dir+i)%3;
  for(i=0;i<3;i++) {
    n_grid[2][i] = n_grid[1][(i+1)%3];
    n_grid[3][i] = n_grid[1][(i+2)%3];
  }
  fft_plan[2].row_dir = (fft_plan[1].row_dir-1)%3;
  fft_plan[3].row_dir = (fft_plan[1].row_dir-2)%3;

  /* === communication groups === */
  /* copy local mesh off real space charge assignment grid */
  for(i=0;i<3;i++) fft_plan[0].new_mesh[i] = ca_mesh_dim[i];
  for(i=1; i<4;i++) {
    fft_plan[i].g_size=find_comm_groups(n_grid[i-1], n_grid[i], n_id[i-1], n_id[i], 
					fft_plan[i].group, n_pos[i], my_pos[i]);

    fft_plan[i].send_block = (int *)realloc(fft_plan[i].send_block, 6*fft_plan[i].g_size*sizeof(int));
    fft_plan[i].send_size  = (int *)realloc(fft_plan[i].send_size, 1*fft_plan[i].g_size*sizeof(int));
    fft_plan[i].recv_block = (int *)realloc(fft_plan[i].recv_block, 6*fft_plan[i].g_size*sizeof(int));
    fft_plan[i].recv_size  = (int *)realloc(fft_plan[i].recv_size, 1*fft_plan[i].g_size*sizeof(int));

    fft_plan[i].new_size = calc_local_mesh(my_pos[i], n_grid[i], p3m.mesh,
					   p3m.mesh_off, fft_plan[i].new_mesh, 
					   fft_plan[i].start);  
    permute_ifield(fft_plan[i].new_mesh,3,-(fft_plan[i].n_permute));
    permute_ifield(fft_plan[i].start,3,-(fft_plan[i].n_permute));
    fft_plan[i].n_ffts = fft_plan[i].new_mesh[0]*fft_plan[i].new_mesh[1];

    /* === send/recv block specifications === */
    for(j=0; j<fft_plan[i].g_size; j++) {
      int k, node;
      /* send block: this_node to comm-group-node i (identity: node) */
      node = fft_plan[i].group[j];
      fft_plan[i].send_size[j] 
	= calc_send_block(my_pos[i-1], n_grid[i-1], &(n_pos[i][3*node]), n_grid[i],
			  p3m.mesh, p3m.mesh_off, &(fft_plan[i].send_block[6*j]));
      permute_ifield(&(fft_plan[i].send_block[6*j]),3,-(fft_plan[i-1].n_permute));
      permute_ifield(&(fft_plan[i].send_block[6*j+3]),3,-(fft_plan[i-1].n_permute));
      if(fft_plan[i].send_size[j] > max_comm_size) 
	max_comm_size = fft_plan[i].send_size[j];
      /* First plan send blocks have to be adjusted, since the CA grid
	 may have an additional margin outside the actual domain of the
	 node */
      if(i==1) {
	for(k=0;k<3;k++) 
	  fft_plan[1].send_block[6*j+k  ] += ca_mesh_margin[2*k];
      }
      /* recv block: this_node from comm-group-node i (identity: node) */
      fft_plan[i].recv_size[j] 
	= calc_send_block(my_pos[i], n_grid[i], &(n_pos[i-1][3*node]), n_grid[i-1],
			  p3m.mesh,p3m.mesh_off,&(fft_plan[i].recv_block[6*j]));
      permute_ifield(&(fft_plan[i].recv_block[6*j]),3,-(fft_plan[i].n_permute));
      permute_ifield(&(fft_plan[i].recv_block[6*j+3]),3,-(fft_plan[i].n_permute));
      if(fft_plan[i].recv_size[j] > max_comm_size) 
	max_comm_size = fft_plan[i].recv_size[j];
    }

    for(j=0;j<3;j++) fft_plan[i].old_mesh[j] = fft_plan[i-1].new_mesh[j];
    if(i==1) 
      fft_plan[i].element = 1; 
    else {
      fft_plan[i].element = 2;
      for(j=0; j<fft_plan[i].g_size; j++) {
	fft_plan[i].send_size[j] *= 2;
	fft_plan[i].recv_size[j] *= 2;
      }
    }
    /* DEBUG */
    for(j=0;j<n_nodes;j++) {
      /* MPI_Barrier(MPI_COMM_WORLD); */
      if(j==this_node) FFT_TRACE(print_fft_plan(fft_plan[i]));
    }
  }

  /* Factor 2 for complex fields */
  max_comm_size *= 2;
  max_mesh_size = (ca_mesh_dim[0]*ca_mesh_dim[1]*ca_mesh_dim[2]);
  for(i=1;i>4;i++) 
    if(2*fft_plan[i].new_size > max_mesh_size) max_mesh_size = 2*fft_plan[i].new_size;

  FFT_TRACE(fprintf(stderr,"%d: max_comm_size = %d, max_mesh_size = %d\n",
		    this_node,max_comm_size,max_mesh_size));

  /* === pack function === */
  for(i=1;i<4;i++) fft_plan[i].pack_function = pack_block_permute2;
  if(fft_plan[1].row_dir==2) fft_plan[1].pack_function = pack_block;
  else if(fft_plan[1].row_dir==1) fft_plan[1].pack_function = pack_block_permute1;

  /* Factor 2 for complex numbers */
  send_buf = (double *)realloc(send_buf, max_comm_size*sizeof(double));
  recv_buf = (double *)realloc(recv_buf, max_comm_size*sizeof(double));
  data     = (double *)realloc(data, max_mesh_size*sizeof(double));
  data_buf = (double *)realloc(data_buf, max_mesh_size*sizeof(double));
  if(!data || !data_buf || !recv_buf || !send_buf) 
    fprintf(stderr,"%d: Could not allocate FFT data arays\n",this_node);

  c_data     = (fftw_complex *) data;
  c_data_buf = (fftw_complex *) data_buf;

  /* === FFT Routines (Using FFTW / RFFTW package)=== */
  for(i=1;i<4;i++) {
    fft_plan[i].dir = FFTW_FORWARD;   
    /* FFT plan creation. 
       Attention: destroys contents of c_data/data and c_data_buf/data_buf. */
    wisdom_status   = FFTW_FAILURE;
    sprintf(wisdom_file_name,"fftw_1d_wisdom_forw_n%d.file",
	    fft_plan[i].new_mesh[2]);
    if( (wisdom_file=fopen(wisdom_file_name,"r"))!=NULL ) {
      wisdom_status = fftw_import_wisdom_from_file(wisdom_file);
      fclose(wisdom_file);
    }
      fft_plan[i].fft_plan = 
	fftw_create_plan_specific(fft_plan[i].new_mesh[2], fft_plan[i].dir,
				  FFTW_MEASURE | FFTW_IN_PLACE | FFTW_USE_WISDOM,
				  c_data, 1,c_data_buf, 1);

    if( wisdom_status == FFTW_FAILURE && 
	(wisdom_file=fopen(wisdom_file_name,"w"))!=NULL ) {
      fftw_export_wisdom_to_file(wisdom_file);
      fclose(wisdom_file);
    }

    fft_plan[i].fft_function = fftw;       
  }

  /* === The BACK Direction === */
  /* this is needed because slightly different functions are used */
  for(i=1;i<4;i++) {
    fft_back[i].dir = FFTW_BACKWARD;
    wisdom_status   = FFTW_FAILURE;
    sprintf(wisdom_file_name,"fftw_1d_wisdom_back_n%d.file",
	    fft_plan[i].new_mesh[2]);
    if( (wisdom_file=fopen(wisdom_file_name,"r"))!=NULL ) {
      wisdom_status = fftw_import_wisdom_from_file(wisdom_file);
      fclose(wisdom_file);
    }    
    fft_back[i].fft_plan = 
      fftw_create_plan_specific(fft_plan[i].new_mesh[2], fft_back[i].dir,
				FFTW_MEASURE | FFTW_IN_PLACE | FFTW_USE_WISDOM,
				c_data, 1,c_data_buf, 1);
    if( wisdom_status == FFTW_FAILURE && 
	(wisdom_file=fopen(wisdom_file_name,"w"))!=NULL ) {
      fftw_export_wisdom_to_file(wisdom_file);
      fclose(wisdom_file);
    }
    fft_back[i].fft_function = fftw;
    fft_back[i].pack_function = pack_block_permute1;
  }
  if(fft_plan[1].row_dir==2) fft_back[1].pack_function = pack_block;
  else if(fft_plan[1].row_dir==1) fft_back[1].pack_function = pack_block_permute2;

  free(data);
  for(i=0;i<4;i++) { free(n_id[i]); free(n_pos[i]); }
  return max_mesh_size; 
}

void fft_perform_forw(double *data)
{
  int i;

  /* ===== first direction  ===== */
  FFT_TRACE(fprintf(stderr,"%d: fft_perform_forw: dir 1:\n",this_node));

  c_data     = (fftw_complex *) data;
  c_data_buf = (fftw_complex *) data_buf;

  /* communication to current dir row format (in is data) */
  forw_grid_comm(fft_plan[1], data, data_buf);
  /* complexify the real data array (in is data_buf) */
  for(i=0;i<fft_plan[1].new_size;i++) {
    data[2*i]     = data_buf[i];     /* real value */
    data[(2*i)+1] = 0;       /* complex value */
  }
  /* perform FFT (in/out is data)*/
  fft_plan[1].fft_function(fft_plan[1].fft_plan, fft_plan[1].n_ffts,
  			   c_data, 1, fft_plan[1].new_mesh[2],
  			   c_data_buf, 1, fft_plan[1].new_mesh[2]);
  
  /* ===== second direction ===== */
  FFT_TRACE(fprintf(stderr,"%d: fft_perform_forw: dir 2:\n",this_node));
  /* communication to current dir row format (in is data) */
  forw_grid_comm(fft_plan[2], data, data_buf);
  /* perform FFT (in/out is data_buf)*/
  fft_plan[2].fft_function(fft_plan[2].fft_plan, fft_plan[2].n_ffts,
  			   c_data_buf, 1, fft_plan[2].new_mesh[2],
  			   c_data, 1, fft_plan[2].new_mesh[2]);

  /* ===== third direction  ===== */
  FFT_TRACE(fprintf(stderr,"%d: fft_perform_forw: dir 3:\n",this_node));
  /* communication to current dir row format (in is data_buf) */
  forw_grid_comm(fft_plan[3], data_buf, data);
  /* perform FFT (in/out is data)*/
  fft_plan[3].fft_function(fft_plan[3].fft_plan, fft_plan[3].n_ffts,
  			   c_data, 1, fft_plan[3].new_mesh[2],
  			   c_data_buf, 1, fft_plan[3].new_mesh[2]);

  /* REMARK: Result has to be in data. */
}

void fft_perform_back(double *data)
{
  int i;
  /* ===== third direction  ===== */
  FFT_TRACE(fprintf(stderr,"%d: fft_perform_back: dir 3:\n",this_node));
  /* perform FFT (in is data) */
  fft_back[3].fft_function(fft_back[3].fft_plan, fft_plan[3].n_ffts,
  			   c_data, 1, fft_plan[3].new_mesh[2],
  			   c_data_buf, 1, fft_plan[3].new_mesh[2]);
  /* communicate (in is data)*/
  back_grid_comm(fft_plan[3],fft_back[3],data,data_buf);
 
  /* ===== second direction ===== */
  FFT_TRACE(fprintf(stderr,"%d: fft_perform_back: dir 2:\n",this_node));
  /* perform FFT (in is data_buf) */
  fft_back[2].fft_function(fft_back[2].fft_plan, fft_plan[2].n_ffts,
  			   c_data_buf, 1, fft_plan[2].new_mesh[2],
  			   c_data, 1, fft_plan[2].new_mesh[2]);
  /* communicate (in is data_buf) */
  back_grid_comm(fft_plan[2],fft_back[2],data_buf,data);

  /* ===== first direction  ===== */
  FFT_TRACE(fprintf(stderr,"%d: fft_perform_back: dir 1:\n",this_node));
  /* perform FFT (in is data) */
  fft_back[1].fft_function(fft_back[1].fft_plan, fft_plan[1].n_ffts,
  			   c_data, 1, fft_plan[1].new_mesh[2],
  			   c_data_buf, 1, fft_plan[1].new_mesh[2]);
  /* through away the (hopefully) empty complex component (in is data)*/
  for(i=0;i<fft_plan[1].new_size;i++) {
    data_buf[i] = data[2*i]; /* real value */
  }
  /* communicate (in is data_buf) */
  back_grid_comm(fft_plan[1],fft_back[1],data_buf,data);

  /* REMARK: Result has to be in data. */
}

void pack_block(double *in, double *out, int start[3], int size[3], int dim[3], int element)
{
  /* mid and slow changing indices */
  int m,s;
  /* linear index of in grid, linear index of out grid */
  int li_in,li_out=0;
  /* copy size */
  int copy_size;
  /* offsets for indizes in input grid */
  int m_in_offset,s_in_offset;
  /* offsets for indizes in output grid */
  int m_out_offset;

  copy_size    = element * size[2] * sizeof(double);
  m_in_offset  = element * dim[2];
  s_in_offset  = element * (dim[2] * (dim[1] - size[1]));
  m_out_offset = element * size[2];
  li_in        = element * (start[2]+dim[2]*(start[1]+dim[1]*start[0]));

  for(s=0 ;s<size[0]; s++) {
    for(m=0; m<size[1]; m++) {
      memcpy(&(out[li_out]), &(in[li_in]), copy_size);
      li_in  += m_in_offset;
      li_out += m_out_offset;
    }
    li_in += s_in_offset;
  }
}

void pack_block_permute1(double *in, double *out, int start[3], int size[3], 
			 int dim[3], int element)
{
  /* slow,mid and fast changing indices for input  grid */
  int s,m,f,e;
  /* linear index of in grid, linear index of out grid */
  int li_in,li_out=0;
  /* offsets for indizes in input grid */
  int m_in_offset,s_in_offset;
  /* offset for mid changing indices of output grid */
  int m_out_offset;

  m_in_offset  =  element * (dim[2] - size[2]);
  s_in_offset  =  element * (dim[2] * (dim[1] - size[1]));
  m_out_offset = (element * size[0]) - element;
  li_in        =  element * (start[2]+dim[2]*(start[1]+dim[1]*start[0]));

  for(s=0 ;s<size[0]; s++) {      /* fast changing out */
    li_out = element*s;
    for(m=0; m<size[1]; m++) {    /* slow changing out */
      for(f=0 ;f<size[2]; f++) {  /* mid  changing out */
	for(e=0; e<element; e++) out[li_out++] = in[li_in++];
	li_out += m_out_offset;
      }
      li_in  += m_in_offset;
    }
    li_in += s_in_offset;
  }

}

void pack_block_permute2(double *in, double *out, int start[3], int size[3], 
			 int dim[3],int element)
{
  /* slow,mid and fast changing indices for input  grid */
  int s,m,f,e;
  /* linear index of in grid, linear index of out grid */
  int li_in,li_out=0;
  /* offsets for indizes in input grid */
  int m_in_offset,s_in_offset;
  /* offset for slow changing index of output grid */
  int s_out_offset;
  /* start index for mid changing index of output grid */
  int m_out_start;

  m_in_offset  = element * (dim[2]-size[2]);
  s_in_offset  = element * (dim[2] * (dim[1]-size[1]));
  s_out_offset = (element * size[0] * size[1]) - element;
  li_in        = element * (start[2]+dim[2]*(start[1]+dim[1]*start[0]));

  for(s=0 ;s<size[0]; s++) {      /* mid changing out */
    m_out_start = element*(s * size[1]);
    for(m=0; m<size[1]; m++) {    /* fast changing out */
      li_out = m_out_start + element*m;
      for(f=0 ;f<size[2]; f++) {  /* slow  changing out */
	for(e=0; e<element; e++) out[li_out++] = in[li_in++];
	li_out += s_out_offset;
      }
      li_in += m_in_offset; 
    }
    li_in += s_in_offset; 
  }

}

void unpack_block(double *in, double *out, int start[3], int size[3], 
		  int dim[3], int element)
{
  /* mid and slow changing indices */
  int m,s;
  /* linear index of in grid, linear index of out grid */
  int li_in=0,li_out;
  /* copy size */
  int copy_size;
  /* offset for indizes in input grid */
  int m_in_offset;
  /* offsets for indizes in output grid */
  int m_out_offset,s_out_offset;

  copy_size    = element * (size[2] * sizeof(double));
  m_out_offset = element * dim[2];
  s_out_offset = element * (dim[2] * (dim[1] - size[1]));
  m_in_offset  = element * size[2];
  li_out       = element * (start[2]+dim[2]*(start[1]+dim[1]*start[0]));

  for(s=0 ;s<size[0]; s++) {
    for(m=0; m<size[1]; m++) {
      memcpy(&(out[li_out]), &(in[li_in]), copy_size);
      li_in  += m_in_offset;
      li_out += m_out_offset;
    }
    li_out += s_out_offset;
  }

}

/************************************************
 * privat functions
 ************************************************/

int find_comm_groups(int grid1[3], int grid2[3], int *node_list1, int *node_list2, 
		     int *group, int *pos, int *my_pos)
{
  int i;
  /* communication group cell size on grid1 and grid2 */
  int s1[3], s2[3];
  /* The communication group cells build the same super grid on grid1 and grid2 */
  int ds[3];
  /* communication group size */
  int g_size=1;
  /* comm. group cell index */
  int gi[3];
  /* position of a node in a grid */
  int p1[3], p2[3];
  /* node identity */
  int n;
  /* this_node position in the communication group. */
  int c_pos=-1;
  /* flag for group identification */
  int my_group=0;

  FFT_TRACE(fprintf(stderr,"%d: find_comm_groups:\n",this_node));

  /* calculate dimension of comm. group cells for both grids */ 
  if( (grid1[0]*grid1[1]*grid1[2]) != (grid2[0]*grid2[1]*grid2[2]) ) return -1; /* unlike number of nodes */
  for(i=0;i<3;i++) {
    s1[i] = grid1[i] / grid2[i];
    if(s1[i] == 0) s1[i] = 1;
    else if(grid1[i] != grid2[i]*s1[i]) return -1; /* grids do not match!!! */

    s2[i] = grid2[i] / grid1[i];
    if(s2[i] == 0) s2[i] = 1;
    else if(grid2[i] != grid1[i]*s2[i]) return -1; /* grids do not match!!! */

    ds[i] = grid2[i] / s2[i]; 
    g_size *= s2[i];
  }

  /* calc node_list2 */
  /* loop through all comm. group cells */
  for(gi[2] = 0; gi[2] < ds[2]; gi[2]++) 
    for(gi[1] = 0; gi[1] < ds[1]; gi[1]++)
      for(gi[0] = 0; gi[0] < ds[0]; gi[0]++) {
	/* loop through all nodes in that comm. group cell */
	for(i=0;i<g_size;i++) {
	  p1[0] = (gi[0]*s1[0]) + (i%s1[0]);
	  p1[1] = (gi[1]*s1[1]) + ((i/s1[0])%s1[1]);
	  p1[2] = (gi[2]*s1[2]) + (i/(s1[0]*s1[1]));

	  p2[0] = (gi[0]*s2[0]) + (i%s2[0]);
	  p2[1] = (gi[1]*s2[1]) + ((i/s2[0])%s2[1]);
	  p2[2] = (gi[2]*s2[2]) + (i/(s2[0]*s2[1]));

	  n = node_list1[ get_linear_index(p1[0],p1[1],p1[2],grid1) ];
	  node_list2[ get_linear_index(p2[0],p2[1],p2[2],grid2) ] = n ;

	  pos[3*n+0] = p2[0];  pos[3*n+1] = p2[1];  pos[3*n+2] = p2[2];	  
	  if(my_group==1) group[i] = n;
	  if(n==this_node && my_group==0) { 
	    my_group = 1; 
	    c_pos = i;
	    my_pos[0] = p2[0]; my_pos[1] = p2[1]; my_pos[2] = p2[2];
	    i=-1; /* restart the loop */ 
	  }
	}
	my_group=0;
      }

  /* permute comm. group according to the nodes position in the group */
  /* This is necessary to have matching node pairs during communication! */
  while( c_pos>0 ) {
    n=group[g_size-1];
    for(i=g_size-1; i>0; i--) group[i] = group[i-1];
    group[0] = n;
    c_pos--;
  }
  return g_size;
}

int calc_local_mesh(int n_pos[3], int n_grid[3], int mesh[3], double mesh_off[3], 
		     int loc_mesh[3], int start[3])
{
  int i, last[3], size=1;
  
  for(i=0;i<3;i++) {
    start[i] = (int)ceil( (mesh[i]/(double)n_grid[i])*n_pos[i]     - mesh_off[i] );
    last[i]  = (int)floor((mesh[i]/(double)n_grid[i])*(n_pos[i]+1) - mesh_off[i] );
    loc_mesh[i] = last[i]-start[i]+1;
    size *= loc_mesh[i];
  }
  return size;
}


int calc_send_block(int pos1[3], int grid1[3], int pos2[3], int grid2[3], 
		    int mesh[3], double mesh_off[3], int block[6])
{
  int i,size=1;
  int mesh1[3], first1[3], last1[3];
  int mesh2[3], first2[3], last2[3];

  calc_local_mesh(pos1, grid1, mesh, mesh_off, mesh1, first1);
  calc_local_mesh(pos2, grid2, mesh, mesh_off, mesh2, first2);

  for(i=0;i<3;i++) {
    last1[i] = first1[i] + mesh1[i] -1;
    last2[i] = first2[i] + mesh2[i] -1;
    block[i  ] = imax(first1[i],first2[i]) - first1[i];
    block[i+3] = (imin(last1[i], last2[i] ) - first1[i])-block[i]+1;
    size *= block[i+3];
  }
  return size;
}

void forw_grid_comm(fft_forw_plan plan, double *in, double *out)
{
  int i;
  MPI_Status status;
  double *tmp_ptr;

  for(i=0;i<plan.g_size;i++) {   
    plan.pack_function(in, send_buf, &(plan.send_block[6*i]), 
		       &(plan.send_block[6*i+3]), plan.old_mesh, plan.element);

    if(plan.group[i]<this_node) {       /* send first, receive second */
      MPI_Send(send_buf, plan.send_size[i], MPI_DOUBLE, 
	       plan.group[i], REQ_FFT_FORW, MPI_COMM_WORLD);
      MPI_Recv(recv_buf, plan.recv_size[i], MPI_DOUBLE, 
	       plan.group[i], REQ_FFT_FORW, MPI_COMM_WORLD, &status); 	
    }
    else if(plan.group[i]>this_node) {  /* receive first, send second */
      MPI_Recv(recv_buf, plan.recv_size[i], MPI_DOUBLE, 
	       plan.group[i], REQ_FFT_FORW, MPI_COMM_WORLD, &status); 	
      MPI_Send(send_buf, plan.send_size[i], MPI_DOUBLE, 
	       plan.group[i], REQ_FFT_FORW, MPI_COMM_WORLD);      
    }
    else {                              /* Self communication... */   
      tmp_ptr  = send_buf;
      send_buf = recv_buf;
      recv_buf = tmp_ptr;
    }
    unpack_block(recv_buf, out, &(plan.recv_block[6*i]), 
		 &(plan.recv_block[6*i+3]), plan.new_mesh, plan.element);
  }
}

void back_grid_comm(fft_forw_plan plan_f,  fft_back_plan plan_b, double *in, double *out)
{
  int i;
  MPI_Status status;
  double *tmp_ptr;

  /* Back means: Use the send/recieve stuff from the forward plan but
     replace the recieve blocks by the send blocks and vice
     versa. Attention then also new_mesh and old_mesh are exchanged */

  for(i=0;i<plan_f.g_size;i++) {
    
    plan_b.pack_function(in, send_buf, &(plan_f.recv_block[6*i]), 
		       &(plan_f.recv_block[6*i+3]), plan_f.new_mesh, plan_f.element);

    if(plan_f.group[i]<this_node) {       /* send first, receive second */
      MPI_Send(send_buf, plan_f.recv_size[i], MPI_DOUBLE, 
	       plan_f.group[i], REQ_FFT_BACK, MPI_COMM_WORLD);
      MPI_Recv(recv_buf, plan_f.send_size[i], MPI_DOUBLE, 
	       plan_f.group[i], REQ_FFT_BACK, MPI_COMM_WORLD, &status); 	
    }
    else if(plan_f.group[i]>this_node) {  /* receive first, send second */
      MPI_Recv(recv_buf, plan_f.send_size[i], MPI_DOUBLE, 
	       plan_f.group[i], REQ_FFT_BACK, MPI_COMM_WORLD, &status); 	
      MPI_Send(send_buf, plan_f.recv_size[i], MPI_DOUBLE, 
	       plan_f.group[i], REQ_FFT_BACK, MPI_COMM_WORLD);      
    }
    else {                                /* Self communication... */   
      tmp_ptr  = send_buf;
      send_buf = recv_buf;
      recv_buf = tmp_ptr;
    }
    unpack_block(recv_buf, out, &(plan_f.send_block[6*i]), 
		 &(plan_f.send_block[6*i+3]), plan_f.old_mesh, plan_f.element);
  }
}

void print_fft_plan(fft_forw_plan pl)
{
  int i;

  fprintf(stderr,"%d: dir=%d, row_dir=%d, n_permute=%d, n_ffts=%d\n",
	  this_node, pl.dir,  pl.row_dir, pl.n_permute, pl.n_ffts);

  fprintf(stderr,"    local: old_mesh=(%d,%d,%d), new_mesh=(%d,%d,%d), start=(%d,%d,%d)\n",
	  pl.old_mesh[0],  pl.old_mesh[1],  pl.old_mesh[2], 
	  pl.new_mesh[0],  pl.new_mesh[1],  pl.new_mesh[2], 
	  pl.start[0], pl.start[1],  pl.start[2]);

  fprintf(stderr,"    g_size=%d group=(",pl.g_size);
  for(i=0;i<pl.g_size-1;i++) fprintf(stderr,"%d,", pl.group[i]);
  fprintf(stderr,"%d)\n",pl.group[pl.g_size-1]);

  fprintf(stderr,"    send=[");
  for(i=0;i<pl.g_size;i++) fprintf(stderr,"(%d,%d,%d)+(%d,%d,%d), ",
				   pl.send_block[6*i+0], pl.send_block[6*i+1], pl.send_block[6*i+2],
				   pl.send_block[6*i+3], pl.send_block[6*i+4], pl.send_block[6*i+5]);
  fprintf(stderr,"]\n    recv=[");
  for(i=0;i<pl.g_size;i++) fprintf(stderr,"(%d,%d,%d)+(%d,%d,%d), ",
				   pl.recv_block[6*i+0], pl.recv_block[6*i+1], pl.recv_block[6*i+2],
				   pl.recv_block[6*i+3], pl.recv_block[6*i+4], pl.recv_block[6*i+5]);
  fprintf(stderr,"]\n");
 
 fflush(stderr);
}

void print_global_fft_mesh(fft_forw_plan plan, double *data, int element, int num)
{
  int i0,i1,i2,b=1;
  int mesh,divide=0,block1=-1,start1;
  int st[3],en[3],si[3];
  int my=-1;
  double tmp;

  for(i1=0;i1<3;i1++) {
    st[i1] = plan.start[i1];
    en[i1] = plan.start[i1]+plan.new_mesh[i1];
    si[i1] = plan.new_mesh[i1];
  }

  mesh = plan.new_mesh[2];
  MPI_Barrier(MPI_COMM_WORLD);  
  if(this_node==0) fprintf(stderr,"All: Print Global Mesh: (%d of %d elements)\n",
			   num+1,element);
  MPI_Barrier(MPI_COMM_WORLD);
  while(divide==0) {
    if(b*mesh > 7) {
      block1=b;
      divide = (int)ceil(mesh/(double)block1);
    }
    b++;
  }

  for(b=0;b<divide;b++) {
    start1 = b*block1;
    for(i0=mesh-1; i0>=0; i0--) {
      for(i1=start1; i1<imin(start1+block1,mesh);i1++) {
	for(i2=0; i2<mesh;i2++) {
	  if(i0>=st[0] && i0<en[0] && i1>=st[1] && 
	     i1<en[1] && i2>=st[2] && i2<en[2]) my=1;
	  else my=0;
	  MPI_Barrier(MPI_COMM_WORLD);
	  if(my==1) {
	   
	    tmp=data[num+(element*((i2-st[2])+si[2]*((i1-st[1])+si[1]*(i0-st[0]))))];
	    if(tmp<0) fprintf(stderr,"%1.2e",tmp);
	    else      fprintf(stderr," %1.2e",tmp);
	  }
	  MPI_Barrier(MPI_COMM_WORLD);
	}
	if(my==1) fprintf(stderr," | ");
      }
      if(my==1) fprintf(stderr,"\n");
    }
    if(my==1) fprintf(stderr,"\n");
  }

}
