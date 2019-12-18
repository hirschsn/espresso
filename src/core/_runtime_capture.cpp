
#include "cells.hpp"
#include <vector>

namespace _runtime_capture {
namespace _impl {

std::vector<double> cellruntimes;
}

void reset_runtime_recording() {
  _impl::cellruntimes.clear();
  _impl::cellruntimes.resize(local_cells.n, 0.0);
}

std::vector<size_t> get_local_h() {
  std::vector<size_t> h;

  for (const auto &c : local_cells) {
    auto npart = c->n;
    if (h.size() <= npart)
      h.resize(npart + 1, 0);
    h[npart]++;
  }
  return h;
}

std::vector<double> get_local_r() {
  std::vector<double> r;
  assert(_impl::cellruntimes.size() == local_cells.n);

  for (size_t i = 0; i < local_cells.n; ++i) {
    auto npart = local_cells.cell[i]->n;
    if (r.size() <= npart)
      r.resize(npart + 1, 0.0);
    r[npart] += _impl::cellruntimes[i];
  }

  return r;
}

double get_local_l() {
  return std::accumulate(std::begin(_impl::cellruntimes),
                         std::end(_impl::cellruntimes), 0.0);
}

} // namespace _runtime_capture