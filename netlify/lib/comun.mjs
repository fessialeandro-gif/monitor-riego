// Funciones compartidas por todas las Netlify Functions
import { getStore } from "@netlify/blobs";

export const store = () => getStore({ name: "riego", consistency: "strong" });

export const json = (data, status = 200) =>
  new Response(JSON.stringify(data), {
    status,
    headers: { "content-type": "application/json", "cache-control": "no-store" },
  });

export const ZONAS = ["A", "B"];
export const SENSORES = ["s1", "s2", "s3", "s4"];
export const MAX_EDAD_MIN = 15; // una lectura más vieja que esto no sirve para decidir si regar

export const CONFIG_DEFAULT = {
  plantas: ["Planta 1", "Planta 2", "Planta 3", "Planta 4"],
  zonas: {
    A: { nombre: "Zona A", sensores: [1, 2], umbral: 30, objetivo: 45, maxMin: 10 },
    B: { nombre: "Zona B", sensores: [3, 4], umbral: 30, objetivo: 45, maxMin: 10 },
  },
};

const CONTROL_DEFAULT = () => ({
  A: { orden: "detener", id: 0, ts: null },
  B: { orden: "detener", id: 0, ts: null },
});

export async function leerConfig(st) {
  const c = (await st.get("config", { type: "json" })) || {};
  const out = structuredClone(CONFIG_DEFAULT);
  if (Array.isArray(c.plantas)) c.plantas.forEach((n, i) => { if (i < 4 && n) out.plantas[i] = n; });
  for (const z of ZONAS) Object.assign(out.zonas[z], c.zonas?.[z] || {}, { sensores: out.zonas[z].sensores });
  return out;
}

export async function leerControl(st) {
  const c = (await st.get("control", { type: "json" })) || {};
  const out = CONTROL_DEFAULT();
  for (const z of ZONAS) Object.assign(out[z], c[z] || {});
  return out;
}

export async function leerEventos(st) {
  return (await st.get("eventos", { type: "json" })) || [];
}

// Promedio de la zona. null si falta la lectura o algún sensor está sin señal.
export function promedioZona(lectura, zona) {
  if (!lectura) return null;
  const vals = zona.sensores.map((n) => lectura["s" + n]);
  if (vals.some((v) => v === null || v === undefined)) return null;
  return Math.round(vals.reduce((a, b) => a + b, 0) / vals.length);
}

export function edadMin(lectura) {
  return lectura ? (Date.now() - new Date(lectura.created_at)) / 60000 : Infinity;
}

// tipo "device" = el ESP, tipo "admin" = acciones desde la web
export function autorizado(req, tipo) {
  const device = process.env.DEVICE_TOKEN;
  const admin = process.env.ADMIN_TOKEN || device;
  const t = req.headers.get("x-token");
  if (!t) return false;
  return tipo === "device" ? t === device : t === admin;
}
