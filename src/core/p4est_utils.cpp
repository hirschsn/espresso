#include "p4est_utils.hpp"

#if (defined(LB_ADAPTIVE) || defined(DD_P4EST))

#include "debug.hpp"
#include "domain_decomposition.hpp"
#include "lb-adaptive.hpp"
#include "p4est_dd.hpp"
#include "p4est_gridchange_criteria.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mpi.h>
#include <p8est_algorithms.h>
#include <p8est_bits.h>
#include <p8est_search.h>
#include <vector>

#define USE_VEL_CRIT
#define USE_VORT_CRIT
// #define DUMP_DECISIONS

static std::vector<p4est_utils_forest_info_t> forest_info;

// number of (MD) intergration steps before grid changes
int steps_until_grid_change = -1;

// CAUTION: Do ONLY use this pointer in p4est_utils_adapt_grid
std::vector<int> *flags;

const p4est_utils_forest_info_t &p4est_utils_get_forest_info(forest_order fo) {
  // Use at() here because forest_info might not have been initialized yet.
  return forest_info.at(static_cast<int>(fo));
}

static inline void tree_to_boxlcoords(double x[3]) {
  for (int i = 0; i < 3; ++i)
#ifdef DD_P4EST
    x[i] *= box_l[i] / dd_p4est_num_trees_in_dir(i);
#else  // defined(DD_P4EST)
    x[i] *= box_l[i] / lb_conn_brick[i];
#endif // defined(DD_P4EST)
}

static inline void maybe_tree_to_boxlcoords(double x[3]) {
#ifndef LB_ADAPTIVE
  tree_to_boxlcoords(x);
#else
  // Id mapping
#endif
}

static inline void boxl_to_treecoords(double x[3]) {
  for (int i = 0; i < 3; ++i)
#ifdef DD_P4EST
    x[i] /= (box_l[i] / dd_p4est_num_trees_in_dir(i));
#else  // defined(DD_P4EST)
    x[i] /= (box_l[i] / lb_conn_brick[i]);
#endif // defined(DD_P4EST)
}

static inline void maybe_boxl_to_treecoords(double x[3]) {
#ifndef LB_ADAPTIVE
  boxl_to_treecoords(x);
#else
   // Id mapping
#endif
}

static inline std::array<double, 3>
maybe_boxl_to_treecoords_copy(const double x[3]) {
  std::array<double, 3> res{{x[0], x[1], x[2]}};
  maybe_boxl_to_treecoords(res.data());
  return res;
}

// forward declaration
int64_t
p4est_utils_pos_morton_idx_global(p8est_t *p4est, int level,
                                  std::vector<int> tree_quadrant_offset_synced,
                                  const double pos[3]);

static p4est_utils_forest_info_t p4est_to_forest_info(p4est_t *p4est) {
  // fill element to insert
  p4est_utils_forest_info_t insert_elem(p4est);

  // allocate a local send buffer to insert local quadrant offsets
  std::vector<p4est_locidx_t> local_tree_offsets(p4est->trees->elem_count);

  // fetch last tree index from last processor
  p4est_topidx_t last_tree_prev_rank = -1;
  if (p4est->mpirank != p4est->mpisize - 1) {
    MPI_Send(&p4est->last_local_tree, 1, P4EST_MPI_TOPIDX, p4est->mpirank + 1,
             p4est->mpirank, p4est->mpicomm);
  }
  if (p4est->mpirank != 0) {
    MPI_Recv(&last_tree_prev_rank, 1, P4EST_MPI_TOPIDX, p4est->mpirank - 1,
             p4est->mpirank - 1, p4est->mpicomm, MPI_STATUS_IGNORE);
  }
  // only fill local send buffer if current process is not empty
  if (p4est->local_num_quadrants != 0) {
    // set start index; if first tree is not completely owned by current
    // process it will set a wrong quadrant offset
    int start_idx = (p4est->first_local_tree == last_tree_prev_rank)
                        ? p4est->first_local_tree + 1
                        : p4est->first_local_tree;
    for (int i = p4est->first_local_tree; i <= p4est->last_local_tree; ++i) {
      p8est_tree_t *tree = p8est_tree_array_index(p4est->trees, i);
      if (start_idx <= i) {
        local_tree_offsets[i] = tree->quadrants_offset +
                                p4est->global_first_quadrant[p4est->mpirank];
      }
      /* local max level */
      if (insert_elem.finest_level_local < tree->maxlevel) {
        insert_elem.finest_level_local = insert_elem.coarsest_level_local =
            tree->maxlevel;
      }
      /* local min level */
      for (int l = insert_elem.coarsest_level_local; l >= 0; --l) {
        if (l < insert_elem.coarsest_level_local &&
            tree->quadrants_per_level[l]) {
          insert_elem.coarsest_level_local = l;
        }
      }
    }
  }
  // synchronize offsets and level and insert into forest_info vector
  // clang-format off
  MPI_Allreduce(local_tree_offsets.data(),
                insert_elem.tree_quadrant_offset_synced.data(),
                p4est->trees->elem_count, P4EST_MPI_LOCIDX, MPI_MAX,
                p4est->mpicomm);
  // clang-format on
  MPI_Allreduce(&insert_elem.finest_level_local,
                &insert_elem.finest_level_global, 1, P4EST_MPI_LOCIDX, MPI_MAX,
                p4est->mpicomm);
  MPI_Allreduce(&insert_elem.coarsest_level_local,
                &insert_elem.coarsest_level_global, 1, P4EST_MPI_LOCIDX,
                MPI_MIN, p4est->mpicomm);
  insert_elem.finest_level_ghost = insert_elem.finest_level_global;
  insert_elem.coarsest_level_ghost = insert_elem.coarsest_level_global;

  // ensure monotony
  P4EST_ASSERT(std::is_sorted(insert_elem.tree_quadrant_offset_synced.begin(),
                              insert_elem.tree_quadrant_offset_synced.end()));

  for (int i = 0; i < p4est->mpisize; ++i) {
    p4est_quadrant_t *q = &p4est->global_first_position[i];
    double xyz[3];
    p4est_utils_get_front_lower_left(p4est, q->p.which_tree, q, xyz);

    // Scale xyz because p4est_utils_pos_morton_idx_global will assume it is
    // and undo this.
    maybe_tree_to_boxlcoords(xyz);

    insert_elem.first_quad_morton_idx[i] = p4est_utils_pos_morton_idx_global(
        p4est, insert_elem.finest_level_global,
        insert_elem.tree_quadrant_offset_synced, xyz);
  }
  insert_elem.first_quad_morton_idx[p4est->mpisize] =
      p4est->trees->elem_count *
      (1 << (P8EST_DIM * insert_elem.finest_level_global));
  P4EST_ASSERT(std::is_sorted(insert_elem.first_quad_morton_idx.begin(),
                              insert_elem.first_quad_morton_idx.end()));

  return insert_elem;
}

void p4est_utils_prepare(std::vector<p8est_t *> p4ests) {
  forest_info.clear();

  std::transform(std::begin(p4ests), std::end(p4ests),
                 std::back_inserter(forest_info), p4est_to_forest_info);
}

void p4est_utils_rebuild_p4est_structs(p4est_connect_type_t btype) {
  std::vector<p4est_t *> forests;
#ifdef DD_P4EST
  forests.push_back(dd.p4est);
#endif // DD_P4EST
  forests.push_back(adapt_p4est);
  p4est_utils_prepare(forests);
#ifdef DD_P4EST
  p4est_utils_partition_multiple_forests(forest_order::short_range,
                                         forest_order::adaptive_LB);
#else
  p4est_partition(adapt_p4est, 1, lbadapt_partition_weight);
#endif // DD_P4EST
#ifdef LB_ADAPTIVE_GPU
  local_num_quadrants = adapt_p4est->local_num_quadrants;
#endif // LB_ADAPTIVE_GPU

  adapt_ghost.reset(p4est_ghost_new(adapt_p4est, btype));
  adapt_mesh.reset(p4est_mesh_new_ext(adapt_p4est, adapt_ghost, 1, 1, 1,
                                      btype));
  adapt_virtual.reset(p4est_virtual_new_ext(adapt_p4est, adapt_ghost,
                                            adapt_mesh, btype, 1));
  adapt_virtual_ghost.reset(p4est_virtual_ghost_new(adapt_p4est, adapt_ghost,
                                                    adapt_mesh, adapt_virtual,
                                                    btype));
}

int p4est_utils_pos_to_proc(forest_order forest, const double pos[3]) {
  const p4est_utils_forest_info_t &current_forest =
      forest_info.at(static_cast<int>(forest));
  int qid = p4est_utils_pos_morton_idx_global(forest, pos);

  int p = std::distance(current_forest.first_quad_morton_idx.begin(),
                        std::upper_bound(
                            current_forest.first_quad_morton_idx.begin(),
                            current_forest.first_quad_morton_idx.end(), qid)) -
          1;

  P4EST_ASSERT(0 <= p && p < current_forest.p4est->mpisize);

  return p;
}

int64_t p4est_utils_cell_morton_idx(int x, int y, int z) {
  int64_t idx = 0;
  int64_t pos = 1;

  for (int i = 0; i < 21; ++i) {
    if ((x & 1))
      idx += pos;
    x >>= 1;
    pos <<= 1;
    if ((y & 1))
      idx += pos;
    y >>= 1;
    pos <<= 1;
    if ((z & 1))
      idx += pos;
    z >>= 1;
    pos <<= 1;
  }

  return idx;
}

/**
 * CAUTION: If LB_ADAPTIVE is not set, all p4ests will be scaled by the side
 *          length of the p4est instance used for short-ranged MD.
 */
int p4est_utils_map_pos_to_tree(p4est_t *p4est, const double pos[3]) {
  int tid = -1;
  for (int t = 0; t < p4est->connectivity->num_trees; ++t) {
    // collect corners of tree
    std::array<double, 3> c[P4EST_CHILDREN];
    for (int ci = 0; ci < P4EST_CHILDREN; ++ci) {
      int v = p4est->connectivity->tree_to_vertex[t * P4EST_CHILDREN + ci];
      c[ci][0] = p4est->connectivity->vertices[P4EST_DIM * v + 0];
      c[ci][1] = p4est->connectivity->vertices[P4EST_DIM * v + 1];
      c[ci][2] = p4est->connectivity->vertices[P4EST_DIM * v + 2];

      // As pure MD allows for box_l != 1.0, "pos" will be in [0,box_l) and
      // not in [0,1). So manually scale the trees to fill [0,box_l).
      maybe_tree_to_boxlcoords(c[ci].data());
    }

    // find lower left and upper right corner of tree
    std::array<double, 3> pos_min{{0., 0., 0.}};
    std::array<double, 3> pos_max{{box_l[0], box_l[1], box_l[2]}};
    int idx_min, idx_max;
    double dist;
    double dist_min = std::numeric_limits<double>::max();
    double dist_max = std::numeric_limits<double>::max();
    for (int ci = 0; ci < P4EST_CHILDREN; ++ci) {
      dist = distance(c[ci], pos_min);
      if (dist < dist_min) {
        dist_min = dist;
        idx_min = ci;
      }
      dist = distance(c[ci], pos_max);
      if (dist < dist_max) {
        dist_max = dist;
        idx_max = ci;
      }
    }

    // if position is between lower left and upper right corner of forest this
    // is the right tree
    if ((c[idx_min][0] <= pos[0]) && (c[idx_min][1] <= pos[1]) &&
        (c[idx_min][2] <= pos[2]) && (pos[0] < c[idx_max][0]) &&
        (pos[1] < c[idx_max][1]) && (pos[2] < c[idx_max][2])) {
      // ensure trees do not overlap
      P4EST_ASSERT(-1 == tid);
      tid = t;
    }
  }
  // ensure that we found a tree
  P4EST_ASSERT(tid != -1);
  return tid;
}

int64_t
p4est_utils_pos_morton_idx_global(p8est_t *p4est, int level,
                                  std::vector<int> tree_quadrant_offset_synced,
                                  const double pos[3]) {

  // find correct tree
  int tid = p4est_utils_map_pos_to_tree(p4est, pos);
  // Qpos is the 3d cell index within tree "tid".
  int qpos[3];

  // In case of pure MD arbitrary numbers are allowed for box_l.
  // Scale "spos" such that it corresponds to a box_l of 1.0
  auto spos = maybe_boxl_to_treecoords_copy(pos);

  int nq = 1 << level;
  for (int i = 0; i < P8EST_DIM; ++i) {
    qpos[i] = (spos[i] - (int)spos[i]) * nq;
    P4EST_ASSERT(0 <= qpos[i] && qpos[i] < nq);
  }

  int qid = p4est_utils_cell_morton_idx(qpos[0], qpos[1], qpos[2]) +
            tree_quadrant_offset_synced[tid];

  return qid;
}

int64_t p4est_utils_pos_morton_idx_global(forest_order forest,
                                          const double pos[3]) {
  const p4est_utils_forest_info_t &current_p4est =
      forest_info.at(static_cast<int>(forest));
  return p4est_utils_pos_morton_idx_global(
      current_p4est.p4est, current_p4est.finest_level_global,
      current_p4est.tree_quadrant_offset_synced, pos);
}

static inline bool is_valid_local_quad(const p8est *p4est, int64_t quad) {
  return quad >= 0 && quad < p4est->local_num_quadrants;
}

#define RETURN_IF_VALID_QUAD(q, fo)                                            \
  do {                                                                         \
    int64_t qid = q;                                                           \
    if (is_valid_local_quad(forest_info[static_cast<int>(fo)].p4est, qid))     \
      return qid;                                                              \
  } while (0)

int64_t p4est_utils_pos_quad_ext(forest_order forest, const double pos[3]) {
  // Try pos itself
  RETURN_IF_VALID_QUAD(p4est_utils_pos_qid_local(forest, pos), forest);

  // If pos is outside of the local domain try the bounding box enlarged
  // ROUND_ERROR_PREC
  for (int i = -1; i <= 1; i += 2) {
    for (int j = -1; j <= 1; j += 2) {
      for (int k = -1; k <= 1; k += 2) {
        double spos[3] = {pos[0] + i * box_l[0] * ROUND_ERROR_PREC,
                          pos[1] + j * box_l[1] * ROUND_ERROR_PREC,
                          pos[2] + k * box_l[2] * ROUND_ERROR_PREC};

        RETURN_IF_VALID_QUAD(p4est_utils_pos_qid_local(forest, spos), forest);
      }
    }
  }

  return -1;
}

int p4est_utils_find_qid_prepare(forest_order forest, const double pos[3],
                                 p8est_tree_t **tree,
                                 p8est_quadrant_t *pquad) {
  const p4est_utils_forest_info_t &current_p4est =
      forest_info.at(static_cast<int>(forest));
  p8est_t *p4est = current_p4est.p4est;

  // find correct tree
  int tid = p4est_utils_map_pos_to_tree(p4est, pos);
  int level = current_p4est.finest_level_global;
  *tree = p4est_tree_array_index(p4est->trees, tid);

  double first_pos[3];
  p4est_qcoord_to_vertex(p4est->connectivity, tid, 0, 0, 0, first_pos);

  // Trees might not have a base length of 1
  auto spos = maybe_boxl_to_treecoords_copy(pos);

  int qcoord[3];
  for (int i = 0; i < P8EST_DIM; ++i) {
    qcoord[i] = (spos[i] - first_pos[i]) * (1 << level);
  }

  int64_t pidx = p4est_utils_cell_morton_idx(qcoord[0], qcoord[1], qcoord[2]);
  p4est_quadrant_set_morton(pquad, level, pidx);
  pquad->p.which_tree = tid;

  return 0;
}

p4est_locidx_t p4est_utils_pos_qid_local(forest_order forest,
                                         const double pos[3]) {
  p4est_tree_t *tree;
  p4est_quadrant_t pquad;
  p4est_utils_find_qid_prepare(forest, pos, &tree, &pquad);

  p4est_locidx_t index = p8est_find_lower_bound_overlap(
      &tree->quadrants, &pquad, 0.5 * tree->quadrants.elem_count);

#ifdef P4EST_ENABLE_DEBUG
  p8est_quadrant_t *quad = p4est_quadrant_array_index(&tree->quadrants, index);
  P4EST_ASSERT(p8est_quadrant_overlaps(&pquad, quad));
#endif // P4EST_ENABLE_DEBUG

  index += tree->quadrants_offset;

  P4EST_ASSERT(
      0 <= index &&
      index <
          forest_info.at(static_cast<int>(forest)).p4est->local_num_quadrants);

  return index;
}

p4est_locidx_t p4est_utils_pos_qid_ghost(forest_order forest,
                                         p8est_ghost_t *ghost,
                                         const double pos[3]) {
  p8est_tree_t *tree;
  p8est_quadrant_t q;
  p4est_utils_find_qid_prepare(forest, pos, &tree, &q);

  p4est_locidx_t index = p8est_find_lower_bound_overlap_piggy(
      &ghost->ghosts, &q, 0.5 * ghost->ghosts.elem_count);

#ifdef P4EST_ENABLE_DEBUG
  p8est_quadrant_t *quad = p4est_quadrant_array_index(&ghost->ghosts, index);
  P4EST_ASSERT(p8est_quadrant_overlaps(&q, quad));
#endif // P4EST_ENABLE_DEBUG

  P4EST_ASSERT(0 <= index && (size_t) index < ghost->ghosts.elem_count);

  return index;
}

// CAUTION: Currently LB only
int coarsening_criteria(p8est_t *p8est, p4est_topidx_t which_tree,
                        p8est_quadrant_t **quads) {
  // get quad id
  int qid = quads[0]->p.user_long;
  if (qid == -1) return 0;

  lbadapt_payload_t *data =
    &lbadapt_local_data[quads[0]->level][adapt_virtual->quad_qreal_offset[qid]];
  int coarsen = 1;
  for (int i = 0; i < P8EST_CHILDREN; ++i) {
    // avoid coarser cells than base_level
    if (quads[i]->level == lbpar.base_level) return 0;
    coarsen &= !(data->lbfields.boundary) && ((*flags)[qid + i] == 2);
    ++data;
  }
#ifdef DUMP_DECISIONS
  if (coarsen) {
    std::string filename = "coarsened_quads_size_" +
                           std::to_string(p8est->mpisize) +
                           "_step_" +
                           std::to_string(n_lbsteps) +
                           ".txt";
    std::ofstream myfile;
    myfile.open(filename, std::ofstream::out | std::ofstream::app);
    myfile << qid + p8est->global_first_quadrant[p8est->mpirank] << std::endl;
    myfile.flush();
    myfile.close();
  }
#endif // DUMP_DECISIONS
  return coarsen;
}


int refinement_criteria(p8est_t *p8est, p4est_topidx_t which_tree,
                        p8est_quadrant_t *q) {
  // get quad id
  int qid = q->p.user_long;

  // perform geometric refinement
  int refine = refine_geometric(p8est, which_tree, q);

  // refine if we have marked the cell as to be refined and add padding to flag
  // vector
  if ((q->level < lbpar.max_refinement_level) &&
      ((1 == (*flags)[qid] || refine))) {
#if 0
    int fill[] = { 0, 0, 0, 0, 0, 0, 0 };
    flags->insert(flags->begin() + qid, fill, fill + 7);
#endif // 0
    return 1;
  }
  return 0;
}

void dump_decisions_synced(sc_array_t * vel, sc_array_t * vort,
                           double vel_thresh_coarse, double vel_thresh_refine,
                           double vort_thresh_coarse, double vort_thresh_refine)
{
#ifndef LB_ADAPTIVE_GPU
  int nqid = 0;
  p4est_quadrant_t *q;
  std::string filename = "refinement_decision_step_" +
                         std::to_string(n_lbsteps) + ".txt";

  for (int qid = 0; qid < adapt_p4est->global_num_quadrants; ++qid) {
    // Synchronization point
    MPI_Barrier(adapt_p4est->mpicomm);

    // MPI rank holding current quadrant will open the file, append its
    // information, flush it, and close the file afterwards.
    if ((adapt_p4est->global_first_quadrant[adapt_p4est->mpirank] <= qid) &&
        (qid < adapt_p4est->global_first_quadrant[adapt_p4est->mpirank + 1])) {
      // get quadrant for level information
      q = p4est_mesh_get_quadrant(adapt_p4est, adapt_mesh, nqid);
      lbadapt_payload_t *data =
          &lbadapt_local_data[q->level][adapt_virtual->quad_qreal_offset[nqid]];
      std::ofstream myfile;
      myfile.open(filename, std::ofstream::out | std::ofstream::app);
      myfile << "id: " << qid << " level: " << (int)q->level
             << " boundary: " << data->lbfields.boundary
             << " local: " << nqid << std::endl;

      double v = sqrt(SQR(*(double*) sc_array_index(vel, 3 * nqid)) +
                      SQR(*(double*) sc_array_index(vel, 3 * nqid + 1)) +
                      SQR(*(double*) sc_array_index(vel, 3 * nqid + 2)));
      myfile << "v: coarse: " << vel_thresh_coarse << " refine: "
             << vel_thresh_refine << " actual: " << v << std::endl;
      myfile << "vort: coarse: " << vort_thresh_coarse << " refine: "
             << vort_thresh_refine << " actual: ";
      for (int d = 0; d < P4EST_DIM; ++d) {
        myfile << abs(*(double*) sc_array_index(vort, 3 * nqid + d));
        if (d < 2) myfile << ", ";
        else myfile << std::endl;
      }
      myfile << "decision: " << (*flags)[nqid] << std::endl;
      myfile << std::endl;

      myfile.flush();
      myfile.close();
      // increment local quadrant index
      ++nqid;
    }
  }
  // make sure that we have inspected all local quadrants.
  P4EST_ASSERT (nqid == adapt_p4est->local_num_quadrants);
#endif // !defined (LB_ADAPTIVE_GPU)
}


int p4est_utils_collect_flags(std::vector<int> *flags) {
  // get refinement string for first grid change operation
#if 0
  std::fstream fs;
  fs.open("refinement.txt", std::fstream::in);
  int flag;
  char comma;
  int ctr = 0;
  int ctr_ones = 0;
  int ctr_twos = 0;
  while (fs >> flag) {
    if ((adapt_p4est->global_first_quadrant[adapt_p4est->mpirank] <= ctr) &&
        (ctr < adapt_p4est->global_first_quadrant[adapt_p4est->mpirank + 1])) {
      if (flag == 1)
        ++ctr_ones;
      if (flag == 2)
        ++ctr_twos;
      flags->push_back(flag);
    }
    ++ctr;
    // fetch comma to prevent early loop exit
    fs >> comma;
  }
  std::cout << "[p4est " << adapt_p4est->mpirank << "] ones: " << ctr_ones
            << " twos: " << ctr_twos << " total: " << ctr << std::endl;
  fs.close();
#else // 0
  // velocity
  // Euclidean norm
  castable_unique_ptr<sc_array_t> vel_values =
      sc_array_new_size(sizeof(double), 3 * adapt_p4est->local_num_quadrants);
  lbadapt_get_velocity_values(vel_values);
  double v;
  double v_min = std::numeric_limits<double>::max();
  double v_max = std::numeric_limits<double>::min();
  for (int qid = 0; qid < adapt_p4est->local_num_quadrants; ++qid) {
    v = sqrt(SQR(*(double *)sc_array_index(vel_values, 3 * qid)) +
             SQR(*(double *)sc_array_index(vel_values, 3 * qid + 1)) +
             SQR(*(double *)sc_array_index(vel_values, 3 * qid + 2)));
    if (v < v_min) {
      v_min = v;
    }
    if (v > v_max) {
      v_max = v;
    }
  }
  // sync
  v = v_min;
  MPI_Allreduce(&v, &v_min, 1, MPI_DOUBLE, MPI_MIN, adapt_p4est->mpicomm);
  v = v_max;
  MPI_Allreduce(&v, &v_max, 1, MPI_DOUBLE, MPI_MAX, adapt_p4est->mpicomm);

  // vorticity
  // max norm
  castable_unique_ptr<sc_array_t> vort_values =
      sc_array_new_size(sizeof(double), 3 * adapt_p4est->local_num_quadrants);
  lbadapt_get_vorticity_values(vort_values);
  double vort_min = std::numeric_limits<double>::max();
  double vort_max = std::numeric_limits<double>::min();
  double vort_temp;
  for (int qid = 0; qid < adapt_p4est->local_num_quadrants; ++qid) {
    for (int d = 0; d < P4EST_DIM; ++d) {
      vort_temp = abs(*(double*) sc_array_index(vort_values, 3 * qid + d));
      if (vort_temp < vort_min) {
        vort_min = vort_temp;
      }
      if (vort_temp > vort_max) {
        vort_max = vort_temp;
      }
    }
  }
  // sync
  vort_temp  = vort_min;
  MPI_Allreduce(&vort_temp, &vort_min, 1, MPI_DOUBLE, MPI_MIN,
                adapt_p4est->mpicomm);
  vort_temp  = vort_max;
  MPI_Allreduce(&vort_temp, &vort_max, 1, MPI_DOUBLE, MPI_MAX,
                adapt_p4est->mpicomm);

  double v_thresh_coarse = 0.05;
  double v_thresh_refine = 0.15;
  double vort_thresh_coarse = 0.02;
  double vort_thresh_refine = 0.05;
  // traverse forest and decide if the current quadrant is to be refined or
  // coarsened
  for (int qid = 0; qid < adapt_p4est->local_num_quadrants; ++qid) {
#ifdef USE_VEL_CRIT
    // velocity
    double v = sqrt(SQR(*(double*) sc_array_index(vel_values, 3 * qid)) +
                    SQR(*(double*) sc_array_index(vel_values, 3 * qid + 1)) +
                    SQR(*(double*) sc_array_index(vel_values, 3 * qid + 2)));
    // Note, that this formulation stems from the fact that velocity is 0 at
    // boundaries
    if (v_thresh_refine * (v_max - v_min) <= (v - v_min)) {
      flags->push_back(1);
    }
    else if (v - v_min <= v_thresh_coarse * (v_max - v_min)) {
      flags->push_back(2);
    }
    else {
      flags->push_back(0);
    }
#endif // USE_VEL_CRIT
#ifdef USE_VORT_CRIT
    // vorticity
    double vort = std::numeric_limits<double>::min();
    for (int d = 0; d < P4EST_DIM; ++d) {
      vort_temp = abs(*(double*) sc_array_index(vort_values, 3 * qid + d));
      if (vort < vort_temp) {
        vort = vort_temp;
      }
    }
#ifndef USE_VEL_CRIT
    if (vort_thresh_refine * (vort_max - vort_min) <= (vort - vort_min)) {
      flags->push_back(1);
    }
    else if (vort - vort_min < vort_thresh_coarse * (vort_max - vort_min)) {
      flags->push_back(2);
    }
    else {
      flags->push_back(0);
    }
#else // USE_VEL_CRIT
    if (vort_thresh_refine * (vort_max - vort_min) <= (vort - vort_min)) {
      (*flags)[qid] = 1;
    }
    else if ((1 != (*flags)[qid]) &&
             (vort - vort_min < vort_thresh_coarse * (vort_max - vort_min))) {
      (*flags)[qid] = 2;
    }
#endif // USE_VEL_CRIT
#endif // USE_VORT_CRIT
  }
#endif //0
#ifdef DUMP_DECISIONS
  dump_decisions_synced(vel_values, vort_values,
                        v_thresh_coarse * (v_max - v_min),
                        v_thresh_refine * (v_max - v_min),
                        vort_thresh_coarse * (vort_max - vort_min),
                        vort_thresh_refine * (vort_max - vort_min));
#endif // DUMP_DECISIONS
  return 0;
}

void p4est_utils_qid_dummy (p8est_t *p8est, p4est_topidx_t which_tree,
                            p8est_quadrant_t *q) {
  q->p.user_long = -1;
}
int p4est_utils_adapt_grid() {
#ifdef LB_ADAPTIVE
  p4est_connect_type_t btype = P4EST_CONNECT_FULL;

  // 1st step: alter copied grid and map data between grids.
  // collect refinement and coarsening flags.
  flags = new std::vector<int>();
  flags->reserve(P4EST_CHILDREN * adapt_p4est->local_num_quadrants);

  p4est_utils_collect_flags(flags);
  p4est_iterate(adapt_p4est, adapt_ghost, 0, lbadapt_init_qid_payload, 0, 0, 0);

  // copy forest and perform refinement step.
  p8est_t *p4est_adapted = p8est_copy(adapt_p4est, 0);
  P4EST_ASSERT(p4est_is_equal(p4est_adapted, adapt_p4est, 0));
  p8est_refine_ext(p4est_adapted, 0, lbpar.max_refinement_level,
                   refinement_criteria, p4est_utils_qid_dummy, 0);
  // perform coarsening step
  p8est_coarsen_ext(p4est_adapted, 0, 0, coarsening_criteria, 0, 0);
  delete flags;
  // balance forest after grid change
  p8est_balance_ext(p4est_adapted, P8EST_CONNECT_FULL, 0, 0);

  // 2nd step: locally map data between forests.
  // de-allocate invalid storage and data-structures
    p4est_utils_deallocate_levelwise_storage(lbadapt_ghost_data);
  // locally map data between forests.
    lbadapt_payload_t *mapped_data_flat =
        P4EST_ALLOC_ZERO(lbadapt_payload_t, p4est_adapted->local_num_quadrants);
    p4est_utils_post_gridadapt_map_data(adapt_p4est, adapt_mesh, adapt_virtual,
                                        p4est_adapted, lbadapt_local_data,
                                        mapped_data_flat);
    // cleanup
    p4est_utils_deallocate_levelwise_storage(lbadapt_local_data);
    adapt_virtual.reset();
    adapt_mesh.reset();
    adapt_p4est.reset();

    // 3rd step: partition grid and transfer data to respective new owner ranks
    // FIXME: Interface to Steffen's partitioning logic
    // FIXME: Synchronize partitioning between short-range MD and adaptive
    //        p4ests
    p8est_t *p4est_partitioned = p8est_copy(p4est_adapted, 0);
    p8est_partition_ext(p4est_partitioned, 1, lbadapt_partition_weight);
    std::vector<std::vector<lbadapt_payload_t>> data_partitioned(
        p4est_partitioned->mpisize, std::vector<lbadapt_payload_t>());
    p4est_utils_post_gridadapt_data_partition_transfer(
        p4est_adapted, p4est_partitioned, mapped_data_flat, data_partitioned);

  p4est_destroy(p4est_adapted);
    P4EST_FREE(mapped_data_flat);

  // 4th step: Insert received data into new levelwise data-structure
    adapt_p4est.reset(p4est_partitioned);
    adapt_ghost.reset(p4est_ghost_new(adapt_p4est, btype));
    adapt_mesh.reset(p4est_mesh_new_ext(adapt_p4est, adapt_ghost, 1, 1, 1,
                                        btype));
    adapt_virtual.reset(p4est_virtual_new_ext(adapt_p4est, adapt_ghost,
                                              adapt_mesh, btype, 1));
    adapt_virtual_ghost.reset(p4est_virtual_ghost_new(adapt_p4est, adapt_ghost,
                                                      adapt_mesh, adapt_virtual,
                                                      btype));
    p4est_utils_allocate_levelwise_storage(lbadapt_local_data, adapt_mesh,
                                           adapt_virtual, true);
    p4est_utils_allocate_levelwise_storage(lbadapt_ghost_data, adapt_mesh,
                                           adapt_virtual, false);
    p4est_utils_post_gridadapt_insert_data(
      p4est_partitioned, adapt_mesh, adapt_virtual, data_partitioned,
      lbadapt_local_data);

  // 5th step: Prepare next integration step
    std::vector<p4est_t *> forests;
#ifdef DD_P4EST
    forests.push_back(dd.p4est);
#endif // DD_P4EST
    forests.push_back(adapt_p4est);
    p4est_utils_prepare(forests);
    const p4est_utils_forest_info_t new_forest =
        p4est_utils_get_forest_info(forest_order::adaptive_LB);
    // synchronize ghost data for next collision step
    std::vector<lbadapt_payload_t *> local_pointer(P8EST_QMAXLEVEL);
    std::vector<lbadapt_payload_t *> ghost_pointer(P8EST_QMAXLEVEL);
    prepare_ghost_exchange(lbadapt_local_data, local_pointer,
                           lbadapt_ghost_data, ghost_pointer);
    for (int level = new_forest.coarsest_level_global;
         level <= new_forest.finest_level_global; ++level) {
      p4est_virtual_ghost_exchange_data_level (adapt_p4est, adapt_ghost,
                                               adapt_mesh, adapt_virtual,
                                               adapt_virtual_ghost, level,
                                               sizeof(lbadapt_payload_t),
                                               (void**)local_pointer.data(),
                                               (void**)ghost_pointer.data());
    }

#endif // LB_ADAPTIVE
  return 0;
}

template <typename T>
int p4est_utils_post_gridadapt_map_data(
    p4est_t *p4est_old, p4est_mesh_t *mesh_old, p4est_virtual_t *virtual_quads,
    p4est_t *p8est_new, std::vector<std::vector<T>> &local_data_levelwise,
    T *mapped_data_flat) {
  // counters
  unsigned int tid_old = p4est_old->first_local_tree;
  unsigned int tid_new = p4est_new->first_local_tree;
  unsigned int qid_old = 0, qid_new = 0;
  unsigned int tqid_old = 0, tqid_new = 0;

  // trees
  p8est_tree_t *curr_tree_old =
      p8est_tree_array_index(p4est_old->trees, tid_old);
  p8est_tree_t *curr_tree_new =
      p8est_tree_array_index(p4est_new->trees, tid_new);
  // quadrants
  p8est_quadrant_t *curr_quad_old, *curr_quad_new;

  int level_old, sid_old;
  int level_new;

  while (qid_old < (size_t) p4est_old->local_num_quadrants &&
         qid_new < (size_t) p4est_new->local_num_quadrants) {
    // wrap multiple trees
    if (tqid_old == curr_tree_old->quadrants.elem_count) {
      ++tid_old;
      P4EST_ASSERT(tid_old < p4est_old->trees->elem_count);
      curr_tree_old = p8est_tree_array_index(p4est_old->trees, tid_old);
      tqid_old = 0;
    }
    if (tqid_new == curr_tree_new->quadrants.elem_count) {
      ++tid_new;
      P4EST_ASSERT(tid_new < p4est_new->trees->elem_count);
      curr_tree_new = p8est_tree_array_index(p4est_new->trees, tid_new);
      tqid_new = 0;
    }

    // fetch next quadrants in old and new forest and obtain storage id
    curr_quad_old =
        p8est_quadrant_array_index(&curr_tree_old->quadrants, tqid_old);
    level_old = curr_quad_old->level;
    sid_old = virtual_quads->quad_qreal_offset[qid_old];

    curr_quad_new =
        p8est_quadrant_array_index(&curr_tree_new->quadrants, tqid_new);
    level_new = curr_quad_new->level;

    // distinguish three cases to properly map data and increase indices
    if (level_old == level_new) {
      // old cell has neither been coarsened nor refined
      data_transfer(p4est_old, p4est_new, curr_quad_old, curr_quad_new, tid_old,
                    &local_data_levelwise[level_old][sid_old],
                    &mapped_data_flat[qid_new]);
      ++qid_old;
      ++qid_new;
      ++tqid_old;
      ++tqid_new;
    } else if (level_old == level_new + 1) {
      // old cell has been coarsened
      for (int child = 0; child < P8EST_CHILDREN; ++child) {
        data_restriction(p4est_old, p4est_new, curr_quad_old, curr_quad_new,
                         tid_old, &local_data_levelwise[level_old][sid_old],
                         &mapped_data_flat[qid_new]);
        ++sid_old;
        ++tqid_old;
        ++qid_old;
      }
      ++tqid_new;
      ++qid_new;
    } else if (level_old + 1 == level_new) {
      // old cell has been refined.
      for (int child = 0; child < P8EST_CHILDREN; ++child) {
        data_interpolation(p4est_old, p4est_new, curr_quad_old, curr_quad_new,
                           tid_old, &local_data_levelwise[level_old][sid_old],
                           &mapped_data_flat[qid_new]);
        ++tqid_new;
        ++qid_new;
      }
      ++tqid_old;
      ++qid_old;
    } else {
      SC_ABORT_NOT_REACHED();
    }

    // sanity check of indices
    P4EST_ASSERT(tqid_old + curr_tree_old->quadrants_offset == qid_old);
    P4EST_ASSERT(tqid_new + curr_tree_new->quadrants_offset == qid_new);
    P4EST_ASSERT(tid_old == tid_new);
  }
  P4EST_ASSERT(qid_old == (size_t) p4est_old->local_num_quadrants);
  P4EST_ASSERT(qid_new == (size_t) p4est_new->local_num_quadrants);

  return 0;
}

template <typename T>
int p4est_utils_post_gridadapt_data_partition_transfer(
    p8est_t *p4est_old, p8est_t *p4est_new, T *data_mapped,
    std::vector<std::vector<T>> &data_partitioned) {
  // simple consistency checks
  P4EST_ASSERT(p4est_old->mpirank == p4est_new->mpirank);
  P4EST_ASSERT(p4est_old->mpisize == p4est_new->mpisize);
  P4EST_ASSERT(p4est_old->global_num_quadrants ==
               p4est_new->global_num_quadrants);

  int rank = p4est_old->mpirank;
  int size = p4est_old->mpisize;
  int lb_old_local = p4est_old->global_first_quadrant[rank];
  int ub_old_local = p4est_old->global_first_quadrant[rank + 1];
  int lb_new_local = p4est_new->global_first_quadrant[rank];
  int ub_new_local = p4est_new->global_first_quadrant[rank + 1];
  int lb_old_remote = 0;
  int ub_old_remote = 0;
  int lb_new_remote = 0;
  int ub_new_remote = 0;
  int data_length = 0;
  int send_offset = 0;

  int mpiret;
  MPI_Request r;
  std::vector<MPI_Request> requests(2 * size, MPI_REQUEST_NULL);

  // determine from which processors we receive quadrants
  /** there are 5 cases to distinguish
   * 1. no quadrants of neighbor need to be received; neighbor rank < rank
   * 2. some quadrants of neighbor need to be received; neighbor rank < rank
   * 3. all quadrants of neighbor need to be received from neighbor
   * 4. some quadrants of neighbor need to be received; neighbor rank > rank
   * 5. no quadrants of neighbor need to be received; neighbor rank > rank
   */
  for (int p = 0; p < size; ++p) {
    lb_old_remote = ub_old_remote;
    ub_old_remote = p4est_old->global_first_quadrant[p + 1];

    // number of quadrants from which payload will be received
    data_length = std::max(0,
                           std::min(ub_old_remote, ub_new_local) -
                               std::max(lb_old_remote, lb_new_local));

    // allocate receive buffer and wait for messages
    data_partitioned[p].resize(data_length);
    r = requests[p];
    mpiret =
        MPI_Irecv((void *)data_partitioned[p].data(), data_length * sizeof(T),
                  MPI_BYTE, p, 0, p4est_new->mpicomm, &r);
    requests[p] = r;
    SC_CHECK_MPI(mpiret);
  }

  // send respective quadrants to other processors
  for (int p = 0; p < size; ++p) {
    lb_new_remote = ub_new_remote;
    ub_new_remote = p4est_new->global_first_quadrant[p + 1];

    data_length = std::max(0,
                           std::min(ub_old_local, ub_new_remote) -
                               std::max(lb_old_local, lb_new_remote));

    r = requests[size + p];
    mpiret =
        MPI_Isend((void *)(data_mapped + send_offset), data_length * sizeof(T),
                  MPI_BYTE, p, 0, p4est_new->mpicomm, &r);
    requests[size + p] = r;
    SC_CHECK_MPI(mpiret);
    send_offset += data_length;
  }

  /** Wait for communication to finish */
  mpiret = MPI_Waitall(requests.size(), requests.data(), MPI_STATUSES_IGNORE);
  SC_CHECK_MPI(mpiret);

  return 0;
}

template <typename T>
int p4est_utils_post_gridadapt_insert_data(
    p8est_t *p4est_new, p8est_mesh_t *mesh_new, p4est_virtual_t *virtual_quads,
    std::vector<std::vector<T>> &data_partitioned,
    std::vector<std::vector<T>> &data_levelwise) {
  int size = p4est_new->mpisize;
  // counters
  unsigned int tid = p4est_new->first_local_tree;
  int qid = 0;
  unsigned int tqid = 0;

  // trees
  p8est_tree_t *curr_tree = p8est_tree_array_index(p4est_new->trees, tid);
  // quadrants
  p8est_quadrant_t *curr_quad;

  int level, sid;

  for (int p = 0; p < size; ++p) {
    for (unsigned int q = 0; q < data_partitioned[p].size(); ++q) {
      // wrap multiple trees
      if (tqid == curr_tree->quadrants.elem_count) {
        ++tid;
        P4EST_ASSERT(tid < p4est_new->trees->elem_count);
        curr_tree = p8est_tree_array_index(p4est_new->trees, tid);
        tqid = 0;
      }
      curr_quad = p8est_quadrant_array_index(&curr_tree->quadrants, tqid);
      level = curr_quad->level;
      sid = virtual_quads->quad_qreal_offset[qid];
      std::memcpy(&data_levelwise[level][sid], &data_partitioned[p][q],
                  sizeof(T));
      ++tqid;
      ++qid;
    }
  }

  // verify that all real quadrants have been processed
  P4EST_ASSERT(qid == mesh_new->local_num_quadrants);

  return 0;
}

void p4est_utils_partition_multiple_forests(forest_order reference,
                                            forest_order modify) {
  p8est_t *p4est_ref = forest_info.at(static_cast<int>(reference)).p4est;
  p8est_t *p4est_mod = forest_info.at(static_cast<int>(modify)).p4est;
  P4EST_ASSERT(p4est_ref->mpisize == p4est_mod->mpisize);
  P4EST_ASSERT(p4est_ref->mpirank == p4est_mod->mpirank);
  P4EST_ASSERT(p8est_connectivity_is_equivalent(p4est_ref->connectivity,
                                                p4est_mod->connectivity));

  std::vector<p4est_locidx_t> num_quad_per_proc(p4est_ref->mpisize, 0);
  std::vector<p4est_locidx_t> num_quad_per_proc_global(p4est_ref->mpisize, 0);

  unsigned int tid = p4est_mod->first_local_tree;
  unsigned int tqid = 0;
  // trees
  p8est_tree_t *curr_tree;
  // quadrants
  p8est_quadrant_t *curr_quad;

  if (0 < p4est_mod->local_num_quadrants) {
    curr_tree = p8est_tree_array_index(p4est_mod->trees, tid);
  }

  // Check for each of the quadrants of the given p4est, to which MD cell it
  // maps
  for (int qid = 0; qid < p4est_mod->local_num_quadrants; ++qid) {
    // wrap multiple trees
    if (tqid == curr_tree->quadrants.elem_count) {
      ++tid;
      P4EST_ASSERT(tid < p4est_mod->trees->elem_count);
      curr_tree = p8est_tree_array_index(p4est_mod->trees, tid);
      tqid = 0;
    }
    if (0 < curr_tree->quadrants.elem_count) {
      curr_quad = p8est_quadrant_array_index(&curr_tree->quadrants, tqid);
      double xyz[3];
      p4est_utils_get_front_lower_left(p4est_mod, tid, curr_quad, xyz);
      int proc = p4est_utils_pos_to_proc(reference, xyz);
      ++num_quad_per_proc[proc];
    }
    ++tqid;
  }

  // Gather this information over all processes
  MPI_Allreduce(num_quad_per_proc.data(), num_quad_per_proc_global.data(),
                p4est_mod->mpisize, P4EST_MPI_LOCIDX, MPI_SUM,
                p4est_mod->mpicomm);

  p4est_locidx_t sum = std::accumulate(std::begin(num_quad_per_proc_global),
                                       std::end(num_quad_per_proc_global), 0);

  if (sum < p4est_mod->global_num_quadrants) {
    printf("%i : quadrants lost while partitioning\n", this_node);
    errexit();
  }

  CELL_TRACE(printf("%i : repartitioned LB %i\n", this_node,
                    num_quad_per_proc_global[this_node]));

  // Repartition with the computed distribution
  int shipped =
      p8est_partition_given(p4est_mod, num_quad_per_proc_global.data());
  P4EST_GLOBAL_PRODUCTIONF(
      "Done " P8EST_STRING "_partition shipped %lld quadrants %.3g%%\n",
      (long long)shipped, shipped * 100. / p4est_mod->global_num_quadrants);
}

int fct_coarsen_cb(p4est_t *p4est, p4est_topidx_t tree_idx,
                   p4est_quadrant_t *quad[]) {
  p4est_t *cmp = (p4est_t *)p4est->user_pointer;
  p4est_tree_t *tree = p4est_tree_array_index(cmp->trees, tree_idx);
  for (unsigned int i = 0; i < tree->quadrants.elem_count; ++i) {
    p4est_quadrant_t *q = p4est_quadrant_array_index(&tree->quadrants, i);
    if (p4est_quadrant_overlaps(q, quad[0]) && q->level >= quad[0]->level)
      return 0;
  }
  return 1;
}

p4est_t *p4est_utils_create_fct(p4est_t *t1, p4est_t *t2) {
  p4est_t *fct = p4est_copy(t2, 0);
  fct->user_pointer = (void *)t1;
  p4est_coarsen(fct, 1, fct_coarsen_cb, NULL);
  return fct;
}

bool p4est_utils_check_alignment(const p4est_t *t1, const p4est_t *t2) {
  if (!p4est_connectivity_is_equivalent(t1->connectivity, t2->connectivity)) return false;
  if (t1->first_local_tree != t2->first_local_tree) return false;
  if (t1->last_local_tree != t2->last_local_tree) return false;
  p4est_quadrant_t *q1 = &t1->global_first_position[t1->mpirank];
  p4est_quadrant_t *q2 = &t2->global_first_position[t2->mpirank];
  if (q1->x != q2->x && q1->y != q2->y && q1->z != q2->z) return false;
  q1 = &t1->global_first_position[t1->mpirank+1];
  q2 = &t2->global_first_position[t2->mpirank+1];
  if (q1->x != q2->x && q1->y != q2->y && q1->z != q2->z) return false;
  return true;
}

void p4est_utils_weighted_partition(p4est_t *t1, const std::vector<double> &w1,
                                    double a1, p4est_t *t2,
                                    const std::vector<double> &w2, double a2) {
  P4EST_ASSERT(p4est_utils_check_alignment(t1, t2));

  std::unique_ptr<p4est_t> fct(p4est_utils_create_fct(t1, t2));
  std::vector<double> w_fct(fct->local_num_quadrants, 0.0);
  std::vector<size_t> t1_quads_per_fct_quad(fct->local_num_quadrants, 0);
  std::vector<size_t> t2_quads_per_fct_quad(fct->local_num_quadrants, 0);
  std::vector<p4est_locidx_t> t1_quads_per_proc(fct->mpisize, 0);
  std::vector<p4est_locidx_t> t2_quads_per_proc(fct->mpisize, 0);

  size_t w_id1, w_id2, w_idx;
  w_id1 = w_id2 = w_idx = 0;
  for (p4est_topidx_t t_idx = fct->first_local_tree;
       t_idx <= fct->last_local_tree; ++t_idx) {
    p4est_tree_t *t_fct = p4est_tree_array_index(fct->trees, t_idx);
    p4est_tree_t *t_t1  = p4est_tree_array_index(t1->trees, t_idx);
    p4est_tree_t *t_t2  = p4est_tree_array_index(t2->trees, t_idx);
    size_t q_id1, q_id2;
    q_id1 = q_id2 = 0;
    p4est_quadrant_t *q1 = p4est_quadrant_array_index(&t_t1->quadrants, q_id1);
    p4est_quadrant_t *q2 = p4est_quadrant_array_index(&t_t1->quadrants, q_id2);
    for (size_t q_idx = 0; q_idx < t_fct->quadrants.elem_count; ++q_idx) {
      p4est_quadrant_t *q_fct = p4est_quadrant_array_index(&t_fct->quadrants, q_idx);
      while (p4est_quadrant_overlaps(q_fct, q1)) {
        w_fct[w_idx] += a1*w1[w_id1++];
        ++t1_quads_per_fct_quad[w_idx];
        if (++q_id1 >= t_t1->quadrants.elem_count) {
          // complain if last quad in t1 does not overlap with last quad of FCT
          P4EST_ASSERT(q_idx == t_fct->quadrants.elem_count - 1);
          break;
        }
        q1 = p4est_quadrant_array_index(&t_t1->quadrants, q_id1);
      }
      while (p4est_quadrant_overlaps(q_fct, q2)) {
        w_fct[w_idx] += a2*w2[w_id2++];
        ++t2_quads_per_fct_quad[w_idx];
        if (++q_id2 >= t_t2->quadrants.elem_count) {
          // complain if last quad in t2 does not overlap with last quad of FCT
          P4EST_ASSERT(q_idx == t_fct->quadrants.elem_count - 1);
          break;
        }
        q2 = p4est_quadrant_array_index(&t_t2->quadrants, q_id2);
      }
      ++w_idx;
    }
  }

  // complain if counters haven't reached the end
  P4EST_ASSERT(w_idx == (size_t) fct->local_num_quadrants);
  P4EST_ASSERT(w_id1 == (size_t) t1->local_num_quadrants);
  P4EST_ASSERT(w_id2 == (size_t) t2->local_num_quadrants);

  double localsum = std::accumulate(w_fct.begin(), w_fct.end(), 0.0);
  double sum, prefix = 0; // Initialization is necessary on rank 0!
  MPI_Allreduce(&localsum, &sum, 1, MPI_DOUBLE, MPI_SUM, comm_cart);
  MPI_Exscan(&localsum, &prefix, 1, MPI_DOUBLE, MPI_SUM, comm_cart);
  double target = sum / fct->mpisize;

  for (size_t idx = 0; idx < (size_t) fct->local_num_quadrants; ++idx) {
    int proc = std::min<int>(w_fct[idx] / target, fct->mpisize - 1);
    t1_quads_per_proc[proc] += t1_quads_per_fct_quad[idx];
    t2_quads_per_proc[proc] += t2_quads_per_fct_quad[idx];
  }

  MPI_Allreduce(MPI_IN_PLACE, t1_quads_per_proc.data(), fct->mpisize,
                P4EST_MPI_LOCIDX, MPI_SUM, comm_cart);
  MPI_Allreduce(MPI_IN_PLACE, t2_quads_per_proc.data(), fct->mpisize,
                P4EST_MPI_LOCIDX, MPI_SUM, comm_cart);

  p4est_partition_given(t1, t1_quads_per_proc.data());
  p4est_partition_given(t2, t2_quads_per_proc.data());
}

#endif // defined (LB_ADAPTIVE) || defined (DD_P4EST)
