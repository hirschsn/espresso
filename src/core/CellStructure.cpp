/*
 * Copyright (C) 2010-2020 The ESPResSo project
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

#include "CellStructure.hpp"

#include "bonded_interactions/bonded_interaction_data.hpp"

void CellStructure::remove_particle(int id) {
  Cell *cell = nullptr;
  int position = -1;
  for (auto c : m_local_cells) {
    auto parts = c->particles();

    for (unsigned i = 0; i < parts.size(); i++) {
      auto &p = parts[i];

      if (p.identity() == id) {
        cell = c;
        position = static_cast<int>(i);
      } else {
        remove_all_bonds_to(p, id);
      }
    }
  }

  /* If we found the particle, remove it. */
  if (cell && (position >= 0)) {
    cell->extract(position);
    update_particle_index(id, nullptr);
    update_particle_index(cell);
  }
}

Particle *CellStructure::add_local_particle(Particle &&p) {
  auto const sort_cell = particle_to_cell(p);
  if (sort_cell) {
    append_indexed_particle(sort_cell, std::move(p));

    return &sort_cell->back();
  }

  return {};
}

Particle *CellStructure::add_particle(Particle &&p) {
  auto const sort_cell = particle_to_cell(p);
  /* There is always at least one cell, so if the particle
   * does not belong to a cell on this node we can put it there. */
  auto cell = sort_cell ? sort_cell : local_cells()[0];

  /* If the particle isn't local a global resort may be
   * needed, otherwise a local resort if sufficient. */
  set_resort_particles(sort_cell ? Cells::RESORT_LOCAL : Cells::RESORT_GLOBAL);

  append_indexed_particle(cell, std::move(p));

  return &cell->back();
}

int CellStructure::get_max_local_particle_id() const {
  auto it = std::find_if(m_particle_index.rbegin(), m_particle_index.rend(),
                         [](const Particle *p) { return p != nullptr; });

  return (it != m_particle_index.rend()) ? (*it)->identity() : -1;
}

void CellStructure::remove_all_particles() {
  for (auto c : m_local_cells) {
    for (auto &p : c->particles()) {
      p.~Particle();
    }

    c->clear();
  }

  m_particle_index.clear();
}
