from script_interface import PScriptInterface


class __PyFlowfield(object):

    def __init__(self):
        self._instance = PScriptInterface(
            "ScriptInterface::Flowfield::Flowfield")

    def load(self, prefix=None):
        """Loads a flowfield."""
        if PY_USE_FLOWFIELD == 0:
            raise RuntimeError("Flowfield support not compiled in.")
        if prefix is None:
            raise ValueError(
                "Need to supply output prefix via 'prefix' kwarg.")

        self._instance.set_params(prefix=prefix)

    def size(self):
        if PY_USE_FLOWFIELD == 0:
            raise RuntimeError("Flowfield support not compiled in.")
        return PY_FLOWFIELD_SIZE

    def vel(self, pos):
        return self._instance.call_method("fluid_velocity", pos=pos)


flowfield = __PyFlowfield()
