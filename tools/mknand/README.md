# mknand: NAND sintética de Old 3DS

Genera una imagen de NAND de Old 3DS que Blossom puede arrancar con el boot9 emulado,
sin tener un dump de una consola real. Es la fase 1 del estudio de la NAND sintética:
llega hasta que el boot9 acepte el OTP, la cabecera y el FIRM, y salte al NATIVE_FIRM.
Con `--cias` instala además los títulos de sistema en CTRNAND y escribe `dbs/title.db` e `import.db`.

## Qué mete en la imagen

| Parte | Contenido |
|---|---|
| OTP | Inventado: magic `0xDEADB00F`, DeviceId y datos de claves aleatorios, SHA-256 correcto, cifrado con la clave OTP de **tu** `boot9.bin` |
| CID | 16 bytes inventados; de ahí salen los contadores AES de cada partición |
| NCSD (0x0) | Tabla de particiones estándar de Old 3DS, firmada "a sí misma" (ver abajo) |
| essential.exefs (0x200) | `otp` (cifrado) y `nand_cid`, como lo deja GodMode9 y como lo lee Blossom |
| TWL MBR + TWLN + TWLP | MBR estándar y FAT16 vacías, cifradas con la clave TWL derivada del OTP |
| AGB_FIRM save | Vacía |
| FIRM0 / FIRM1 | NATIVE_FIRM de Old 3DS (`0004013800000002`), cifrado con el keyslot 0x06 |
| `dbs/title.db`, `dbs/import.db` | Escritos desde cero: contenedor DIFF (árbol IVFC), sistema de archivos BDRI con una entrada de 0x80 bytes por título y CMAC `CTR-9DB0` con el keyslot 0x0B (derivado del OTP). Ver `titledb.py` |
| CTR MBR + CTRNAND | MBR y FAT16, cifradas con el keyslot 0x04. Con `--cias`, `title/<alto>/<bajo>/content/00000000.tmd` y un `<id>.app` por contenido (sin la capa de title key; el cifrado NCCH se queda como en la consola) |

Toda la criptografía (claves de consola a partir del OTP, contadores a partir del CID, title key
de los CIA) la hace [pyctr](https://github.com/ihaveamac/pyctr) (MIT). No se incluye ni se descarga
ninguna clave ni firmware: todo sale de tus archivos.

## Firma de la cabecera NCSD

El boot9 comprueba la firma RSA de la cabecera NCSD y nadie fuera de Nintendo puede firmarla.
mknand escribe como "firma" el propio bloque PKCS#1 que el boot9 espera ver tras descifrarla.
Si arrancas Blossom con `--dev-unsigned`, el motor RSA trata ese bloque como si el exponente
fuera 1, y el boot9 lo acepta. Las firmas reales (la del NATIVE_FIRM, por ejemplo) se siguen
verificando igual que siempre, porque ya descifran a un bloque válido.

## Uso

```sh
python3 -m venv .venv && .venv/bin/pip install -r tools/mknand/requirements.txt

.venv/bin/python tools/mknand/mknand.py \
    --boot9 /ruta/a/boot9.bin \
    --native-firm-cia /ruta/a/0004013800000002.cia \
    --cias /ruta/a/11.17.0-50E.OLD/ \
    -o nand_synth.bin

./build/src/blossom_qt/blossom --boot9 boot9.bin --boot11 boot11.bin \
    --nand nand_synth.bin --dev-unsigned --autoload-nocart
```

### Si AM rehace las bases de datos

En el primer arranque, AM puede descartar `title.db` e `import.db` y crear las suyas vacías
(junto con `ticket.db`, `certs.db`, `tmp_t.db` y `tmp_i.db`). En ese caso, después de arrancar
una vez:

```sh
# qué bases de datos hay, su geometría y si su CMAC cuadra con las claves de esta NAND
.venv/bin/python tools/mknand/mknand.py --boot9 boot9.bin --inspect-dbs nand_synth.bin

# añadir los títulos al title.db que creó AM, respetando su geometría, y volver a firmarlo
.venv/bin/python tools/mknand/mknand.py --boot9 boot9.bin --update-dbs nand_synth.bin \
    --cias /ruta/a/11.17.0-50E.OLD/
```

### Archivos .cmd y movable.sed

Cada título lleva `content/cmd/00000001.cmd`, la lista de contenidos instalados que Process9
consulta antes de abrir el `.tmd`. Su AES-CMAC usa el keyslot 0x0B, el mismo de las bases de
datos (probado: con 0x30 Process9 da 0xD9004586). mknand también crea `/private/movable.sed`, falso (sin CMAC) con keyY aleatoria y la firma RSA de
la LocalFriendCodeSeed autofirmada como la cabecera NCSD (Process9 la comprueba al arrancar, así
que hace falta `--dev-unsigned`). Para una imagen
hecha antes de esto:

```sh
.venv/bin/python tools/mknand/mknand.py --boot9 boot9.bin --fix-titles nand_synth.bin \
    --cias /ruta/a/11.17.0-50E.OLD/
```

Además escribe `/rw/sys/SecureInfo_A` autofirmado con la región de la consola (`--region`, EUR
por defecto; con `--fix-titles`, se reescribe si das `--region`). Sin él, cfg cae en otra región
y NS pide el menú y los applets de esa región.
`--movable new` sustituye el movable.sed de la imagen por uno nuevo, y `--movable none` lo quita
(los .cmd se firman entonces con una keyY a cero; sirve para aislar problemas).

Opciones útiles:

- `--cias RUTA...`: CIA sueltos o carpetas con CIA. Los títulos TWL (`0004800x…`) se saltan de momento.
- `--explain IMAGEN`: no genera nada; imprime los SHA que el boot9 calcula para esa imagen, para compararlos con un log de Blossom.
- `--firm archivo.firm`: usar un NATIVE_FIRM ya descifrado en vez del CIA.
- `--seed texto`: OTP y CID reproducibles (misma semilla, misma NAND).
- `--no-verify`: no releer la imagen al terminar.

Al acabar, mknand reabre la imagen igual que si fuera un dump (cogiendo OTP y CID solo de
essential.exefs) y comprueba que los MBR, los FIRM y las FAT16 se descifran bien.

## Tests

```sh
.venv/bin/python -m unittest discover -s tools/mknand -v
```

El test de extremo a extremo usa un boot9 de mentira con claves aleatorias, así que comprueba
que la imagen es coherente consigo misma, no que el boot9 real la acepte. Eso solo se puede
probar en Blossom con tu propio `boot9.bin`.

## Lo que falta (fases siguientes)

- `dbs/ticket.db` y `dbs/certs.db`.
- Ficheros `.cmd` de los títulos, si Process9 los pide.
- Títulos TWL en TWLN.
- `movable.sed`, `SecureInfo_A` autofirmado, HWCAL.
- New 3DS: necesita el sector secreto 0x96 de una consola real.

Nota: genera la imagen en el sistema de archivos de Linux (por ejemplo `~/` en WSL2), no en
`/mnt/c`. Usa archivos dispersos de casi 1 GB y en NTFS van mucho más lentos.
