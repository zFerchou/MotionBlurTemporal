# Motion Blur Temporal Dinámico (DirectX 11)

Mod de posprocesamiento que aplica un **motion blur temporal** a juegos de **DirectX 11** en Windows
(probado con la idea de Minecraft Bedrock). Mezcla cada fotograma con un *history buffer* usando un
factor de mezcla dinámico, y permite simular desde tasas de fotogramas muy altas (suavizado para
monitores de alta frecuencia) hasta tasas bajas de estilo cinematográfico (24/30 FPS).

> Resumen en inglés: a DX11 `Present` hook (MinHook) that applies temporal-accumulation motion blur
> with an adjustable simulated frame rate and intensity.

## Cómo funciona

Consta de dos partes:

| Parte | Lenguaje | Qué hace |
|---|---|---|
| `Injector/` | C# (.NET 8) | Busca el proceso del juego y carga la DLL con `LoadLibraryW` |
| `MotionBlurTemporal/` | C++ (DLL) | Engancha `IDXGISwapChain::Present` con MinHook y aplica el shader |

En cada `Present` la DLL copia el fotograma, lo mezcla con el historial y escribe el resultado en el
backbuffer y en el historial (dos texturas que se alternan).

```
simFps = exp( lerp( ln 500, ln 12, slider ) )   // FPS simulados (slider logarítmico)
tau    = min( 0.5 * shutter / simFps , trailCap ) * intensidad
alpha  = 1 - exp( -dt / tau )                    // peso del fotograma actual
salida = lerp( historial_acotado, actual, alpha )
```

El resultado es independiente del FPS real, porque depende del tiempo `dt` entre fotogramas.

## Requisitos

- Windows 10/11 de 64 bits
- Visual Studio 2022 con la carga de trabajo **Desarrollo de escritorio con C++**
- **.NET 8 SDK**
- Un juego de 64 bits que use **DirectX 11**

## Compilar

**DLL**
1. Abre `MotionBlurTemporal.slnx` en Visual Studio.
2. Selecciona **Release** y **x64**.
3. Compila (`Ctrl + Shift + B`). Resultado: `x64\Release\MotionBlurTemporal.dll`.

**Inyector**
```
cd Injector
dotnet publish -c Release -r win-x64 --self-contained false -o ..\dist
```

## Uso

1. Copia `MotionBlurTemporal.dll` e `Injector.exe` a una carpeta con ruta corta, por ejemplo
   `C:\MotionBlurMod\`. Evita Escritorio, Documentos y carpetas de usuario: los juegos de Microsoft
   Store pueden no tener acceso a ellas.
2. Abre el juego y **entra a un mundo** (no te quedes en el menú).
3. Ejecuta el inyector **desde una terminal**:
   ```
   C:\MotionBlurMod\Injector.exe
   ```
   Opcionalmente: `Injector.exe NombreDelProceso RutaDeLaDll`. Por defecto busca `Minecraft.Windows`.
4. Debe aparecer una barra naranja en la esquina superior izquierda del juego.

## Controles (con la ventana del juego enfocada)

| Tecla | Acción |
|---|---|
| `F6` | Activar / desactivar el efecto |
| `F7` / `F8` (mantener) | FPS simulados (barra azul) |
| `F9` | Presets de FPS simulados: 240, 120, 60, 30, 24 |
| `Av Pág` / `Re Pág` (mantener) | Intensidad del blur de 0% a 200% (barra naranja) |
| `Inicio` | Intensidad de vuelta al 100% |

## Solución de problemas

- Los mensajes de diagnóstico de la DLL se pueden ver con **DebugView** (Sysinternals). Activa
  *Capture Win32* y *Capture Global Win32* y filtra por `MotionBlur`.
- `LoadLibraryW devolvió NULL`: la DLL está en una ruta sin permisos para el juego. Muévela a una
  carpeta corta como `C:\MotionBlurMod\`.
- `El dispositivo NO es D3D11`: el juego está usando DirectX 12; este mod solo funciona con DX11.
- No se puede reemplazar la DLL al recompilar: cierra el juego primero.

## Limitaciones

- Es acumulación temporal sobre la imagen final, sin vectores de movimiento: también afecta al HUD.
- Si el FPS real es menor que el simulado, el efecto se desvanece (no se inventan fotogramas).
- Solo DirectX 11. No se procesan swap chains con MSAA.

## Aviso importante

Este proyecto inyecta una DLL en el proceso de otro programa. Úsalo **solo en modo un jugador o en
servidores propios**: muchos servidores, plataformas y sistemas anti-trampas prohíben la inyección de
DLLs, aunque el efecto sea solo visual, y pueden sancionar tu cuenta. Revisa los términos de uso del
juego. Se ofrece "tal cual", sin garantías, y bajo tu propia responsabilidad.

## Créditos y licencia

- [MinHook](https://github.com/TsudaKageyu/minhook) de Tsuda Kageyu, licencia BSD 2-Clause
  (incluida en `MotionBlurTemporal/MinHook/LICENSE.txt`).
- El código de este repositorio se publica bajo la licencia indicada en el archivo `LICENSE`.
