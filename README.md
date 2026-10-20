# Monitor de Riego — 4 sensores, 2 zonas, 2 relés (ESP8266 + Netlify)

- **Zona A** = sensores 1 y 2 → relé A · **Zona B** = sensores 3 y 4 → relé B
- La web muestra el **promedio** de cada zona y te deja prender el riego **solo si está seca**.
- El riego se corta solo (al llegar al % de corte o al tiempo máximo) o con el botón **Detener**.

![Conexiones](docs/conexiones.png)

## Cómo funciona el riego

```
[Web] botón "Regar Zona A"
   │  POST /api/control  (con tu clave)
   ▼
[Netlify] ¿hay lectura reciente? ¿los 2 sensores tienen señal? ¿promedio < umbral de seco?
   │  si alguna falla → rechaza y la web muestra el motivo
   ▼  si todo ok → guarda la orden
[ESP8266] la recibe en ≤15 s, VUELVE a chequear que esté seco y prende el relé
   │  mientras riega lee cada 3 s y corta solo si:
   │    • el promedio llega a "cortar al llegar a"
   │    • pasa el tiempo máximo
   │    • un sensor de la zona se queda sin señal
   │    • apretás "Detener" en la web
   ▼
[Web] muestra "Regando…", el corte y el historial de riegos
```

### Protecciones
| Protección | Dónde |
|---|---|
| No riega si el promedio **no** está por debajo del umbral de seco | Netlify **y** ESP |
| No riega si un sensor de la zona no tiene señal | Netlify **y** ESP |
| No riega si el equipo no mandó datos en los últimos 15 min | Netlify |
| Corta al llegar al % de corte | ESP (funciona sin internet) |
| Corta por tiempo máximo (por defecto 10 min) | ESP (funciona sin internet) |
| Al prender/reiniciar el ESP los relés arrancan apagados y no retoma órdenes viejas | ESP |
| Solo con clave se puede regar o cambiar valores | Netlify |

Los valores (umbral de seco, % de corte, tiempo máximo, nombres) se cambian desde la web con el ⚙ de cada zona.

## Pasar tu sitio actual (monitor-riego-1) a esta versión

Usás el mismo repo, el mismo sitio de Netlify y el mismo `DEVICE_TOKEN`.

1. En la carpeta del repo **reemplazá**: `public/`, `netlify/`, `docs/`, `package.json`, `README.md`.
2. **Borrá** `netlify/functions/sensores.mjs` (ya no se usa).
3. Subí los cambios **sin** el `.ino` con tu token:
   ```bash
   git add public netlify docs package.json README.md
   git rm netlify/functions/sensores.mjs
   git commit -m "4 sensores, 2 zonas y riego"
   git push
   ```
4. (Opcional, recomendado) En Netlify → Environment variables agregá `ADMIN_TOKEN` con otra clave larga.
   Esa es la clave de la web para regar y cambiar valores. Si no la creás, la web usa `DEVICE_TOKEN`.
   Después: **Deploys → Trigger deploy**.

## Firmware (`firmware/monitor_riego_4/monitor_riego_4.ino`)

1. Librerías (Herramientas → Administrar bibliotecas):
   - **ArduinoJson** (de Benoit Blanchon, versión 7)
   - **DHT sensor library** y **Adafruit Unified Sensor** (si usás DHT)
2. Placa: **NodeMCU 1.0 (ESP-12E Module)**.
3. Completá `WIFI_SSID`, `WIFI_PASS`, `BASE_URL` (`https://TU-SITIO.netlify.app`, sin `/api`) y `DEVICE_TOKEN`.
4. **Calibrá cada sensor** (con los diodos es obligatorio):
   - `MODO_CALIBRACION = true`, subir, Monitor Serie a 115200. Vas a ver `S1=… S2=… S3=… S4=…`.
   - Todos al aire → anotá cada valor en `CAL_SECO`. Todos en un vaso de agua → `CAL_MOJADO`.
   - Volvé a `MODO_CALIBRACION = false` y subí.
5. Si tu módulo de relés se activa con HIGH (poco común), poné `RELE_ACTIVO_BAJO = false`.

En el Monitor Serie vas a ver algo así:
```
S1=512(31%) S2=498(35%) S3=430(48%) S4=455(42%) T=22.4 H=51.0 | A=33% B=45%
POST {"s1":31,"s2":35,...}
>>> Zona A: RIEGO ON (promedio 27%)
>>> Zona A: RIEGO OFF (corte_objetivo, promedio 46%)
```

## Uso de Netlify
El ESP consulta órdenes cada 15 s (cada 5 s mientras riega) y manda lecturas cuando cambian.
Son unas 180.000 llamadas por mes. Revisá el consumo en Netlify → *Usage*; si te acercás al límite
de tu plan, subí `CONSULTA_MS` a 30000 (la respuesta al botón tarda hasta 30 s).

## Estructura
```
public/index.html                 la web
netlify/functions/lecturas.mjs    POST del ESP (devuelve órdenes) / GET historial
netlify/functions/estado.mjs      estado actual (web y ESP)
netlify/functions/control.mjs     botón Regar / Detener, con las validaciones
netlify/functions/config.mjs      umbrales, % de corte, tiempo máximo, nombres
netlify/lib/comun.mjs             funciones compartidas
firmware/monitor_riego_4/         código del ESP8266
docs/conexiones.png               croquis
```
