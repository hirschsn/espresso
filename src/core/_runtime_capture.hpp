
#pragma once

#include <mpi.h>
#include <vector>

namespace _runtime_capture {
namespace _impl {
extern std::vector<double> cellruntimes;

struct RuntimeRecorder {
  RuntimeRecorder(int cellidx) : _cellidx(cellidx), _t_start(MPI_Wtime()) {}
  ~RuntimeRecorder() { cellruntimes[_cellidx] += MPI_Wtime() - _t_start; }

private:
  const int _cellidx;
  const double _t_start;
};
}

typedef _impl::RuntimeRecorder record_cell;

void reset_runtime_recording();
std::vector<size_t> get_local_h();
std::vector<double> get_local_r();
double get_local_l();

}

// TODO: Captute einbauen und script interface