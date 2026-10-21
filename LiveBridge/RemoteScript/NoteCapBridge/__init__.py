"""NoteCapBridge: a Live 12 control surface that serves JSON commands on localhost.

Select it in Settings > Link, Tempo & MIDI > Control Surface (no MIDI ports needed).
The socket is polled from update_display, Live's ~100 ms main-thread tick, so every
Live Object Model call happens on the main thread. See LiveBridge/README.md.
"""

from _Framework.ControlSurface import ControlSurface

from .commands import Commands
from .server import BridgeServer, DEFAULT_HOST, DEFAULT_PORT


def create_instance(c_instance):
    return NoteCapBridge(c_instance)


class NoteCapBridge(ControlSurface):
    def __init__(self, c_instance):
        ControlSurface.__init__(self, c_instance)
        self._server = None
        self._commands = Commands(self.song, self.application, log=self._log)
        try:
            self._server = BridgeServer(self._commands.dispatch, DEFAULT_HOST, DEFAULT_PORT, log=self._log)
            self._log("listening on %s:%d" % self._server.address)
            self.show_message("NoteCapBridge listening on port %d" % DEFAULT_PORT)
        except OSError as e:
            self._log("could not listen on port %d: %s" % (DEFAULT_PORT, e))
            self.show_message("NoteCapBridge: port %d busy (%s)" % (DEFAULT_PORT, e))

    def _log(self, msg):
        self.log_message("NoteCapBridge: " + msg)

    def update_display(self):
        ControlSurface.update_display(self)
        if self._server is not None:
            try:
                self._server.poll()
            except Exception as e:  # never let the bridge break Live's tick
                self._log("poll error: %r" % e)

    def disconnect(self):
        if self._server is not None:
            self._server.close()
            self._server = None
            self._log("closed")
        ControlSurface.disconnect(self)
