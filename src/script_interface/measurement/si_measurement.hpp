/*
 * Copyright (C) 2010-2019 The ESPResSo project
 * Copyright (C) 2002,2003,2004,2005,2006,2007,2008,2009,2010
 *   Max-Planck-Institute for Polymer Research, Theory Group
 *
 * This file is part of ESPResSo.
 *
 * ESPResSo is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ESPResSo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ESPRESSO_SCRIPTINTERFACE_MEASUREMENT_HPP
#define ESPRESSO_SCRIPTINTERFACE_MEASUREMENT_HPP

#include <boost/mpi/collectives.hpp>

#include "config.hpp"
#include "script_interface/ScriptInterface.hpp"
#include "script_interface/auto_parameters/AutoParameters.hpp"
#include "script_interface/get_value.hpp"
#include "core/communication.hpp"


extern double _integrate_vv_time;
extern double _force_calc_time;
extern double _srloop_time;


namespace ScriptInterface {
namespace Measurement {

double &get_var(const std::string &name)
{
  if (name == "integrate")
    return _integrate_vv_time;
  else if (name == "force_calc")
    return _force_calc_time;
  else if (name == "short_range")
    return _srloop_time;
  else
    throw std::runtime_error("No such var.");
}

class MeasurementScript : public AutoParameters<MeasurementScript> {
public:
  MeasurementScript() { add_parameters({}); }

  Variant call_method(const std::string &name,
                      const VariantMap &parameters) override {

    if (name == "reset_all") {
      _integrate_vv_time = 0.0;
      _force_calc_time = 0.0;
      _srloop_time = 0.0;
    } else if (name == "reset") {
      get_var(get_value<std::string>(parameters.at("var"))) = 0.0;
    } else if (name == "get") {
      double d = get_var(get_value<std::string>(parameters.at("var")));
      std::vector<double> timings;
      boost::mpi::gather(comm_cart, d, timings, 0);
      return timings;
    }
    return {};
  }
};

} // namespace MPIIO
} // namespace ScriptInterface

#endif
