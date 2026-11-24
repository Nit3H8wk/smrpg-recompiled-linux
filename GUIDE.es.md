# SMRPG Recomp: guía rápida

[English](GUIDE.md) · **Español**

Port no oficial, por recompilación estática, de Super Mario RPG (Switch, 1.0.0) a PC con Vulkan. Necesita los archivos
de tu propia copia del juego, ya extraídos (las carpetas `exefs` y `romfs`), que no van en este repositorio. Proyecto
de aficionados, sin relación con Nintendo ni con Square Enix; Super Mario RPG es marca de Nintendo.

## Instalar (para jugar sin el entorno de desarrollo)

`SMRPG-Recomp-Installer-<versión>.exe` (de la release de GitHub, o `make installer` → `build/dist/`): una ventana donde
se elige **la carpeta de tu copia del juego, ya extraída** (la que trae `exefs` y `romfs`, versión 1.0.0; también vale
una instalación anterior) y la **carpeta de instalación**. Al pulsar Instalar:

1. comprueba tu copia antes de tocar nada (Super Mario RPG 1.0.0: el build ID de `exefs/main`, el título y los 1.934
   archivos de la RomFS por su ruta y su tamaño) y la pone en `extracted/` de la instalación: con enlaces duros si está
   en la misma unidad (ni tiempo ni sitio de más) y, si no, copiada;
2. copia el programa (el ejecutable, sus DLL y `builder\`: **nada del juego**);
3. construye el código del juego **en tu PC y dentro del propio instalador**, sin abrir otros procesos:
   `builder\builder.dll` lleva el generador (en un Python incluido), clang y lld, y traduce el programa del juego a C
   y lo compila en `extracted/lasgame.dll` para el procesador de ese PC (con AVX2 si lo tiene; si no, también funciona);
   unos 13 minutos con 16 hilos (actualizar encima, segundos). No hace falta nada más que Windows 10 u 11 de 64 bits;
4. crea los accesos directos (escritorio y menú Inicio) a `smrpg-recomp.exe --play`.

Pide unos 11 GB libres (4 si la copia está en la misma unidad) y una ruta de como mucho 110 caracteres (el juego no
usa rutas largas de Windows). Si se corta, se reanuda; instalar encima de una instalación anterior (con el juego cerrado;
como copia del juego vale la propia instalación) la actualiza sin tocar las partidas (`save/`) ni los ajustes
(`larecomp.ini`). Registros: `install.log` y `build.log` en la carpeta.

El instalador sale en español con Windows en español, y en inglés si no (`--lang en|es` fuerza uno).

Para construir el instalador (desde MSYS2 UCRT64, después de `tools/setup_windows.ps1`):
- los paquetes `mingw-w64-ucrt-x86_64-clang`, `-lld` y `-llvm` de MSYS2 (probado con LLVM 23) y `make deps-windows`
  (el FFmpeg LGPL);
- las tres DLL de AMD FSR (`amd_fidelityfx_loader_vk.dll`, `amd_fidelityfx_upscaler_vk.dll` y
  `amd_fidelityfx_framegeneration_vk.dll`) en `build/`, de `Kits/FidelityFX/signedbin/` del FidelityFX SDK de AMD
  (`third_party/amd-fsr/README.txt`);
- `build/lsfg/windows/lsfg.dll`: el código de [lsfg-vk-android](https://github.com/GameNative/lsfg-vk-android) (rama
  release, commit 437208a, con sus submódulos) en `../deps/src/lsfg-vk-android` y `sh tools/lsfg/build.sh windows`;
- y `make installer`, que crea `build/gen-venv` con `py -3.12` (el Python 3.12 de python.org) y descarga y comprueba
  el Python «embeddable».

Probar el constructor suelto: `tools/installer/test_builder.c`. Pruebas sin ventana:
`installer.exe --game J --dest D [--no-shortcuts] --auto --hidden` (código 0 = bien), `--copy-only`,
`--screenshot ventana.bmp`; `SMRPG_INSTALLER_SHORTCUTS=<carpeta>` deja ahí los accesos directos.

## Jugar

- Instalado: el acceso directo «SMRPG Recomp». Desde el repositorio: `play.cmd` (con los controles explicados dentro del
  archivo, en inglés).
- Menú: **Esc** o **F1** (también Home o Back + Start en el mando). Pestañas: Controles, Vídeo, Audio, Salir. Sale en
  español si el idioma del juego es el español (Vídeo > Idioma del juego), y en inglés si no.
- Menú > **Vídeo**, recuadro de la derecha: arriba, lo que gasta el juego ahora (CPU, GPU, RAM, SSD, VRAM);
  encima de los botones, el impacto del ajuste elegido en cada uno (verde, amarillo, rojo, con cifras);
  abajo, Guardar, Deshacer, Restablecer y Reiniciar (con mando: Abajo desde la última fila).
- Teclas y botones: Menú > **Controles** dice qué hace cada uno (con el teclado: W A S D moverse, K hablar / aceptar,
  J saltar / cancelar, L objetos, I especiales, Intro menú del juego); «Configurar controles» los cambia.

## Ajustes recomendados (Vídeo)

Para un monitor ultrawide de alta frecuencia con HDR y una tarjeta potente (probado: 3440x1440, RX 9070 XT):

| Ajuste | Valor | Para qué |
| --- | --- | --- |
| Modo de pantalla | Ventana sin bordes | Ocupa el monitor a su resolución sin ser pantalla completa (Alt+Tab al momento) |
| Pantalla ancha | Sí, llenar la pantalla | En 21:9, más escena a los lados (nada se estira); sigue a la ventana también en marcha |
| Vídeos en pantalla ancha | Llenar la pantalla | Las películas cubren el 21:9 sin estirarse (recortan un poco arriba y abajo); el fondo del título, en 16:9 |
| Límite de FPS | 240 (o el de tu monitor); con generación, la mitad (120) | Fuera de los combates el juego cuenta por tiempo; la lógica de los combates va siempre a 60 pasos por segundo, como en la consola |
| Negro puro | Sí | Quita el velo verdoso que el etalonaje del juego deja en el negro (cargas, lados de los vídeos, sombras) |
| Resolución interna | Igual que la pantalla | Medido a 3440x1440: 3,9 ms por imagen; 4K, 7,6; 8K, 13,7 (y sin FSR) |
| Antialiasing | AMD FSR | FSR 4 en las Radeon RX 9000 (FSR 3.1 en las demás). Trabaja a la resolución interna: en una RX 9070 XT, FSR 4 cuesta 1,4 ms por imagen a 3440x1440 y 3,1 ms a 2160p en 21:9 (de 240 a ~175 FPS); SMAA, casi nada |
| Calidad de FSR | Nativo (antialiasing) | Los demás modos dibujan con menos píxeles y amplían: para tarjetas modestas |
| Generación de fotogramas | Opcional | Una imagen generada entre cada dos: con el límite en 120, se ven 240. Algo más de retardo |
| Supersampling | No (o x2/x4 si sobra tarjeta) | Se combina con FSR |
| Nitidez | 2-4 | Afinado final (AMD RCAS) |
| Filtrado anisótropo | 16x | |
| HDR | Sí, si el monitor lo tiene | |

AMD FSR necesita junto a `build/smrpg-recomp.exe` las DLL del SDK de AMD FSR 2.4
(`amd_fidelityfx_loader_vk.dll`, `amd_fidelityfx_upscaler_vk.dll`; para la generación de fotogramas, también
`amd_fidelityfx_framegeneration_vk.dll`): ver `third_party/amd-fsr/README.txt`.

**Generación de fotogramas** (Vídeo > Imagen, con Antialiasing «AMD FSR»): entre cada dos imágenes del juego AMD
presenta una generada con los vectores de movimiento del juego (FG 4, por red neuronal, en las Radeon RX 9000; FG 3.1 en
las demás). Más fluido a cambio de algo de retardo. Lo mejor: límite de FPS en la mitad de la frecuencia del monitor
(120 en uno de 240 Hz) y al menos 60 imágenes del juego. Se pausa con el menú abierto; el contador de FPS dice lo que
se ve («120 fps (240 con generación)»).

**Generación de Lossless Scaling** (Vídeo > «Generación Lossless Scaling»): usa el `Lossless.dll` de **tu** copia de
Lossless Scaling de Steam (de pago; se busca en el registro y en tus bibliotecas de Steam). Ni el repositorio ni el
instalador lo llevan ni lo descargan; sin él, la opción queda apagada.

## Texturas HD

Por ahora, solo desde el repositorio (el instalador no lleva `hd-textures.cmd`). Las texturas volcadas son del juego:
no las compartas.

1. Menú > Vídeo > Imagen > **Volcar texturas**: Sí. Juega un rato: cada textura nueva se guarda en `textures-dump/`.
2. Doble clic en **`hd-textures.cmd`**: amplía con IA (Real-ESRGAN ncnn Vulkan, `realesrgan-ncnn-vulkan-*-windows.zip`
   de https://github.com/xinntao/Real-ESRGAN/releases, descomprimido en la carpeta `realesrgan/`) las texturas de
   color volcadas y las deja en `textures/` (x2; `hd-textures.cmd --scale 4` para x4, que gasta más memoria de vídeo).
3. **Texturas HD**: Sí (viene activado). El juego usa las de `textures/` la próxima vez que carga cada textura.

También se pueden poner a mano: cualquier PNG en `textures/` cuyo nombre empiece por la huella de 16 cifras de su
volcado, con las mismas proporciones y al menos el mismo tamaño. (Las carpetas `texturas/` y `texturas-volcadas/` de las
versiones anteriores siguen valiendo.)

## Qué hay hecho

El juego se juega de principio a fin en lo probado (vídeos, título, menús, partida, batallas, guardar y cargar, zonas
de todo el mapa). El detalle de cada versión está en `CHANGELOG.txt`; el estado técnico, en `STATUS.md` (los dos, en
inglés).
