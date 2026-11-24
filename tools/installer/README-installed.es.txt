SMRPG Recomp @VERSION@
=====================================

Super Mario RPG (Switch, 2023) recompilado para PC a partir de tu propia copia del juego.

Jugar
  El acceso directo «SMRPG Recomp» (escritorio o menú Inicio), o smrpg-recomp.exe --play en esta carpeta.
  Menú del port: Esc o F1 (vídeo, sonido, controles). Con mando: - y + a la vez.

Qué hay en esta carpeta
  extracted\       los archivos de tu copia del juego y el código del juego construido en tu PC (lasgame.dll)
  save\            tus partidas            larecomp.ini   tus ajustes          cache\   los shaders ya preparados
  builder\, gamelib-src\: con lo que el instalador construye el código del juego en tu PC, dentro de su propio
                   proceso (el generador en un Python incluido, y clang y lld): no hace falta instalar nada más
  licenses\        las licencias de lo que acompaña al programa

Actualizar
  Cierra el juego, ejecuta el instalador nuevo y elige esta misma carpeta como instalación y también como copia del
  juego (vale la que ya tiene: extracted\). Tus partidas y tus ajustes se quedan, y solo se vuelve a compilar lo que
  haya cambiado.

Desinstalar
  Copia antes save\ si quieres guardar tus partidas. Después borra esta carpeta y los accesos directos.

Si algo falla
  install.log (el instalador), build.log (la construcción del código del juego) y lasrecomp.log (el juego).

Necesita Windows 10 u 11 de 64 bits y una tarjeta gráfica con Vulkan 1.3. El código del juego se construye para el
procesador de este PC: con AVX2 (Intel desde 2013, AMD desde 2015) va como está pensado; sin AVX2 también funciona,
más lento. Si llevas esta carpeta a otro PC con otro procesador, vuelve a ejecutar allí el instalador.

Aviso legal
  Proyecto de aficionados, sin relación con Nintendo ni con Square Enix y sin su respaldo. Super Mario RPG y Mario son
  marcas de Nintendo; el juego es © Nintendo / SQUARE ENIX. El instalador y el programa no contienen nada del juego:
  el código y los datos del juego salen de tu copia. No compartas esta carpeta (extracted\ y las texturas volcadas
  son del juego).
