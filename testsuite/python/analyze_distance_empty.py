# Copyright (C) 2010-2019 The ESPResSo project
#
# This file is part of ESPResSo.
#
# ESPResSo is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# ESPResSo is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
import unittest as ut
import espressomd


class AnalyzeDistanceEmpty(ut.TestCase):
    system = espressomd.System(box_l=[1.0, 1.0, 1.0])

    def test_min_dist(self):
        self.assertEqual(self.system.analysis.min_dist(), float("inf"))

if __name__ == "__main__":
    ut.main()
