import tkinter as tk
import sys
from interfaz import InterfazGrafica

def on_closing(root, app=None):
    if app and hasattr(app, 'timer_id') and app.timer_id is not None:
        try:
            root.after_cancel(app.timer_id)
            app.timer_id = None
        except Exception:
            pass
    root.quit()
    root.destroy()
    sys.exit()

if __name__ == "__main__":
    root = tk.Tk()
    app = InterfazGrafica(root)
    root.protocol("WM_DELETE_WINDOW", lambda: on_closing(root, app))
    root.mainloop()