
/*
  Copyright (C) 2010,2011,2012,2013,2014 The ESPResSo project
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

#ifndef ESPRESSO_SCRIPTINTERFACE_FLOWFIELD_FLOWFIELD_HPP
#define ESPRESSO_SCRIPTINTERFACE_FLOWFIELD_FLOWFIELD_HPP

#include "ScriptInterface.hpp"
#include "thermostat.hpp"
#include <string>

namespace ScriptInterface {
namespace Flowfield {

class SIFlowfield : public ScriptInterfaceBase {
public:
  SIFlowfield() {}

  ParameterMap valid_parameters() const override {
    return {{"prefix", {ParameterType::STRING, true}}};
  }

  VariantMap get_parameters() const override {
    return {{"prefix", m_prefix}};
  }

  void set_parameter(const std::string &name, const Variant &value) override {
    if (name == "prefix") {
#ifdef USE_FLOWFIELD
      m_prefix = boost::get<std::string>(value);
      ff_name_u = m_prefix + ".u";
      ff_name_v = m_prefix + ".v";
      ff_name_w = m_prefix + ".w";
      fluid_init();
#else
      fprintf(stderr, "Error: USE_FLOWFIELD not defined at compile time.\n");
      errexit();
#endif
    }
  }

  Variant call_method(const std::string &name,
                      const VariantMap &parameters) override {
    if (name == "fluid_velocity") {
        std::vector<double> pos = boost::get<std::vector<double>>(parameters.at("pos"));
        if (pos.size() != 3) {
            throw std::runtime_error("Pos has wrong size. Needs to be a 3d vector.");
        }

        std::vector<double> fv(3, 0.0);
        fluid_velocity(pos.data(), fv.data());
        return fv;
    }

    return {};
  }

private:
  std::string m_prefix;
};

}
}

#endif
