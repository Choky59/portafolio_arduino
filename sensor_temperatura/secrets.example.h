// Copia este archivo como "secrets.h" (en esta misma carpeta) y llena tus datos.
// secrets.h esta en .gitignore: tu WiFi nunca se sube al repo.
// La llave del sensor NO va aqui: el ESP32 la obtiene con el codigo de vinculacion
// y la guarda en su memoria interna.

#pragma once

#define WIFI_SSID     "nombre-de-tu-wifi"
#define WIFI_PASSWORD "contrasena-de-tu-wifi"

// URL del backend, sin "/" al final.
//   Produccion:  "https://tu-api.herokuapp.com"
//   Pruebas en tu red local (la IP de tu PC, backend en el puerto 3000):
//                "http://192.168.1.50:3000"
#define API_URL "https://tu-api.herokuapp.com"

// Certificado raiz (CA) del servidor, solo se usa con https://.
// Como obtenerlo: abre la URL de tu API en Chrome -> candado -> "La conexion es segura"
// -> "El certificado es valido" -> pestana Detalles -> selecciona el certificado de
// hasta arriba (el raiz) -> Exportar como "Base64 (PEM)". Pega aqui su contenido.
static const char ROOT_CA[] = R"PEM(
-----BEGIN CERTIFICATE-----
PEGA_AQUI_EL_CERTIFICADO_RAIZ
-----END CERTIFICATE-----
)PEM";
