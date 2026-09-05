import socket
import ipaddress
import tkinter as tk
from tkinter import ttk, messagebox
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
try:
    from calculos import AnalizadorDePotencia
except ImportError:
    from pc_software.calculos import AnalizadorDePotencia

# ==========================================
# MÓDULO 2: INTERFAZ GRÁFICA (La "Vista")
# ==========================================
class InterfazGrafica:
    def __init__(self, root):
        self.root = root
        self.root.title("Analizador de Potencia - Electrónica de Potencia")
        self.root.geometry("1280x760")
        self.root.minsize(1100, 680)
        self.root.configure(bg="#f4f6f9")
        
        # Estilos modernos con fuentes más grandes para máxima legibilidad
        style = ttk.Style()
        if "clam" in style.theme_names():
            style.theme_use("clam")
        style.configure("TLabel", background="#f4f6f9", font=("Segoe UI", 11), foreground="#333333")
        style.configure("TLabelframe", background="#f4f6f9", bordercolor="#d1d5db")
        style.configure("TLabelframe.Label", background="#f4f6f9", font=("Segoe UI", 12, "bold"), foreground="#2c3e50")
        style.configure("TButton", font=("Segoe UI", 11, "bold"), padding=5, background="#3498db", foreground="white")
        style.map("TButton", background=[("active", "#2980b9")])
        style.configure("TFrame", background="#f4f6f9")
        style.configure("TCheckbutton", background="#f4f6f9", font=("Segoe UI", 11))
        
        self.root.option_add("*TCombobox*Listbox.font", ("Segoe UI", 11))
        
        # Referencia para anotación/cursor flotante al hacer clic
        self.anotacion = None
        self.timer_id = None            # Identificador del temporizador after para tiempo real
        
        # Instanciar el modelo matemático
        self.analizador = AnalizadorDePotencia()
        
        # Configuración de red para comunicación UDP con el ESP32
        self.esp32_ip = "192.168.1.50"  # IP por defecto configurable desde la interfaz
        self.esp32_port = 8888          # Puerto UDP local del ESP32
        
        # Opciones de señales (12 opciones según arquitectura y Modelo)
        self.tipos_senal = [
            "0. Senoidal",
            "1. Cuadrada",
            "2. Cuasi-cuadrada",
            "3. RMMO",
            "4. RMMOC",
            "5. RMOC",
            "6. RMOCC",
            "7. RTMO",
            "8. RTMOC",
            "9. RTOC",
            "10. RTOCC",
            "11. Muestras ADC (Simuladas)"
        ]
        
        self.crear_widgets()

    def crear_widgets(self):
        # --- Panel Izquierdo: Controles ---
        frame_izq = ttk.Frame(self.root)
        frame_izq.pack(side=tk.LEFT, fill=tk.Y, padx=12, pady=10)

        frame_controles = ttk.LabelFrame(frame_izq, text="Parámetros de Entrada", padding="8")
        frame_controles.pack(fill=tk.X, pady=(0, 6))

        ttk.Label(frame_controles, text="Tipo de Señal:").grid(row=0, column=0, sticky="w", pady=2)
        self.cb_tipo = ttk.Combobox(frame_controles, values=self.tipos_senal, state="readonly", width=23, font=("Segoe UI", 11))
        self.cb_tipo.current(6) # Por defecto RMOCC
        self.cb_tipo.grid(row=0, column=1, columnspan=3, sticky="w", pady=2)
        self.cb_tipo.bind("<<ComboboxSelected>>", self.actualizar_estado_alfa)

        ttk.Label(frame_controles, text="Voltaje Pico (Vp):").grid(row=1, column=0, sticky="w", pady=2)
        self.ent_vp = ttk.Entry(frame_controles, width=8, font=("Segoe UI", 11))
        self.ent_vp.insert(0, "311")
        self.ent_vp.grid(row=1, column=1, sticky="w", pady=2)

        ttk.Label(frame_controles, text="Ángulo de Disparo α:").grid(row=2, column=0, sticky="w", pady=2)
        frame_alfa = ttk.Frame(frame_controles)
        frame_alfa.grid(row=2, column=1, columnspan=3, sticky="w", pady=2)
        self.ent_alfa = ttk.Entry(frame_alfa, width=8, font=("Segoe UI", 11))
        self.ent_alfa.insert(0, "60")
        self.ent_alfa.pack(side=tk.LEFT)
        self.cb_unidad_alfa = ttk.Combobox(frame_alfa, values=["Grados", "Radianes"], state="readonly", width=9, font=("Segoe UI", 11))
        self.cb_unidad_alfa.current(0)
        self.cb_unidad_alfa.pack(side=tk.LEFT, padx=4)

        # R y X organizados en la misma fila para optimizar altura con fuentes más grandes
        ttk.Label(frame_controles, text="Resistencia R (Ω):").grid(row=3, column=0, sticky="w", pady=2)
        self.ent_carga = ttk.Entry(frame_controles, width=8, font=("Segoe UI", 11))
        self.ent_carga.insert(0, "10")
        self.ent_carga.grid(row=3, column=1, sticky="w", pady=2)
        
        ttk.Label(frame_controles, text="Reactancia X (Ω):").grid(row=3, column=2, sticky="w", padx=(8, 2), pady=2)
        self.ent_reactancia = ttk.Entry(frame_controles, width=8, font=("Segoe UI", 11))
        self.ent_reactancia.insert(0, "0")
        self.ent_reactancia.grid(row=3, column=3, sticky="w", pady=2)

        # Cant. de Armónicas y Muestras por Ciclo organizados en la misma fila
        ttk.Label(frame_controles, text="Cant. Armónicas:").grid(row=4, column=0, sticky="w", pady=2)
        self.ent_arm = ttk.Entry(frame_controles, width=8, font=("Segoe UI", 11))
        self.ent_arm.insert(0, "15")
        self.ent_arm.grid(row=4, column=1, sticky="w", pady=2)

        ttk.Label(frame_controles, text="Muestras / Ciclo:").grid(row=4, column=2, sticky="w", padx=(8, 2), pady=2)
        self.ent_muestras = ttk.Entry(frame_controles, width=8, font=("Segoe UI", 11))
        self.ent_muestras.insert(0, "256")
        self.ent_muestras.grid(row=4, column=3, sticky="w", pady=2)

        btn_calcular = ttk.Button(frame_controles, text="Calcular y Graficar", command=self.procesar_datos)
        btn_calcular.grid(row=5, column=0, columnspan=4, pady=(6, 2))

        # --- Controles de Tiempo Real ---
        frame_tiempo_real = ttk.LabelFrame(frame_izq, text="Actualización en Tiempo Real", padding="6")
        frame_tiempo_real.pack(fill=tk.X, pady=(0, 6))
        
        self.var_tiempo_real = tk.BooleanVar()
        chk_tiempo_real = ttk.Checkbutton(frame_tiempo_real, text="Activar", variable=self.var_tiempo_real, command=self.toggle_tiempo_real)
        chk_tiempo_real.grid(row=0, column=0, sticky="w", pady=2)
        
        ttk.Label(frame_tiempo_real, text="Refresco (seg):").grid(row=0, column=1, sticky="w", padx=(8, 4), pady=2)
        self.ent_refresco = ttk.Entry(frame_tiempo_real, width=5, font=("Segoe UI", 11))
        self.ent_refresco.insert(0, "1.0")
        self.ent_refresco.config(state="disabled")
        self.ent_refresco.grid(row=0, column=2, sticky="w", pady=2)

        # --- Control de Hardware (ESP32) ---
        frame_hw = ttk.LabelFrame(frame_izq, text="Control de Hardware (ESP32)", padding="6")
        frame_hw.pack(fill=tk.X, pady=(0, 6))

        frame_ip = ttk.Frame(frame_hw)
        frame_ip.pack(fill=tk.X, pady=(0, 3))
        ttk.Label(frame_ip, text="IP ESP32:").pack(side=tk.LEFT)
        self.ent_esp32_ip = ttk.Entry(frame_ip, width=15, font=("Segoe UI", 10))
        self.ent_esp32_ip.insert(0, self.esp32_ip)
        self.ent_esp32_ip.pack(side=tk.LEFT, padx=(4, 0), fill=tk.X, expand=True)

        self.lbl_estado_hw = ttk.Label(frame_hw, text="Estado: Desconectado", font=("Segoe UI", 10, "bold"))
        self.lbl_estado_hw.pack(anchor="w", pady=(0, 3))

        self.btn_enviar_hw = ttk.Button(
            frame_hw,
            text="Enviar Ángulo (α) al Hardware",
            command=self.enviar_angulo_hardware,
            padding=4
        )
        self.btn_enviar_hw.pack(fill=tk.X)

        # --- Resultados en Texto ---
        frame_resultados = ttk.LabelFrame(frame_izq, text="Resultados", padding="8")
        frame_resultados.pack(fill=tk.BOTH, expand=True)

        self.lbl_resultados = tk.StringVar()
        self.lbl_resultados.set("Esperando cálculo...")
        lbl = ttk.Label(frame_resultados, textvariable=self.lbl_resultados, justify=tk.LEFT, font=("Consolas", 11))
        lbl.pack(anchor="nw", fill=tk.BOTH, expand=True)

        # --- Panel Derecho: Gráficos (Matplotlib) con 2 Subplots ---
        self.frame_grafico = ttk.Frame(self.root)
        self.frame_grafico.pack(side=tk.RIGHT, fill=tk.BOTH, expand=True, padx=12, pady=10)
        
        try:
            plt.style.use('ggplot')
        except:
            pass
            
        self.fig, (self.ax1, self.ax2) = plt.subplots(2, 1, figsize=(8, 6), dpi=100)
        self.fig.patch.set_facecolor('#f4f6f9')
        # Configuración fija de márgenes para máxima estabilidad y evitar parpadeos en tiempo real
        self.fig.subplots_adjust(left=0.09, right=0.96, top=0.93, bottom=0.09, hspace=0.38)
        self.canvas = FigureCanvasTkAgg(self.fig, master=self.frame_grafico)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)
        
        # Conectar evento de clic del mouse para cursor interactivo
        self.canvas.mpl_connect('button_press_event', self.al_hacer_click)
        
        # Inicializar estado del campo Alfa según la señal por defecto
        self.actualizar_estado_alfa()

        # Cálculo inicial automático
        self.procesar_datos()

    def toggle_tiempo_real(self):
        if self.var_tiempo_real.get():
            self.ent_refresco.config(state="normal")
            self.bucle_actualizacion()
        else:
            self.ent_refresco.config(state="disabled")
            if self.timer_id is not None:
                self.root.after_cancel(self.timer_id)
                self.timer_id = None

    def bucle_actualizacion(self):
        if self.var_tiempo_real.get():
            try:
                segundos = float(self.ent_refresco.get())
                if segundos <= 0:
                    raise ValueError("El tiempo debe ser mayor a 0")
                ms = int(segundos * 1000)
                
                self.procesar_datos()
                self.timer_id = self.root.after(ms, self.bucle_actualizacion)
            except ValueError:
                self.var_tiempo_real.set(False)
                self.ent_refresco.config(state="disabled")
                if self.timer_id is not None:
                    self.root.after_cancel(self.timer_id)
                    self.timer_id = None
                messagebox.showwarning("Error de Formato", "Por favor ingresa un número numérico válido para los segundos de refresco.")

    def actualizar_estado_alfa(self, event=None):
        """Habilita o deshabilita el input de Alfa según la señal seleccionada"""
        idx = self.cb_tipo.current()
        # Alfa aplica en Cuasi-cuadrada (2), RMMOC (4), RMOCC (6), RTMOC (8), RTOCC (10)
        # y Muestras ADC (11) únicamente para envío al hardware (sin injerencia en el gráfico)
        if idx in [2, 4, 6, 8, 10, 11]:
            self.ent_alfa.config(state="normal")
            self.cb_unidad_alfa.config(state="readonly")
        else:
            self.ent_alfa.config(state="disabled")
            self.cb_unidad_alfa.config(state="disabled")

    def enviar_angulo_hardware(self):
        """Lee el ángulo alfa de entrada, valida que sea un float seguro entre 0° y 180°,
        transmite el comando vía UDP al microcontrolador ESP32 y verifica la conexión
        aguardando activamente un acuse de recibo (ACK)."""
        try:
            # 0. Leer y validar sintaxis de la dirección IP
            ip_ingresada = self.ent_esp32_ip.get().strip() if hasattr(self, 'ent_esp32_ip') else self.esp32_ip
            if not ip_ingresada:
                raise ValueError("Debe ingresar una dirección IP válida para el ESP32.")
            try:
                ipaddress.ip_address(ip_ingresada)
            except ValueError:
                raise ValueError(f"'{ip_ingresada}' no es una dirección IP válida (ej. 192.168.1.50).")
            self.esp32_ip = ip_ingresada

            # 1. Leer el valor actual del campo self.ent_alfa
            valor_raw = self.ent_alfa.get().strip()
            angulo = float(valor_raw)

            # Validar que no sea NaN o Infinito
            if np.isnan(angulo) or np.isinf(angulo):
                raise ValueError("El valor ingresado no es un número finito válido.")

            # Conversión auxiliar si la unidad seleccionada es Radianes
            unidad = self.cb_unidad_alfa.get() if hasattr(self, "cb_unidad_alfa") else "Grados"
            if unidad == "Radianes":
                alfa_deg = float(np.degrees(angulo))
            else:
                alfa_deg = angulo

            # Validar rango físico posible para el disparo (entre 0 y 180 grados)
            if not (0.0 <= alfa_deg <= 180.0):
                raise ValueError(
                    f"El ángulo ({alfa_deg:.2f}°) está fuera del rango físico permitido (0° a 180°)."
                )

            # Generar mensaje en texto plano
            mensaje = f"ALFA:{alfa_deg:.1f}"

            # Feedback visual de envío en progreso
            self.lbl_estado_hw.config(
                text=f"Estado: Verificando conexión con {self.esp32_ip}:{self.esp32_port}...",
                foreground="#2980b9"
            )
            self.root.update_idletasks()

            # Transmisión y verificación bidireccional vía socket UDP
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.settimeout(1.2) # Timeout de 1.2 segundos para esperar el ACK del ESP32
            try:
                sock.sendto(mensaje.encode('utf-8'), (self.esp32_ip, self.esp32_port))

                # Espera obligatoria del paquete de confirmación (ACK) emitido por el ESP32
                data, addr = sock.recvfrom(1024)
                respuesta = data.decode('utf-8').strip()

                if respuesta.startswith("ACK:ALFA:"):
                    self.lbl_estado_hw.config(
                        text=f"Estado: Conectado (ESP32 confirmó {respuesta})",
                        foreground="#27ae60"
                    )
                elif respuesta.startswith("NACK:"):
                    self.lbl_estado_hw.config(
                        text=f"Estado: Rechazado por ESP32 ({respuesta})",
                        foreground="#e67e22"
                    )
                    messagebox.showwarning(
                        "Comando Rechazado por Hardware",
                        f"El ESP32 en {addr[0]} recibió el paquete pero rechazó el comando:\n{respuesta}"
                    )
                else:
                    self.lbl_estado_hw.config(
                        text=f"Estado: Respuesta desconocida ({respuesta})",
                        foreground="#e67e22"
                    )
            except (socket.timeout, ConnectionResetError):
                self.lbl_estado_hw.config(
                    text=f"Estado: Sin respuesta (Timeout / Inaccesible en {self.esp32_ip}:{self.esp32_port})",
                    foreground="#c0392b"
                )
                messagebox.showerror(
                    "Error de Conexión con Hardware",
                    f"No se recibió confirmación (ACK) del ESP32 en {self.esp32_ip}:{self.esp32_port}.\n\n"
                    f"El paquete UDP no pudo ser verificado. Asegúrese de que:\n"
                    f"1. La dirección IP ({self.esp32_ip}) pertenezca efectivamente al ESP32.\n"
                    f"2. El ESP32 se encuentre encendido y conectado a la misma red Wi-Fi.\n"
                    f"3. No existan bloqueos de firewall para tráfico UDP en el puerto {self.esp32_port}."
                )
            finally:
                sock.close()

        except ValueError as e:
            self.lbl_estado_hw.config(
                text="Estado: Parámetros inválidos",
                foreground="#c0392b"
            )
            messagebox.showerror(
                "Error de Parámetros de Hardware",
                f"El valor de ángulo (α) o la dirección IP ingresada no son válidos.\n\n"
                f"Detalle: {e}"
            )
        except (socket.error, OSError) as e:
            self.lbl_estado_hw.config(
                text=f"Estado: Error de red ({self.esp32_ip}:{self.esp32_port})",
                foreground="#c0392b"
            )
            messagebox.showerror(
                "Error de Socket UDP",
                f"Fallo de socket de red al intentar comunicar con {self.esp32_ip}:{self.esp32_port}.\n\n"
                f"Detalle: {e}"
            )

    def procesar_datos(self):
        try:
            # 1. Leer inputs de la interfaz
            idx_senal = self.cb_tipo.current()
            vp = float(self.ent_vp.get())
            # Para Muestras ADC (11), el ángulo no tiene injerencia en el cálculo matemático ni gráfico
            if idx_senal in [2, 4, 6, 8, 10]:
                alfa_val = float(self.ent_alfa.get()) if self.ent_alfa.instate(['!disabled']) else 0.0
            else:
                alfa_val = 0.0
            r_carga = float(self.ent_carga.get())
            x_carga = float(self.ent_reactancia.get())
            armonicas = int(self.ent_arm.get())
            muestras = int(self.ent_muestras.get())

            if r_carga < 0:
                raise ValueError("La resistencia no puede ser negativa")
            if r_carga == 0 and x_carga == 0:
                raise ValueError("La impedancia total no puede ser 0")
            if armonicas <= 0:
                raise ValueError("La cantidad de armónicas debe ser mayor a 0")
            if muestras <= 0:
                raise ValueError("La cantidad de muestras debe ser mayor a 0")
            if armonicas >= muestras / 2:
                raise ValueError(
                    f"Por el teorema de Nyquist, la cantidad de armónicas ({armonicas}) "
                    f"debe ser estrictamente menor a la mitad de las muestras por ciclo ({muestras // 2})."
                )
                
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

            # 4. Actualizar Vista (Textos) con Imax, Imin, Iavg, Irms y THD_I
            texto = (f"Valores de Señal:\n"
                     f"Vmax: {res['Vmax']:.1f} V | Vmin: {res['Vmin']:.1f} V\n"
                     f"Vavg: {res['Vavg']:.2f} V | Vrms: {res['Vrms']:.2f} V\n"
                     f"Imax: {res['Imax']:.2f} A | Imin: {res['Imin']:.2f} A\n"
                     f"Iavg: {res['Iavg']:.2f} A | Irms: {res['Irms']:.2f} A\n\n"
                     f"Parámetros de Potencia:\n"
                     f"S (Aparente): {res['S']:.2f} VA\n"
                     f"P (Activa): {res['P']:.2f} W\n"
                     f"Q (Reactiva Fund.): {res['Q']:.2f} VAR\n"
                     f"D (Distorsión): {res['D']:.2f} VAD\n"
                     f"FP: {res['FP']:.3f}\n"
                     f"THD (Tensión): {res['THD']:.2f} %\n"
                     f"THD (Corriente): {res['THD_I']:.2f} %")
            self.lbl_resultados.set(texto)

            # 5. Actualizar Vista: Subplot 1 (Ondas en el tiempo)
            self.ax1.clear()
            self.anotacion = None
            
            # Duplicar señales para mostrar 2 ciclos
            theta_2_ciclos = np.concatenate([self.analizador.theta, self.analizador.theta + 2 * np.pi])
            v_2_ciclos = np.concatenate([v, v])
            i_2_ciclos = np.concatenate([i, i])
            
            max_i = np.max(np.abs(i_2_ciclos))
            factor_escala = (vp / max_i) * 0.8 if max_i > 0 else 1.0
            
            self.ax1.plot(theta_2_ciclos, v_2_ciclos, label='Voltaje (V)', color='#2980b9', linewidth=2)
            self.ax1.plot(theta_2_ciclos, i_2_ciclos * factor_escala, 
                          label=f'Corriente (Escala x{factor_escala:.1f})' if factor_escala != 1 else 'Corriente (A)', 
                          color='#e67e22', linestyle='--', linewidth=2)
            
            # Dibujar líneas de alfa si corresponde para los 2 ciclos
            if idx_senal == 2: # Cuasi-cuadrada
                for k in [0, 2 * np.pi]:
                    self.ax1.axvline(x=alfa_rad + k, color='#e74c3c', linestyle=':', label='Corte Alfa' if k == 0 else "")
                    self.ax1.axvline(x=np.pi - alfa_rad + k, color='#e74c3c', linestyle=':')
                    self.ax1.axvline(x=np.pi + alfa_rad + k, color='#e74c3c', linestyle=':')
                    self.ax1.axvline(x=2 * np.pi - alfa_rad + k, color='#e74c3c', linestyle=':')
            elif idx_senal == 4: # RMMOC
                for k in [0, 2 * np.pi]:
                    self.ax1.axvline(x=alfa_rad + k, color='#e74c3c', linestyle=':', label='Corte Alfa' if k == 0 else "")
            elif idx_senal == 6: # RMOCC
                for k in [0, 2 * np.pi]:
                    self.ax1.axvline(x=alfa_rad + k, color='#e74c3c', linestyle=':', label='Corte Alfa' if k == 0 else "")
                    self.ax1.axvline(x=np.pi + alfa_rad + k, color='#e74c3c', linestyle=':')
            elif idx_senal == 8: # RTMOC
                for k in [0, 2 * np.pi]:
                    self.ax1.axvline(x=np.pi / 6 + alfa_rad + k, color='#e74c3c', linestyle=':', label='Corte Alfa' if k == 0 else "")
                    self.ax1.axvline(x=5 * np.pi / 6 + alfa_rad + k, color='#e74c3c', linestyle=':')
                    self.ax1.axvline(x=3 * np.pi / 2 + alfa_rad + k, color='#e74c3c', linestyle=':')
            elif idx_senal == 10: # RTOCC
                for k in [0, 2 * np.pi]:
                    self.ax1.axvline(x=np.pi / 6 + alfa_rad + k, color='#e74c3c', linestyle=':', label='Corte Alfa' if k == 0 else "")
                    self.ax1.axvline(x=np.pi / 2 + alfa_rad + k, color='#e74c3c', linestyle=':')
                    self.ax1.axvline(x=5 * np.pi / 6 + alfa_rad + k, color='#e74c3c', linestyle=':')
                    self.ax1.axvline(x=7 * np.pi / 6 + alfa_rad + k, color='#e74c3c', linestyle=':')
                    self.ax1.axvline(x=3 * np.pi / 2 + alfa_rad + k, color='#e74c3c', linestyle=':')
                    self.ax1.axvline(x=11 * np.pi / 6 + alfa_rad + k, color='#e74c3c', linestyle=':')

            self.ax1.set_title("Reconstrucción de Ondas en el Tiempo (2 Ciclos)", fontsize=12, fontweight='bold', color='#2c3e50')
            self.ax1.set_xlabel("Ángulo (radianes)", fontsize=10, fontweight='bold')
            self.ax1.set_ylabel("Amplitud", fontsize=10, fontweight='bold')
            self.ax1.tick_params(axis='both', labelsize=9)
            self.ax1.legend(loc="upper right", frameon=True, facecolor='white', framealpha=0.9, fontsize=9)
            
            ticks = [0, np.pi, 2 * np.pi, 3 * np.pi, 4 * np.pi]
            labels = ['0', 'π', '2π', '3π', '4π']
            self.ax1.set_xticks(ticks)
            self.ax1.set_xticklabels(labels)
            self.ax1.grid(True, linestyle='--', alpha=0.7)

            # 6. Actualizar Vista: Subplot 2 (Espectro armónico de Fourier)
            self.ax2.clear()
            
            n_armonicas = np.arange(1, armonicas + 1)
            esp_v = res['espectro_v']
            esp_i = res['espectro_i']
            
            ancho_barra = 0.38
            self.ax2.bar(n_armonicas - ancho_barra / 2, esp_v, width=ancho_barra, 
                         label='Tensión RMS (V)', color='#2980b9', alpha=0.85)
            self.ax2.bar(n_armonicas + ancho_barra / 2, esp_i, width=ancho_barra, 
                         label='Corriente RMS (A)', color='#e67e22', alpha=0.85)
                         
            self.ax2.set_title("Espectro Armónico de Fourier (Componentes RMS)", fontsize=12, fontweight='bold', color='#2c3e50')
            self.ax2.set_xlabel("Número de Armónica (n)", fontsize=10, fontweight='bold')
            self.ax2.set_ylabel("Magnitud RMS", fontsize=10, fontweight='bold')
            self.ax2.tick_params(axis='both', labelsize=9)
            
            if armonicas <= 20:
                self.ax2.set_xticks(n_armonicas)
            else:
                paso = 2 if armonicas <= 40 else 5
                self.ax2.set_xticks(np.arange(1, armonicas + 1, paso))
                
            self.ax2.legend(loc="upper right", frameon=True, facecolor='white', framealpha=0.9, fontsize=9)
            self.ax2.grid(True, linestyle='--', alpha=0.7)
            
            # Márgenes estables ya configurados fijos con subplots_adjust en __init__
            self.canvas.draw()

        except ValueError as e:
            messagebox.showerror("Error de Entrada", f"Por favor verifica que todos los campos contengan números válidos.\nDetalle: {e}")

    def al_hacer_click(self, event):
        """Manejador de evento de clic para cursores interactivos y anotaciones flotantes"""
        if event.inaxes is not None:
            # Eliminar anotación previa si existe
            if self.anotacion is not None:
                try:
                    self.anotacion.remove()
                except Exception:
                    pass
                self.anotacion = None

            # Anotación para el Subplot 1 (Dominio del Tiempo)
            if event.inaxes == self.ax1:
                texto = f"Ángulo: {event.xdata:.2f} rad\nAmplitud: {event.ydata:.2f} V/A"
                self.anotacion = event.inaxes.annotate(
                    texto,
                    xy=(event.xdata, event.ydata),
                    xytext=(15, 15),
                    textcoords="offset points",
                    bbox=dict(boxstyle="round", fc="white", ec="gray", alpha=0.9),
                    arrowprops=dict(arrowstyle="->", connectionstyle="arc3,rad=0", color="#2c3e50"),
                    fontsize=10
                )
            # Anotación para el Subplot 2 (Dominio de la Frecuencia)
            elif event.inaxes == self.ax2:
                texto = f"Armónica: {int(round(event.xdata))}\nMagnitud: {event.ydata:.2f}"
                self.anotacion = event.inaxes.annotate(
                    texto,
                    xy=(event.xdata, event.ydata),
                    xytext=(15, 15),
                    textcoords="offset points",
                    bbox=dict(boxstyle="round", fc="white", ec="gray", alpha=0.9),
                    arrowprops=dict(arrowstyle="->", connectionstyle="arc3,rad=0", color="#2c3e50"),
                    fontsize=10
                )

            self.canvas.draw_idle()
