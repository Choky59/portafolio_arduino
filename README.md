# portafolio_arduino

Firmware de los proyectos del portafolio de Jorge García ("haciendo proyectos hasta que una empresa me contacte"). Cada carpeta es un sketch de Arduino IDE.

| Sketch | Proyecto | Placa |
|---|---|---|
| [`sensor_temperatura`](sensor_temperatura) | #1 ¿Se puede salir? | ESP32 + DHT11 |

## sensor_temperatura

Mide temperatura y humedad con un DHT11 y las envía cada 30 segundos al dashboard en vivo.

### Conexiones

| DHT11 | ESP32 |
|---|---|
| VCC | 3V3 |
| DATA | GPIO 26 |
| GND | GND |

Si el DHT11 es el sensor suelto de 4 patas (sin módulo), agrega una resistencia de 10 kΩ entre VCC y DATA.

### Librerías

Desde el Gestor de librerías del Arduino IDE:
- **DHT sensor library** (Adafruit)
- **Adafruit Unified Sensor**
- **ArduinoJson** v7 (Benoit Blanchon)

### Configuración

1. Copia `sensor_temperatura/secrets.example.h` como `sensor_temperatura/secrets.h`.
2. Llena el nombre y la contraseña de tu WiFi, y la `API_URL` del backend.
3. Si la URL es `https://`, pega también el certificado raíz del servidor. Los pasos están en el mismo archivo.

`secrets.h` no se sube al repo.

### Vincular el sensor (una sola vez)

1. En la página, entra a `/admin`, crea el sensor (tipo ESP32) y asígnale el proyecto **¿Se puede salir?**.
2. Sube el sketch y abre el Monitor Serie a **115200** baudios.
3. Escribe el código que te dio el panel (por ejemplo `K7QM-4XRT`) y presiona Enter.

El ESP32 canjea el código por su propia llave secreta y la guarda en su memoria (NVS). El código dura 10 minutos y sirve una sola vez.

**Por qué funciona así:** el código de este repo es público, pero ninguna llave vive en él. Cada sensor tiene la suya y se puede revocar desde el panel sin afectar a los demás. Si revocas o borras el sensor, el ESP32 lo detecta, borra su llave y vuelve a pedir un código.

### Comandos por Serial

| Comando | Qué hace |
|---|---|
| `K7QM-4XRT` (el código) | Vincula el sensor |
| `estado` | Muestra el firmware, el servidor, la IP y el ID del sensor |
| `olvidar` | Borra la llave guardada para volver a vincularlo |

### Probar en tu red local

1. Levanta el backend en tu PC (`npm run dev` en `portafolio_back`, puerto 3000).
2. En `secrets.h`, usa `#define API_URL "http://IP-DE-TU-PC:3000"`. La IP la ves con `ipconfig`.
3. Si el ESP32 no llega al servidor, permite Node.js en el Firewall de Windows para redes privadas.

Con `http://` la llave viaja sin cifrar. Úsalo solo para pruebas en tu red y en producción usa `https://`.
