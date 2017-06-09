#include "p4est_utils.hpp"

#if (defined(LB_ADAPTIVE) || defined(DD_P4EST))

#include "debug.hpp"
#include "domain_decomposition.hpp"
#include "p4est_dd.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <mpi.h>
#include <p8est_algorithms.h>
#include <vector>

int p4est_utils_prepare(std::vector<p8est_t *> p4ests) {
  if (tb == 0) {
    tb = new std::vector<p4est_utils_synced_tree_boundary_t>();
  } else {
    for (int i = 0; i < tb->size(); ++i) {
      free(tb->at(i).tree_quadrant_offset_synced);
    }
    tb->clear();
  }

  p8est_t *p4est;
  p8est_tree_t *tree;
  p4est_utils_synced_tree_boundary_t insert_elem;

  for (int i = 0; i < p4ests.size(); ++i) {
    // fetch p4est from list of p4ests
    p4est = p4ests.at(i);

    // fill element to insert
    insert_elem.p4est = p4est;
    insert_elem.tree_quadrant_offset_synced = (p4est_locidx_t *)calloc(
        p4ests.at(i)->trees->elem_count, sizeof(p4est_locidx_t));

    // allocate a local send buffer to insert local quadrant offsets
    int *local_tree_offsets = (p4est_locidx_t *)calloc(
        p4ests.at(i)->trees->elem_count, sizeof(p4est_locidx_t));

    // fetch last tree index from last processor
    int last_tree_prev_rank = -1;
    if (p4est->mpirank != p4est->mpisize - 1) {
      MPI_Send(&p4est->last_local_tree, 1, MPI_INT32_T, p4est->mpirank + 1,
               p4est->mpirank, p4est->mpicomm);
    }
    if (p4est->mpirank != 0) {
      MPI_Recv(&last_tree_prev_rank, 1, MPI_INT32_T, p4est->mpirank - 1,
               p4est->mpirank - 1, p4est->mpicomm, MPI_STATUS_IGNORE);
    }
    // only fill local send buffer if current process is not empty
    if (p4est->local_num_quadrants != 0) {
      // set start index; if first tree is not completetly owned by current
      // process it will set a wrong quadrant offset
      int start_idx = (p4est->first_local_tree == last_tree_prev_rank)
                          ? p4est->first_local_tree + 1
                          : p4est->first_local_tree;
      for (int i = start_idx; i <= p4est->last_local_tree; ++i) {
        tree = p8est_tree_array_index(p4est->trees, i);
        local_tree_offsets[i] = tree->quadrants_offset +
                                p4est->global_first_quadrant[p4est->mpirank];
      }
    }
    // synchronize offsets and insert into tb vector
    MPI_Allreduce(local_tree_offsets, insert_elem.tree_quadrant_offset_synced,
                  p4est->trees->elem_count, MPI_LONG_INT, MPI_MAX,
                  p4est->mpicomm);
    tb->push_back(insert_elem);

    // cleanup
    free(local_tree_offsets);
  }
  return 0;
}

int p4est_utils_pos_to_proc(p8est_t *p4est, double pos[3]) {
#ifdef LB_ADAPTIVE
  int qid = p4est_utils_pos_morton_idx_global(p4est, pos);

  std::vector<int> search_space(p4est->global_first_quadrant,
                                p4est->global_first_quadrant + p4est->mpisize);
  std::vector<int>::iterator it =
      std::lower_bound(search_space.begin(), search_space.end(), qid);
  int p = std::distance(search_space.begin(), it);

  if (it != search_space.begin())
    --p;
  P4EST_ASSERT(0 <= p && p < p4est->mpisize);

#if 0
  fprintf(stderr, "[p%i] mapped pos %lf %lf %lf to qid %i on proc %i\n",
          dd.p4est->mpirank, pos[0], pos[1], pos[2], qid, p);
  fprintf(stderr, "[p%i] qid %i is quad %li on p %i\n", dd.p4est->mpirank, qid,
          qid - dd.p4est->global_first_quadrant[p], p);
#endif // 0
  return p;
#else  // LB_ADAPTIVE
  // compute morton index of cell to which this position belongs
  int64_t idx = dd_p4est_pos_morton_idx(pos);
  // Note: Since p4est_space_idx is a ordered list, it is possible to do a
  // binary search here.
  // Doing so would reduce the complexity from O(N) to O(log(N))
  if (idx >= 0) {
    for (int i = 1; i <= n_nodes; ++i) {
      // compare the first cell of a process with this cell
      if (p4est_space_idx[i] > idx)
        return i - 1;
    }
  }
  fprintf(stderr, "Could not resolve the proc of particle %lf %lf %lf\n",
          pos[0], pos[1], pos[2]);
  errexit();
#endif // LB_ADAPTIVE
}

int64_t p4est_utils_cell_morton_idx(int x, int y, int z) {
  // p4est_quadrant_t c;
  // c.x = x; c.y = y; c.z = z;
  /*if (x < 0 || x >= grid_size[0])
    runtimeErrorMsg() << x << "x" << y << "x" << z << " no valid cell";
  if (y < 0 || y >= grid_size[1])
    runtimeErrorMsg() << x << "x" << y << "x" << z << " no valid cell";
  if (z < 0 || z >= grid_size[2])
    runtimeErrorMsg() << x << "x" << y << "x" << z << " no valid cell";*/

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

  // c.level = P4EST_QMAXLEVEL;
  // return p4est_quadrant_linear_id(&c,P4EST_QMAXLEVEL);
}

int64_t p4est_utils_pos_morton_idx_global(p8est_t *p4est, double pos[3]) {
  // find correct tree
  int tid = -1;
  for (int t = 0; t < p4est->connectivity->num_trees; ++t) {
    std::array<double, 3> c[P4EST_CHILDREN];
    for (int ci = 0; ci < P4EST_CHILDREN; ++ci) {
      int v = p4est->connectivity->tree_to_vertex[t * P4EST_CHILDREN + ci];
      c[ci][0] = p4est->connectivity->vertices[P4EST_DIM * v + 0];
      c[ci][1] = p4est->connectivity->vertices[P4EST_DIM * v + 1];
      c[ci][2] = p4est->connectivity->vertices[P4EST_DIM * v + 2];
    }
    std::array<double, 3> pos_min = {0., 0., 0.};
    std::array<double, 3> pos_max = {box_l[0], box_l[1], box_l[2]};
    int idx_min, idx_max;
    double dist;
    double dist_min = DBL_MAX;
    double dist_max = DBL_MAX;
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

    if ((c[idx_min][0] <= pos[0]) && (c[idx_min][1] <= pos[1]) &&
        (c[idx_min][2] <= pos[2]) && (pos[0] < c[idx_max][0]) &&
        (pos[1] < c[idx_max][1]) && (pos[2] < c[idx_max][2])) {
      P4EST_ASSERT(-1 == tid);
      tid = t;
    }
  }

  // find correct entry in tb.
  p4est_utils_synced_tree_boundary_t current_p4est;
  for (int i = 0; i < tb->size(); ++i) {
    if (p8est_is_equal(p4est, tb->at(i).p4est, 0)) {
      current_p4est = tb->at(i);
      break;
    }
  }

  p4est_tree_t *tree = p4est_tree_array_index(p4est->trees, tid);
  int level = tree->maxlevel;

  double tmp[3] = {pos[0] - (int)pos[0], pos[1] - (int)pos[1],
                   pos[2] - (int)pos[2]};
  int nq = 1 << level;
  int qpos[3];
  for (int i = 0; i < 3; ++i) {
    qpos[i] = tmp[i] * nq;
    P4EST_ASSERT(0 <= qpos[i] && qpos[i] < nq);
  }

  int qid = p4est_utils_cell_morton_idx(qpos[0], qpos[1], qpos[2]);

  qid += current_p4est.tree_quadrant_offset_synced[tid];

  return qid;
}

int64_t p4est_utils_pos_morton_idx_local(p8est_t *p4est, double pos[3]) {
  int idx = p4est_utils_pos_morton_idx_global(p4est, pos);
  idx -= p4est->global_first_quadrant[p4est->mpirank];
  return idx;
}

template <typename T>
int p4est_utils_post_gridadapt_map_data(p8est_t *p4est_old,
                                        p8est_mesh_t *mesh_old,
                                        p8est_t *p4est_new,
                                        T **local_data_levelwise,
                                        T *mapped_data_flat) {
  // counters
  int tid_old = p4est_old->first_local_tree;
  int tid_new = p4est_new->first_local_tree;
  int qid_old = 0, qid_new = 0;
  int tqid_old = 0, tqid_new = 0;

  // trees
  p8est_tree_t *curr_tree_old =
      p8est_tree_array_index(p4est_old->trees, tid_old);
  p8est_tree_t *curr_tree_new =
      p8est_tree_array_index(p4est_new->trees, tid_new);
  // quadrants
  p8est_quadrant_t *curr_quad_old, *curr_quad_new;

  int level_old, sid_old;
  int level_new;
  while (qid_old < p4est_old->local_num_quadrants &&
         qid_new < p4est_new->local_num_quadrants) {
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
    sid_old = mesh_old->quad_qreal_offset[qid_old];

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
    } else if (level_old + 1 == level_new) {
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
    } else if (level_old == level_new + 1) {
      // old cell has been refined
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
  return 0;
}

template <typename T>
int p4est_utils_post_gridadapt_data_partition_transfer(
    p8est_t *p4est_old, p8est_t *p4est_new, T *data_mapped,
    std::vector<T> **data_partitioned) {
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
  sc_MPI_Request *r;
  sc_array_t *requests;
  requests = sc_array_new(sizeof(sc_MPI_Request));

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

    data_length = std::max(0,
                           std::min(ub_old_remote, ub_new_local) -
                               std::max(lb_old_remote, ub_new_local));

    // allocate receive buffer and wait for messages
    data_partitioned[p] = new std::vector<T>(data_length);
    r = (sc_MPI_Request *)sc_array_push(requests);
    mpiret = sc_MPI_Irecv((void *)data_partitioned[p]->begin(),
                          data_length * sizeof(T), sc_MPI_BYTE, p, 0,
                          p4est_new->mpicomm, r);
    SC_CHECK_MPI(mpiret);
  }

  // send respective quadrants to other processors
  for (int p = 0; p < size; ++p) {
    lb_new_remote = ub_new_remote;
    ub_new_remote = p4est_new->global_first_quadrant[p + 1];

    data_length = std::max(0,
                           std::min(ub_old_local, ub_new_remote) -
                               std::max(lb_old_local, ub_new_remote));

    r = (sc_MPI_Request *)sc_array_push(requests);
    mpiret = sc_MPI_Isend((void *)(data_mapped + send_offset * sizeof(T)),
                          data_length * sizeof(T), sc_MPI_BYTE, p, 0,
                          p4est_new->mpicomm, r);
    SC_CHECK_MPI(mpiret);
    send_offset += data_length;
  }

  /** Wait for communication to finish */
  mpiret =
      sc_MPI_Waitall(requests->elem_count, (sc_MPI_Request *)requests->array,
                     sc_MPI_STATUSES_IGNORE);
  SC_CHECK_MPI(mpiret);
  sc_array_destroy(requests);

  return 0;
}

template <typename T>
int p4est_utils_post_gridadapt_insert_data(p8est_t *p4est_new,
                                           p8est_mesh_t *mesh_new,
                                           std::vector<T> **data_partitioned,
                                           T **data_levelwise) {
  int size = p4est_new->mpisize;
  // counters
  int tid = p4est_new->first_local_tree;
  int qid = 0;
  int tqid = 0;

  // trees
  p8est_tree_t *curr_tree = p8est_tree_array_index(p4est_new->trees, tid);
  // quadrants
  p8est_quadrant_t *curr_quad;

  int level, sid;

  for (int p = 0; p < size; ++p) {
    for (int q = 0; q < data_partitioned[p]->size(); ++q) {
      // wrap multiple trees
      if (tqid == curr_tree->quadrants.elem_count) {
        ++tid;
        P4EST_ASSERT(tid < p4est_new->trees->elem_count);
        curr_tree = p8est_tree_array_index(p4est_new->trees, tid);
        tqid = 0;
      }
      curr_quad = p8est_quadrant_array_index(&curr_tree->quadrants, tqid);
      level = curr_quad->level;
      sid = mesh_new->quad_qreal_offset[qid];
      std::memcpy(&data_levelwise[level][sid], &data_partitioned[p]->at(q),
                  sizeof(T));
      ++tqid;
      ++qid;
    }
  }

  // verify that all real quadrants have been processed
  P4EST_ASSERT(qid == mesh_new->local_num_quadrants);

  return 0;
}

void p4est_utils_partition_multiple_forests(p8est_t *p4est_ref,
                                            p8est_t *p4est_mod) {
  P4EST_ASSERT(p4est_ref->mpisize == p4est_mod->mpisize);
  P4EST_ASSERT(p4est_ref->mpirank == p4est_mod->mpirank);
  P4EST_ASSERT(p8est_connectivity_is_equal(p4est_ref->connectivity,
                                           p4est_mod->connectivity));

  p4est_locidx_t num_quad_per_proc[p4est_ref->mpisize];
  p4est_locidx_t num_quad_per_proc_global[p4est_ref->mpisize];
  for (int p = 0; p < p4est_mod->mpisize; ++p) {
    num_quad_per_proc[p] = 0;
    num_quad_per_proc_global[p] = 0;
  }

  int tid = p4est_mod->first_local_tree;
  int tqid = 0;
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
      int proc = p4est_utils_pos_to_proc(p4est_ref, xyz);
      ++num_quad_per_proc[proc];
    }
    ++tqid;
  }

  // Gather this information over all processes
  MPI_Allreduce(num_quad_per_proc, num_quad_per_proc_global, p4est_mod->mpisize,
                P4EST_MPI_LOCIDX, MPI_SUM, p4est_mod->mpicomm);

  p4est_locidx_t sum = 0;

  for (int i = 0; i < p4est_mod->mpisize; ++i)
    sum += num_quad_per_proc_global[i];
  if (sum < p4est_mod->global_num_quadrants) {
    printf("%i : quadrants lost while partitioning\n", this_node);
    return;
  }

  CELL_TRACE(printf("%i : repartitioned LB %i\n", this_node,
                    num_quad_per_proc_global[this_node]));

  // Repartition with the computed distribution
  int shipped = p8est_partition_given(p4est_mod, num_quad_per_proc_global);
  P4EST_GLOBAL_PRODUCTIONF(
      "Done " P8EST_STRING "_partition shipped %lld quadrants %.3g%%\n",
      (long long)shipped, shipped * 100. / p4est_mod->global_num_quadrants);
}

#endif // defined (LB_ADAPTIVE) || defined (DD_P4EST)
