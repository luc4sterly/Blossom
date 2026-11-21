# Blossom

Blossom es un emulador **LLE** (low-level) de Nintendo 3DS para PC x86-64. En vez de
reimplementar el sistema operativo como hacen los emuladores HLE, emula el hardware
(ARM9, ARM11 de 2 o 4 núcleos, DSP, GPU PICA200, crypto, eMMC, WiFi…) y **arranca el
firmware real** de la consola a partir de tus propios dumps.

El núcleo viene de [Corgi3DS](https://github.com/PSI-Rockin/Corgi3DS), modernizado y
reorganizado siguiendo la estructura de [Azahar](https://github.com/azahar-emu/azahar).

> Estado: hito 1 en curso. Compila con C++20, CMake y Qt 6. El objetivo de este hito es
> arrancar GodMode9 igual que el Corgi3DS original. Todavía va por intérprete, así que es lento.

## Estructura

```
src/
  common/      utilidades compartidas (excepciones, bswap, rotaciones)
  core/        núcleo LLE: cpu/ (ARM + VFP), arm9/ (AES, RSA, SHA, NDMA, eMMC, cartucho),
               arm11/ (GPU, DSP, MPCore, WiFi/Xtensa), scheduler, timers, PXI, I2C, SPI
  blossom_qt/  frontend en Qt 6
  tests/       runner de tests de CPU (armwrestler / thumbwrestler)
CMakeModules/  módulos de CMake (FindGMP)
```

## Compilar (Linux)

Dependencias en Ubuntu 24.04 o Debian 12+:

```sh
sudo apt install cmake ninja-build g++ qt6-base-dev libgl-dev libgmp-dev
```

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
2. JIT [dynarmic](https://github.com/azahar-emu/dynarmic) para ARM9 y ARM11
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
* [tiny-AES-c](https://github.com/kokke/tiny-AES-c) (dominio público), usado para AES.
* armwrestler y thumbwrestler, suites de test de CPU ARM.

Blossom es un proyecto independiente y no está afiliado a Nintendo, ni a Corgi3DS, ni a Azahar.
