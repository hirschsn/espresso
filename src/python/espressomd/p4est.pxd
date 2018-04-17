#
# Copyright (C) 2013,2014,2015,2016 The ESPResSo project
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
#

from __future__ import print_function, absolute_import
include "myconfig.pxi"
from libcpp cimport bool
from libcpp.vector cimport vector
from .actors cimport Actor

cdef class GridInteraction(Actor):
    pass

IF LB_ADAPTIVE or EK_ADAPTIVE or ES_ADAPTIVE:

    ##############################################
    #
    # extern functions and structs
    #
    ##############################################

    cdef extern from "p4est_utils.hpp":

        ##############################################
        #
        # Python p4est struct clone of C-struct
        #
        ##############################################
        ctypedef struct p4est_parameters:
            int min_ref_level
            int max_ref_level

        ##############################################
        #
        # init struct
        #
        ##############################################
        ctypedef struct p4est_parameters:
            p4est_parameters p4est_params


        ##############################################
        #
        # exported C-functions from p4est_utils.hpp
        #
        ##############################################
        int p4est_utils_set_min_level(int c_lvl)
        int p4est_utils_set_max_level(int c_lvl)
        int p4est_utils_get_min_level(int *c_lvl)
        int p4est_utils_get_max_level(int *c_lvl)


    ###############################################
    #
    # Wrapper-functions for access to C-pointer: Set params
    #
    ###############################################
    cdef inline python_p4est_set_min_level(p_lvl):

        cdef int c_lvl
        # get pointers
        c_lvl = p_lvl
        # call c-function
        if(p4est_utils_set_min_level(c_lvl)):
            raise Exception("p4est_utils_set_min_level error at C-level"
                            " interface")

        return 0

    ###############################################

    cdef inline python_p4est_set_max_level(p_lvl):

        cdef int c_lvl
        # get pointers
        c_lvl = p_lvl
        # call c-function
        if(p4est_utils_set_max_level(c_lvl)):
            raise Exception("p4est_utils_set_max_level error at C-level"
                            " interface")

        return 0

    ###############################################


    ###############################################
    #
    # Wrapper-functions for access to C-pointer: Get params
    #
    ###############################################
    cdef inline python_p4est_get_min_level(p_lvl):

        cdef int c_lvl[1]
        # call c-function
        if(p4est_utils_get_min_level(c_lvl)):
            raise Exception("p4est_utils_get_min_level error at C-level interface")
        if(isinstance(c_lvl, float)):
            p_lvl = <int > c_lvl[0]
        else:
            p_lvl = c_lvl

        return 0

    ###############################################

    cdef inline python_p4est_get_max_level(p_lvl):

        cdef int c_lvl[1]
        # call c-function
        if(p4est_utils_get_max_level(c_lvl)):
            raise Exception("p4est_utils_get_max_level error at C-level interface")
        if(isinstance(c_lvl, float)):
            p_lvl = <int > c_lvl[0]
        else:
            p_lvl = c_lvl

        return 0

    ###############################################
