import numpy as np
from typing import Dict

# ==========================================
# MÓDULO 1: CÁLCULOS (El "Cerebro" / Modelo)
# ==========================================
class AnalizadorDePotencia:
    def __init__(self, muestras_por_ciclo: int = 256) -> None:
        self.actualizar_muestras(muestras_por_ciclo)

    def actualizar_muestras(self, muestras_por_ciclo: int) -> None:
        self.N: int = muestras_por_ciclo
        self.theta: np.ndarray = np.linspace(0, 2 * np.pi, self.N, endpoint=False)

    def generar_senal_v(self, tipo_idx: int, Vp: float, alfa_rad: float) -> np.ndarray:
        """Genera el vector de tensión según el índice seleccionado en la UI"""
        if tipo_idx == 0:   # Senoidal
            return Vp * np.sin(self.theta)
        
        elif tipo_idx == 1: # Cuadrada
            # Generación lógica para evitar el error de punto flotante de sin(pi)
            return np.where(self.theta < np.pi, Vp, -Vp)
        
        elif tipo_idx == 2: # Cuasi-cuadrada
            senal = np.zeros(self.N)
            # Pulso positivo
            senal[(self.theta >= alfa_rad) & (self.theta <= np.pi - alfa_rad)] = Vp
            # Pulso negativo
            senal[(self.theta >= np.pi + alfa_rad) & (self.theta <= 2*np.pi - alfa_rad)] = -Vp
            return senal
            
        elif tipo_idx == 3: # RMMO
            senal = Vp * np.sin(self.theta)
            senal[self.theta >= np.pi] = 0
            return senal
            
        elif tipo_idx == 4: # RMOC C (Controlado)
            senal = np.zeros(self.N)
            senal[(self.theta >= alfa_rad) & (self.theta < np.pi)] = Vp * np.sin(self.theta[(self.theta >= alfa_rad) & (self.theta < np.pi)])
            senal[(self.theta >= np.pi + alfa_rad) & (self.theta < 2*np.pi)] = -Vp * np.sin(self.theta[(self.theta >= np.pi + alfa_rad) & (self.theta < 2*np.pi)])
            return senal
            
        elif tipo_idx == 5: # ADC (Simulación de ruido para ver algo en pantalla)
            return Vp * np.sin(self.theta) + np.random.normal(0, Vp*0.1, self.N)
            
        return np.zeros(self.N)

    def generar_corriente(self, v, R, X, num_armonicas):
        if R == 0 and X == 0:
            return np.zeros_like(v)
            
        # Caso 1: Carga puramente resistiva (Cálculo exacto en el dominio del tiempo)
        if X == 0:
            return v / R
            
        # Caso 2: Carga compleja (Reconstrucción armónica por correlación)
        a0_v = (1.0 / self.N) * np.sum(v)
        a0_i = a0_v / R if R != 0 else 0.0
        
        i_tiempo = np.full(self.N, a0_i)
        
        for n in range(1, int(num_armonicas) + 1):
            an_v = (2.0 / self.N) * np.sum(v * np.cos(n * self.theta))
            bn_v = (2.0 / self.N) * np.sum(v * np.sin(n * self.theta))
            
            if X >= 0:
                Xn = n * X
            else:
                Xn = X / n
                
            Z2 = R**2 + Xn**2
            if Z2 == 0: Z2 = 1e-6
                
            an_i = (R * an_v - Xn * bn_v) / Z2
            bn_i = (R * bn_v + Xn * an_v) / Z2
            
            i_tiempo += an_i * np.cos(n * self.theta) + bn_i * np.sin(n * self.theta)
            
        return i_tiempo

    def analizar_potencia(self, v: np.ndarray, i: np.ndarray, num_armonicas: int) -> dict:
        """Cálculo por correlación de Fourier y potencias IEEE 1459"""
        v_max, v_min = float(np.max(v)), float(np.min(v))
        v_avg = float(np.mean(v))
        v_rms = float(np.sqrt(np.mean(v**2)))
        i_rms = float(np.sqrt(np.mean(i**2)))
        
        v_harmonics = []
        i_harmonics = []
        
        for n in range(1, int(num_armonicas) + 1):
            an_v = (2.0 / self.N) * np.sum(v * np.cos(n * self.theta))
            bn_v = (2.0 / self.N) * np.sum(v * np.sin(n * self.theta))
            vn_rms = np.sqrt(an_v**2 + bn_v**2) / np.sqrt(2)
            fase_v = np.arctan2(an_v, bn_v) 
            v_harmonics.append((vn_rms, fase_v))
            
            an_i = (2.0 / self.N) * np.sum(i * np.cos(n * self.theta))
            bn_i = (2.0 / self.N) * np.sum(i * np.sin(n * self.theta))
            in_rms = np.sqrt(an_i**2 + bn_i**2) / np.sqrt(2)
            fase_i = np.arctan2(an_i, bn_i)
            i_harmonics.append((in_rms, fase_i))

        v1_rms = v_harmonics[0][0]
        if v1_rms > 0.1: # Umbral para discriminar ruido de punto flotante
            suma_cuadrados = sum(h[0]**2 for h in v_harmonics[1:])
            thd_v = (float(np.sqrt(suma_cuadrados)) / v1_rms) * 100
        else:
            thd_v = 0.0 # Omitir THD si no hay componente fundamental significativa

        S = v_rms * i_rms
        P = float(np.mean(v * i))
        FP = P / S if S > 0 else 0.0
        
        Q1 = v1_rms * i_harmonics[0][0] * np.sin(v_harmonics[0][1] - i_harmonics[0][1])
        D = float(np.sqrt(max(0, S**2 - P**2 - Q1**2)))

        return {
            "Vmax": v_max, "Vmin": v_min, "Vavg": v_avg, "Vrms": v_rms, "Irms": i_rms,
            "S": S, "P": P, "Q": Q1, "D": D, "FP": FP, "THD": thd_v
        }
