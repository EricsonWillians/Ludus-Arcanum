#include "bindings.hpp"

#include <pybind11/pybind11.h>

PYBIND11_MODULE(_native, module) { ludus::python_detail::bind_native_module(module); }
