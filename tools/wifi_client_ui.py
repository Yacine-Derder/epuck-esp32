"""Single full-screen renderer. No socket work or out-of-band terminal writes."""
import asyncio
from collections import deque

from prompt_toolkit import Application
from prompt_toolkit.document import Document
from prompt_toolkit.filters import Condition
from prompt_toolkit.history import History
from prompt_toolkit.key_binding import KeyBindings
from prompt_toolkit.layout import ConditionalContainer, HSplit, Layout, Window
from prompt_toolkit.layout.controls import FormattedTextControl
from prompt_toolkit.layout.dimension import Dimension
from prompt_toolkit.styles import Style
from prompt_toolkit.widgets import Frame, TextArea


class CommandHistory(History):
    """Session-local, bounded history. Recalling a command never submits it."""
    def __init__(self):
        super().__init__()
        self.entries = deque(maxlen=100)

    def load_history_strings(self):
        yield from reversed(tuple(self.entries))

    async def load(self):
        for entry in self.load_history_strings():
            yield entry

    def get_strings(self):
        return list(self.entries)

    def store_string(self, string):
        if string.strip() and len(string) <= 128:
            self.entries.append(string)

    def append_string(self, string):
        self.store_string(string)


class TerminalUI:
    def __init__(self, state, submit, done, stop, sensor_text, *, input=None, output=None):
        self.state, self.submit, self.done, self.stop = state, submit, done, stop
        self.view = state.snapshot()
        self.history_offset = 0
        self.oversized_input = False
        self.revision = None
        self.history = CommandHistory()
        self.events = TextArea(read_only=True, focusable=False, scrollbar=True,
                               height=Dimension(min=1), wrap_lines=True)
        self.command = TextArea(height=1, prompt="Command > ", multiline=False,
                                wrap_lines=False, accept_handler=self.accept, history=self.history)
        # Only user edits/acceptance may change this Buffer. Refreshes never
        # replace its Document, selection, cursor, history or undo state.
        def limit_input(buffer):
            if len(buffer.text) > 128:
                self.oversized_input = True
                buffer.document = Document(buffer.text[:128], 128)
                state.event("Rejected: oversized input; Enter will discard this command")
        self.command.buffer.on_text_changed += limit_input

        bindings = KeyBindings()

        @bindings.add("c-c", eager=True)
        @bindings.add("c-d", eager=True)
        def exit_input(event):
            stop.set()
            event.app.exit()

        @bindings.add("pageup", eager=True)
        def older(event):
            self.history_offset = max(0, min(len(self.view[2])-1, self.history_offset+10))
            self.refresh_events(force=True)

        @bindings.add("pagedown", eager=True)
        def newer(event):
            self.history_offset = max(0, self.history_offset-10)
            self.refresh_events(force=True)

        # On resize, sacrifice panel space before the editable bottom row.
        # No terminal dimensions select plain mode or recreate the input Buffer.
        def rows():
            return self.app.output.get_size().rows

        root = HSplit([
            ConditionalContainer(Window(FormattedTextControl(lambda: [("class:status", self.view[0])]),
                                        height=1), Condition(lambda: rows() >= 2)),
            ConditionalContainer(
                Frame(Window(FormattedTextControl(lambda: sensor_text(self.view[1])),
                             height=Dimension(min=1, preferred=6, max=8), wrap_lines=True),
                      title="Latest sensors (raw)"), Condition(lambda: rows() >= 14)),
            ConditionalContainer(Frame(self.events, title="Events (last 200; PgUp/PgDn scroll)"),
                                 Condition(lambda: rows() >= 6)),
            ConditionalContainer(Window(height=Dimension(min=0)), Condition(lambda: rows() < 6)),
            ConditionalContainer(Window(FormattedTextControl(
                "HELP | STREAM ms | STREAM_STATUS | Up/Down history | Ctrl-C/D exit"),
                height=1), Condition(lambda: rows() >= 10)),
            self.command,  # Always last, always one row; focus never moves on updates.
        ])
        self.app = Application(layout=Layout(root, focused_element=self.command),
                               key_bindings=bindings, full_screen=True,
                               min_redraw_interval=0.1,
                               style=Style.from_dict({"status": "bold", "frame.label": "bold"}),
                               input=input, output=output)

    def accept(self, buffer):
        if self.oversized_input:
            self.state.event("Rejected: oversized command discarded; re-enter it")
            self.oversized_input = False
        else:
            self.submit(buffer.text)
        # Only Enter resets the field. The framework retains bounded history.
        return False

    def refresh_events(self, *, force=False):
        events = self.view[2]
        revision = events[-1][0] if events else 0
        if force or revision != self.revision:
            end = max(0, len(events)-self.history_offset)
            text = "\n".join(text for _, text in events[:end])
            self.events.buffer.set_document(Document(text, len(text)), bypass_readonly=True)
            self.revision = revision

    async def watch(self):
        try:
            while not self.done.is_set():
                self.view = self.state.snapshot()
                self.refresh_events()
                self.app.invalidate()  # Sole background-update route, on the UI loop.
                await asyncio.sleep(0.1)
            self.view = self.state.snapshot()
            self.refresh_events()
            self.app.exit()  # No Enter needed after loss, even with a partial edit.
        except Exception as error:
            self.stop.set()
            self.app.exit(exception=error)  # Report only after the terminal is restored.

    def run(self):
        def start():
            def loop_error(loop, context):
                self.stop.set()
                error = context.get("exception") or RuntimeError(context.get("message", "UI failure"))
                if self.app.is_running:
                    self.app.exit(exception=error)
            asyncio.get_running_loop().set_exception_handler(loop_error)
            self.app.create_background_task(self.watch())
        try:
            # Do not invoke prompt_toolkit's print-and-prompt exception handler.
            self.app.run(pre_run=start, set_exception_handler=False)
        finally:
            self.stop.set()
