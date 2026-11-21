# Blossom

Blossom es un emulador **LLE** (low-level) de Nintendo 3DS para PC x86-64. En vez de
reimplementar el sistema operativo como hacen los emuladores HLE, emula el hardware
(ARM9, ARM11 de 2 o 4 núcleos, DSP, GPU PICA200, crypto, eMMC, WiFi…) y **arranca el
firmware real** de la consola a partir de tus propios dumps.

El núcleo viene de [Corgi3DS](https://github.com/PSI-Rockin/Corgi3DS), modernizado y
reorganizado siguiendo la estructura de [Azahar](https://github.com/azahar-emu/azahar).

> Estado: hito 1 en curso. Compila con C++20, CMake y Qt 6. El objetivo de este hito es
> arrancar GodMode9 igual que el Corgi3DS original. Los núcleos ARM ya van por JIT
> ([dynarmic](https://github.com/azahar-emu/dynarmic)), con el intérprete de Corgi3DS como alternativa.

## Estructura

```
src/
  common/      utilidades compartidas (excepciones, bswap, rotaciones)
  core/        núcleo LLE: cpu/ (ARM + VFP), arm9/ (AES, RSA, SHA, NDMA, eMMC, cartucho),
               arm11/ (GPU, DSP, MPCore, WiFi/Xtensa), scheduler, timers, PXI, I2C, SPI
  blossom_qt/  frontend en Qt 6
  tests/       runner de tests de CPU (armwrestler, thumbwrestler, lletest)
tools/
  mknand/      genera una NAND sintética de Old 3DS a partir de tu boot9 y el NATIVE_FIRM
externals/    dynarmic y sus dependencias
CMakeModules/  módulos de CMake (FindGMP)
```

## Compilar (Linux)

Dependencias en Ubuntu 24.04 o Debian 12+:

```sh
sudo apt install cmake ninja-build g++ qt6-base-dev libgl-dev libgmp-dev libboost-dev
```

Boost solo hace falta por las cabeceras (lo usa dynarmic). En Arch es `boost` y en MSYS2 UCRT64
`mingw-w64-ucrt-x86_64-boost`.

Compilar y pasar los tests:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

El ejecutable queda en `build/src/blossom_qt/blossom`.

Opciones de CMake:

| Opción          | Por defecto | Qué hace                                  |
|-----------------|-------------|-------------------------------------------|
| `ENABLE_QT`     | `ON`        | Compila el frontend Qt 6                  |
| `ENABLE_TESTS`  | `ON`        | Compila el runner de CPU y registra CTest |
| `ENABLE_WERROR` | `OFF`       | Trata los warnings como errores           |
| `ENABLE_JIT`    | `ON` (x86-64) | Compila el JIT dynarmic para ARM9 y ARM11 |

Windows y macOS llegarán más adelante.

## Uso

Necesitas archivos sacados de **tu propia consola**. Blossom no incluye ni descarga
ninguno:

* Boot ROMs `boot9.bin` y `boot11.bin`
* Imagen NAND cifrada sacada con GodMode9 (tiene que incluir `essential.exefs`)
* Opcional: imagen de SD (necesaria con NAND con sighax/B9S, por ejemplo para cargar GodMode9)
* Opcional: dump cifrado de cartucho (`.3ds`)

Haz copia de seguridad de la NAND y de la SD antes de usarlas, porque el emulador escribe en ellas.

Puedes indicarlos desde *Options → Settings* o por línea de comandos:

```sh
./blossom --boot9 boot9.bin --boot11 boot11.bin --nand nand.bin --sd sd.img --autoload-nocart
```

`./blossom --help` muestra todas las opciones.

### CPU: JIT o intérprete

Por defecto los dos tipos de núcleo (ARM11 y ARM9) van por el JIT. Se puede cambiar en
*Options → Settings → CPU*, con `--cpu` o con la variable `BLOSSOM_CPU` (que manda sobre lo demás):

| Valor    | ARM11      | ARM9       |
|----------|------------|------------|
| `jit`    | JIT        | JIT        |
| `jit11`  | JIT        | intérprete |
| `interp` | intérprete | intérprete |

Si algo arranca con el intérprete y no con el JIT, prueba `jit11` y luego `interp` para acotarlo.

Variables de diagnóstico:

* `BLOSSOM_PROFILE=1` imprime en stderr, por frame, el tiempo que se lleva cada parte
  (ARM11, ARM9, GPU, DSP…) y contadores del JIT.
* `BLOSSOM_TRACE_SVC=1` vuelve a sacar la traza de SVC, IPC e interrupciones por stdout.
  Viene apagada porque escribir en consola en cada SVC frena más que el propio emulador.
* `BLOSSOM_TRACE_MMIO=1` saca por stderr cada acceso de los ARM11 a registros de E/S (los bucles
  de espera se cuentan en vez de repetirse). Sirve para comparar el intérprete con el JIT. El ARM9
  queda fuera a propósito, porque escribe las claves de la consola en el motor AES.

### Controles

| Tecla         | 3DS        |
|---------------|------------|
| Flechas       | D-Pad      |
| X / Z         | A / B      |
| S / A         | X / Y      |
| Q / W         | L / R      |
| Enter         | START      |
| Espacio       | SELECT     |
| H             | HOME       |
| P             | Power      |
| Ratón         | Pantalla táctil (pantalla inferior) |

## Hoja de ruta

1. **Build moderno que arranca GodMode9** (C++20, CMake, Qt 6, CI en Linux) ← ahora
2. JIT [dynarmic](https://github.com/azahar-emu/dynarmic) para ARM9 y ARM11 ← hecho, en pruebas
3. Separar la GPU en `video_core` con renderers OpenGL/Vulkan (basados en Azahar)
4. DSP con [teakra](https://github.com/wwylele/teakra)
5. Home Menu y juegos a velocidad jugable, en Old 3DS y New 3DS

## Licencia y créditos

Blossom se distribuye bajo la **GNU GPL v3** (ver [LICENSE.txt](LICENSE.txt)).

* **Corgi3DS** de PSI-Rockin y colaboradores (GPLv3): origen del núcleo LLE y del
  frontend. Importado del commit
  [`38d3d38`](https://github.com/PSI-Rockin/Corgi3DS/commit/38d3d3809bf57922aa40034e28fdf1d6e6856ad0).
* **Azahar** y el proyecto Citra (GPLv3): referencia de arquitectura, y fuente de los
  componentes que se vayan incorporando en próximos hitos.
* [dynarmic](https://github.com/azahar-emu/dynarmic) (0BSD), el JIT ARM, en
  `externals/dynarmic` con parches para LLE (ver
  [`externals/dynarmic/BLOSSOM.md`](externals/dynarmic/BLOSSOM.md)), junto con fmt, mcl,
  robin-map y xbyak.
* [tiny-AES-c](https://github.com/kokke/tiny-AES-c) (dominio público), usado para AES.
* armwrestler y thumbwrestler, suites de test de CPU ARM.

Blossom es un proyecto independiente y no está afiliado a Nintendo, ni a Corgi3DS, ni a Azahar.

## Sin dump de NAND

Si no tienes un dump de NAND, [`tools/mknand`](tools/mknand/README.md) genera una NAND sintética
de Old 3DS con un OTP inventado. Se arranca con `--dev-unsigned`. Está en fase experimental: de
momento solo llega hasta el NATIVE_FIRM, con CTRNAND vacía.
