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

#ifndef ESPRESSO_SCRIPTINTERFACE_RUNTIME_CAPTURE_HPP
#define ESPRESSO_SCRIPTINTERFACE_RUNTIME_CAPTURE_HPP

#include "ScriptInterface.hpp"
#include "auto_parameters/AutoParameters.hpp"
#include "config.hpp"
#include "get_value.hpp"
#include <core/_runtime_capture.hpp>
#include <boost/mpi/collectives.hpp>
#include <core/communication.hpp>


namespace ScriptInterface {
namespace RuntimeCapture {


template <typename T>
std::vector<T> allgather(boost::mpi::communicator &comm, T &&value)
{
  std::vector<T> result;
  boost::mpi::all_gather(comm, value, result);
  assert(result.size() == comm.size());
  return result;
}

class RuntimeCapture : public AutoParameters<RuntimeCapture> {
public:
  RuntimeCapture() { add_parameters({}); }

  Variant call_method(const std::string &name,
                      const VariantMap &parameters) override {

    if (name == "get_h")
      return allgather(comm_cart, _runtime_capture::get_local_h());
    else if (name == "get_r")
      return allgather(comm_cart, _runtime_capture::get_local_r());
    else if (name == "get_l")
      return allgather(comm_cart, _runtime_capture::get_local_l());
    else if (name == "reset")
      _runtime_capture::reset_runtime_recording();

    return {};
  }
};

} // namespace MPIIO
} // namespace ScriptInterface

#endif
