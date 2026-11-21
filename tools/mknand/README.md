# mknand: NAND sintética de Old 3DS

Genera una imagen de NAND de Old 3DS que Blossom puede arrancar con el boot9 emulado,
sin tener un dump de una consola real. Es la fase 1 del estudio de la NAND sintética:
llega hasta que el boot9 acepte el OTP, la cabecera y el FIRM, y salte al NATIVE_FIRM.
CTRNAND va formateada pero vacía (sin títulos ni bases de datos todavía).

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
| CTR MBR + CTRNAND | MBR y FAT16 vacía, cifradas con el keyslot 0x04 |

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
    -o nand_synth.bin

./build/src/blossom_qt/blossom --boot9 boot9.bin --boot11 boot11.bin \
    --nand nand_synth.bin --dev-unsigned --autoload-nocart
```

Opciones útiles:

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

- Instalar los títulos de sistema en CTRNAND (`title/`, `dbs/*.db` con su CMAC, tickets).
- `movable.sed`, `SecureInfo_A` autofirmado, HWCAL.
- New 3DS: necesita el sector secreto 0x96 de una consola real.
