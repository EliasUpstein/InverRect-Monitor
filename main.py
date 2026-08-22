import tkinter as tk
import sys
from interfaz import InterfazGrafica

def on_closing(root):
    root.quit()
    root.destroy()
    sys.exit()

if __name__ == "__main__":
    root = tk.Tk()
    root.protocol("WM_DELETE_WINDOW", lambda: on_closing(root))
    app = InterfazGrafica(root)
    root.mainloop()