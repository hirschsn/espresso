#ifndef CORE_TABULATED_POTENTIAL_HPP
#define CORE_TABULATED_POTENTIAL_HPP

#include "utils/linear_interpolation.hpp"
#include "utils/serialization/List.hpp"

#include <boost/serialization/access.hpp>
#include <boost/serialization/vector.hpp>

#include <vector>
#include <cassert>

struct TabulatedCollisionProbability
  double minval = -1.0;
  double maxval = -1.0;
  double invstepsize = 0.0;
  std::vector<double> collision_probability_tab;

  double collision_probability(double x) const {
    assert(x <= maxval);
    return Utils::linear_interpolation(collision_probability_tab, invstepsize, minval, x);
  }

  double cutoff() const { return maxval; }

private:
  friend boost::serialization::access;
  template <typename Archive>
  void serialize(Archive &ar, long int /* version */) {
    ar &minval;
    ar &maxval;
    ar &invstepsize;
    ar &collision_probability_tab;
  }
};

#endif
