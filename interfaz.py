import tkinter as tk
from tkinter import ttk, messagebox
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from calculos import AnalizadorDePotencia

# ==========================================
# MÓDULO 2: INTERFAZ GRÁFICA (La "Vista")
# ==========================================
class InterfazGrafica:
    def __init__(self, root):
        self.root = root
        self.root.title("Analizador de Potencia - Electrónica de Potencia")
        self.root.geometry("1100x700")
        self.root.configure(bg="#f4f6f9")
        
        # Estilos modernos
        style = ttk.Style()
        if "clam" in style.theme_names():
            style.theme_use("clam")
        style.configure("TLabel", background="#f4f6f9", font=("Segoe UI", 12), foreground="#333333")
        style.configure("TLabelframe", background="#f4f6f9", bordercolor="#d1d5db")
        style.configure("TLabelframe.Label", background="#f4f6f9", font=("Segoe UI", 13, "bold"), foreground="#2c3e50")
        style.configure("TButton", font=("Segoe UI", 12, "bold"), padding=6, background="#3498db", foreground="white")
        style.map("TButton", background=[("active", "#2980b9")])
        style.configure("TFrame", background="#f4f6f9")
        style.configure("TCheckbutton", background="#f4f6f9", font=("Segoe UI", 12))
        
        self.root.option_add("*TCombobox*Listbox.font", ("Segoe UI", 12))
        
        # Instanciar el modelo matemático
        self.analizador = AnalizadorDePotencia()
        
        # Opciones de señales
        self.tipos_senal = [
            "1. Senoidal", "2. Cuadrada", "3. Cuasi-cuadrada", 
            "4. RMMO", "5. RMOC C (Controlado)", "6. Muestras ADC (Simuladas)"
        ]
        
        self.crear_widgets()

    def crear_widgets(self):
        # --- Panel Izquierdo: Controles ---
        frame_izq = ttk.Frame(self.root)
        frame_izq.pack(side=tk.LEFT, fill=tk.Y, padx=15, pady=15)

        frame_controles = ttk.LabelFrame(frame_izq, text="Parámetros de Entrada", padding="15")
        frame_controles.pack(fill=tk.X, pady=(0, 15))

        ttk.Label(frame_controles, text="Tipo de Señal:").grid(row=0, column=0, sticky="w", pady=5)
        self.cb_tipo = ttk.Combobox(frame_controles, values=self.tipos_senal, state="readonly", width=25, font=("Segoe UI", 12))
        self.cb_tipo.current(4) # Por defecto RMOC C
        self.cb_tipo.grid(row=0, column=1, pady=5)
        self.cb_tipo.bind("<<ComboboxSelected>>", self.actualizar_estado_alfa)

        ttk.Label(frame_controles, text="Voltaje Pico (Vp):").grid(row=1, column=0, sticky="w", pady=5)
        self.ent_vp = ttk.Entry(frame_controles, width=10, font=("Segoe UI", 12))
        self.ent_vp.insert(0, "311")
        self.ent_vp.grid(row=1, column=1, sticky="w", pady=5)

        ttk.Label(frame_controles, text="Ángulo de Disparo α:").grid(row=2, column=0, sticky="w", pady=5)
        frame_alfa = ttk.Frame(frame_controles)
        frame_alfa.grid(row=2, column=1, sticky="w", pady=5)
        self.ent_alfa = ttk.Entry(frame_alfa, width=10, font=("Segoe UI", 12))
        self.ent_alfa.insert(0, "60")
        self.ent_alfa.pack(side=tk.LEFT)
        self.cb_unidad_alfa = ttk.Combobox(frame_alfa, values=["Grados", "Radianes"], state="readonly", width=10, font=("Segoe UI", 12))
        self.cb_unidad_alfa.current(0)
        self.cb_unidad_alfa.pack(side=tk.LEFT, padx=5)

        ttk.Label(frame_controles, text="Resistencia R (Ω):").grid(row=3, column=0, sticky="w", pady=5)
        self.ent_carga = ttk.Entry(frame_controles, width=10, font=("Segoe UI", 12))
        self.ent_carga.insert(0, "10")
        self.ent_carga.grid(row=3, column=1, sticky="w", pady=5)
        
        ttk.Label(frame_controles, text="Reactancia X (Ω):").grid(row=4, column=0, sticky="w", pady=5)
        self.ent_reactancia = ttk.Entry(frame_controles, width=10, font=("Segoe UI", 12))
        self.ent_reactancia.insert(0, "0")
        self.ent_reactancia.grid(row=4, column=1, sticky="w", pady=5)

        ttk.Label(frame_controles, text="Cant. de Armónicas:").grid(row=5, column=0, sticky="w", pady=5)
        self.ent_arm = ttk.Entry(frame_controles, width=10, font=("Segoe UI", 12))
        self.ent_arm.insert(0, "15")
        self.ent_arm.grid(row=5, column=1, sticky="w", pady=5)

        ttk.Label(frame_controles, text="Muestras por Ciclo:").grid(row=6, column=0, sticky="w", pady=5)
        self.ent_muestras = ttk.Entry(frame_controles, width=10, font=("Segoe UI", 12))
        self.ent_muestras.insert(0, "256")
        self.ent_muestras.grid(row=6, column=1, sticky="w", pady=5)

        btn_calcular = ttk.Button(frame_controles, text="Calcular y Graficar", command=self.procesar_datos)
        btn_calcular.grid(row=7, column=0, columnspan=2, pady=20)

        # --- Controles de Tiempo Real ---
        frame_tiempo_real = ttk.LabelFrame(frame_izq, text="Actualización en Tiempo Real", padding="10")
        frame_tiempo_real.pack(fill=tk.X, pady=(0, 15))
        
        self.var_tiempo_real = tk.BooleanVar()
        chk_tiempo_real = ttk.Checkbutton(frame_tiempo_real, text="Activar", variable=self.var_tiempo_real, command=self.toggle_tiempo_real)
        chk_tiempo_real.grid(row=0, column=0, sticky="w", pady=5)
        
        ttk.Label(frame_tiempo_real, text="Refresco (seg):").grid(row=0, column=1, sticky="w", padx=(10, 5), pady=5)
        self.ent_refresco = ttk.Entry(frame_tiempo_real, width=6, font=("Segoe UI", 12))
        self.ent_refresco.insert(0, "1.0")
        self.ent_refresco.config(state="disabled")
        self.ent_refresco.grid(row=0, column=2, sticky="w", pady=5)

        # --- Resultados en Texto ---
        frame_resultados = ttk.LabelFrame(frame_izq, text="Resultados", padding="15")
        frame_resultados.pack(fill=tk.BOTH, expand=True)

        self.lbl_resultados = tk.StringVar()
        self.lbl_resultados.set("Esperando cálculo...")
        lbl = ttk.Label(frame_resultados, textvariable=self.lbl_resultados, justify=tk.LEFT, font=("Consolas", 14))
        lbl.pack(anchor="nw")

        # --- Panel Derecho: Gráficos (Matplotlib) ---
        self.frame_grafico = ttk.Frame(self.root)
        self.frame_grafico.pack(side=tk.RIGHT, fill=tk.BOTH, expand=True, padx=15, pady=15)
        
        try:
            plt.style.use('ggplot')
        except:
            pass
            
        self.fig, self.ax = plt.subplots(figsize=(7, 5), dpi=100)
        self.fig.patch.set_facecolor('#f4f6f9')
        self.canvas = FigureCanvasTkAgg(self.fig, master=self.frame_grafico)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)
        
        # Cálculo inicial automático
        self.procesar_datos()

    def toggle_tiempo_real(self):
        if self.var_tiempo_real.get():
            self.ent_refresco.config(state="normal")
            self.bucle_actualizacion()
        else:
            self.ent_refresco.config(state="disabled")

    def bucle_actualizacion(self):
        if self.var_tiempo_real.get():
            try:
                segundos = float(self.ent_refresco.get())
                if segundos <= 0:
                    raise ValueError("El tiempo debe ser mayor a 0")
                ms = int(segundos * 1000)
                
                self.procesar_datos()
                self.root.after(ms, self.bucle_actualizacion)
            except ValueError:
                self.var_tiempo_real.set(False)
                self.ent_refresco.config(state="disabled")
                messagebox.showwarning("Error de Formato", "Por favor ingresa un número numérico válido para los segundos de refresco.")

    def actualizar_estado_alfa(self, event=None):
        """Habilita o deshabilita el input de Alfa según la señal seleccionada"""
        idx = self.cb_tipo.current()
        # Alfa tiene sentido en Cuasi-cuadrada (2) y RMOC C (4)
        if idx in [2, 4]:
            self.ent_alfa.config(state="normal")
            self.cb_unidad_alfa.config(state="readonly")
        else:
            self.ent_alfa.config(state="disabled")
            self.cb_unidad_alfa.config(state="disabled")

    def procesar_datos(self):
        try:
            # 1. Leer inputs de la interfaz
            idx_senal = self.cb_tipo.current()
            vp = float(self.ent_vp.get())
            alfa_val = float(self.ent_alfa.get()) if self.ent_alfa.instate(['!disabled']) else 0.0
            r_carga = float(self.ent_carga.get())
            x_carga = float(self.ent_reactancia.get())
            armonicas = int(self.ent_arm.get())
            muestras = int(self.ent_muestras.get())

            if r_carga < 0:
                raise ValueError("La resistencia no puede ser negativa")
            if r_carga == 0 and x_carga == 0:
                raise ValueError("La impedancia total no puede ser 0")
            if muestras <= 0:
                raise ValueError("La cantidad de muestras debe ser mayor a 0")
                
            # Actualizar la resolución (cantidad de muestras) antes de generar las señales
            self.analizador.actualizar_muestras(muestras)

            # Conversión de grados a radianes si la unidad es Grados
            unidad = self.cb_unidad_alfa.get()
            if unidad == "Grados":
                alfa_rad = np.radians(alfa_val)
            else:
                alfa_rad = alfa_val # Ya está en radianes

            # 2. Llamar al modelo para generar señales
            v = self.analizador.generar_senal_v(idx_senal, vp, alfa_rad)
            i = self.analizador.generar_corriente(v, r_carga, x_carga, armonicas)

            # 3. Llamar al modelo para calcular parámetros
            res = self.analizador.analizar_potencia(v, i, armonicas)

            # 4. Actualizar Vista (Textos)
            texto = (f"Valores de Señal:\n"
                     f"Vmax: {res['Vmax']:.1f} V | Vmin: {res['Vmin']:.1f} V\n"
                     f"Vavg: {res['Vavg']:.2f} V | Vrms: {res['Vrms']:.2f} V\n"
                     f"Irms: {res['Irms']:.2f} A\n\n"
                     f"Parámetros de Potencia:\n"
                     f"S (Aparente): {res['S']:.2f} VA\n"
                     f"P (Activa): {res['P']:.2f} W\n"
                     f"Q (Reactiva Fund.): {res['Q']:.2f} VAR\n"
                     f"D (Distorsión): {res['D']:.2f} VAD\n"
                     f"FP: {res['FP']:.3f}\n"
                     f"THD (Tensión): {res['THD']:.2f} %")
            self.lbl_resultados.set(texto)

            # 5. Actualizar Vista (Gráfico)
            self.ax.clear()
            
            # Duplicar señales para mostrar 2 ciclos
            theta_2_ciclos = np.concatenate([self.analizador.theta, self.analizador.theta + 2 * np.pi])
            v_2_ciclos = np.concatenate([v, v])
            i_2_ciclos = np.concatenate([i, i])
            
            factor_escala = vp/np.max(i_2_ciclos)*0.8 if np.max(i_2_ciclos) > 0 else 1
            
            self.ax.plot(theta_2_ciclos, v_2_ciclos, label='Voltaje (V)', color='#2980b9', linewidth=2)
            self.ax.plot(theta_2_ciclos, i_2_ciclos * factor_escala, 
                         label='Corriente (Escalada)', color='#e67e22', linestyle='--', linewidth=2)
            
            # Dibujar líneas de alfa si corresponde para los 2 ciclos
            if idx_senal in [2, 4]:
                self.ax.axvline(x=alfa_rad, color='#e74c3c', linestyle=':', label='Corte Alfa')
                self.ax.axvline(x=np.pi + alfa_rad, color='#e74c3c', linestyle=':')
                self.ax.axvline(x=alfa_rad + 2*np.pi, color='#e74c3c', linestyle=':')
                self.ax.axvline(x=np.pi + alfa_rad + 2*np.pi, color='#e74c3c', linestyle=':')

            self.ax.set_title("Reconstrucción de Ondas (2 Ciclos)", fontsize=12, fontweight='bold', color='#2c3e50')
            self.ax.set_xlabel("Ángulo (radianes)", fontsize=10)
            self.ax.set_ylabel("Amplitud", fontsize=10)
            self.ax.legend(loc="upper right", frameon=True, facecolor='white', framealpha=0.9)
            
            # Ajustar ticks del eje X a múltiplos de pi
            ticks = [0, np.pi, 2*np.pi, 3*np.pi, 4*np.pi]
            labels = ['0', 'π', '2π', '3π', '4π']
            self.ax.set_xticks(ticks)
            self.ax.set_xticklabels(labels)
            
            self.ax.grid(True, linestyle='--', alpha=0.7)
            self.canvas.draw()

        except ValueError as e:
            messagebox.showerror("Error de Entrada", f"Por favor verifica que todos los campos contengan números válidos.\nDetalle: {e}")
