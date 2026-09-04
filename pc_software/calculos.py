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
            senal[(self.theta >= np.pi + alfa_rad) & (self.theta <= 2 * np.pi - alfa_rad)] = -Vp
            return senal
            
        elif tipo_idx == 3: # RMMO (Monofásico Media Onda)
            return np.where(self.theta < np.pi, Vp * np.sin(self.theta), 0.0)
            
        elif tipo_idx == 4: # RMMOC (Media onda, 0 hasta alfa, senoidal hasta pi)
            senal = np.zeros(self.N)
            mask = (self.theta >= alfa_rad) & (self.theta < np.pi)
            senal[mask] = Vp * np.sin(self.theta[mask])
            return senal
            
        elif tipo_idx == 5: # RMOC (Onda completa sin control, valor absoluto)
            return np.abs(Vp * np.sin(self.theta))
            
        elif tipo_idx == 6: # RMOCC (Onda completa controlado)
            senal = np.zeros(self.N)
            mask_pos = (self.theta >= alfa_rad) & (self.theta < np.pi)
            mask_neg = (self.theta >= np.pi + alfa_rad) & (self.theta < 2 * np.pi)
            senal[mask_pos] = Vp * np.sin(self.theta[mask_pos])
            senal[mask_neg] = -Vp * np.sin(self.theta[mask_neg])
            return senal
            
        elif tipo_idx == 7: # RTMO (Trifásico Media Onda. Conmutación natural en pi/6)
            va = Vp * np.sin(self.theta)
            vb = Vp * np.sin(self.theta - 2 * np.pi / 3)
            vc = Vp * np.sin(self.theta + 2 * np.pi / 3)
            return np.maximum(va, np.maximum(vb, vc))
            
        elif tipo_idx == 8: # RTMOC (Trifásico Media Onda Controlado. Retraso alfa desde pi/6)
            va = Vp * np.sin(self.theta)
            vb = Vp * np.sin(self.theta - 2 * np.pi / 3)
            vc = Vp * np.sin(self.theta + 2 * np.pi / 3)
            t_shift = (self.theta - np.pi / 6 - alfa_rad) % (2 * np.pi)
            senal = np.zeros(self.N)
            s0 = (t_shift >= 0) & (t_shift < 2 * np.pi / 3)
            s1 = (t_shift >= 2 * np.pi / 3) & (t_shift < 4 * np.pi / 3)
            s2 = (t_shift >= 4 * np.pi / 3) & (t_shift < 2 * np.pi)
            senal[s0] = va[s0]
            senal[s1] = vb[s1]
            senal[s2] = vc[s2]
            return np.maximum(senal, 0.0)
            
        elif tipo_idx == 9: # RTOC (Trifásico Onda Completa / Puente de 6 pulsos)
            va = Vp * np.sin(self.theta)
            vb = Vp * np.sin(self.theta - 2 * np.pi / 3)
            vc = Vp * np.sin(self.theta + 2 * np.pi / 3)
            vab = va - vb
            vac = va - vc
            vbc = vb - vc
            vba = vb - va
            vca = vc - va
            vcb = vc - vb
            return np.maximum.reduce([vab, vac, vbc, vba, vca, vcb])
            
        elif tipo_idx == 10: # RTOCC (Trifásico Onda Completa Controlado. Retraso alfa desde pi/3)
            va = Vp * np.sin(self.theta)
            vb = Vp * np.sin(self.theta - 2 * np.pi / 3)
            vc = Vp * np.sin(self.theta + 2 * np.pi / 3)
            vab = va - vb
            vac = va - vc
            vbc = vb - vc
            vba = vb - va
            vca = vc - va
            vcb = vc - vb
            t_shift = (self.theta - np.pi / 6 - alfa_rad) % (2 * np.pi)
            senal = np.zeros(self.N)
            s0 = (t_shift >= 0) & (t_shift < np.pi / 3)
            s1 = (t_shift >= np.pi / 3) & (t_shift < 2 * np.pi / 3)
            s2 = (t_shift >= 2 * np.pi / 3) & (t_shift < np.pi)
            s3 = (t_shift >= np.pi) & (t_shift < 4 * np.pi / 3)
            s4 = (t_shift >= 4 * np.pi / 3) & (t_shift < 5 * np.pi / 3)
            s5 = (t_shift >= 5 * np.pi / 3) & (t_shift < 2 * np.pi)
            senal[s0] = vab[s0]
            senal[s1] = vac[s1]
            senal[s2] = vbc[s2]
            senal[s3] = vba[s3]
            senal[s4] = vca[s4]
            senal[s5] = vcb[s5]
            return np.maximum(senal, 0.0)
            
        elif tipo_idx == 11: # Muestras ADC (Simuladas) - Obligatoriamente la última opción
            return Vp * np.sin(self.theta) + np.random.normal(0, Vp * 0.1, self.N)
            
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
        
        i_max, i_min = float(np.max(i)), float(np.min(i))
        i_avg = float(np.mean(i))
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

        i1_rms = i_harmonics[0][0]
        if i1_rms > 0.01: # Protección ante división por cero / ruido para corriente
            suma_cuadrados_i = sum(h[0]**2 for h in i_harmonics[1:])
            thd_i = (float(np.sqrt(suma_cuadrados_i)) / i1_rms) * 100
        else:
            thd_i = 0.0

        S = v_rms * i_rms
        P = float(np.mean(v * i))
        FP = P / S if S > 0 else 0.0
        
        Q1 = v1_rms * i_harmonics[0][0] * np.sin(v_harmonics[0][1] - i_harmonics[0][1])
        D = float(np.sqrt(max(0, S**2 - P**2 - Q1**2)))

        espectro_v = np.array([h[0] for h in v_harmonics])
        espectro_i = np.array([h[0] for h in i_harmonics])

        return {
            "Vmax": v_max, "Vmin": v_min, "Vavg": v_avg, "Vrms": v_rms,
            "Imax": i_max, "Imin": i_min, "Iavg": i_avg, "Irms": i_rms,
            "S": S, "P": P, "Q": Q1, "D": D, "FP": FP,
            "THD": thd_v, "THD_I": thd_i,
            "espectro_v": espectro_v, "espectro_i": espectro_i
        }
