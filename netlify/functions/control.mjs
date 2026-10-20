// /api/control  (POST, header X-Token = ADMIN_TOKEN)
//   body: { zona: "A"|"B", accion: "regar"|"detener" }
//
// Protecciones al pedir "regar":
//   - tiene que haber una lectura reciente (menos de MAX_EDAD_MIN minutos)
//   - los dos sensores de la zona tienen que tener señal
//   - el promedio de la zona tiene que estar POR DEBAJO del umbral de seco
// El ESP vuelve a chequear lo mismo antes de prender el relé, y corta solo por objetivo / tiempo / sin señal.

import { store, json, ZONAS, MAX_EDAD_MIN, leerConfig, leerControl, promedioZona, edadMin, autorizado } from "../lib/comun.mjs";

export default async (req) => {
  if (req.method !== "POST") return json({ error: "método no permitido" }, 405);
  if (!autorizado(req, "admin")) return json({ error: "Clave incorrecta." }, 401);

  let b;
  try { b = await req.json(); } catch { return json({ error: "JSON inválido" }, 400); }
  const z = b.zona, accion = b.accion;
  if (!ZONAS.includes(z) || !["regar", "detener"].includes(accion)) return json({ error: "Pedido inválido." }, 400);

  const st = store();
  const [config, control, ultima] = await Promise.all([leerConfig(st), leerControl(st), st.get("ultima", { type: "json" })]);
  const zona = config.zonas[z];

  if (accion === "regar") {
    if (edadMin(ultima) > MAX_EDAD_MIN)
      return json({ error: "No hay lecturas recientes del equipo. Revisá que el ESP esté conectado." }, 409);
    const prom = promedioZona(ultima, zona);
    if (prom === null)
      return json({ error: `Un sensor de ${zona.nombre} no tiene señal. No se puede regar sin medir.` }, 409);
    if (prom >= zona.umbral)
      return json({ error: `${zona.nombre} no está seca (${prom}%, umbral ${zona.umbral}%). No se riega.` }, 409);
  }

  control[z] = { orden: accion, id: (control[z].id || 0) + 1, ts: new Date().toISOString() };
  await st.setJSON("control", control);
  return json({ ok: true, control });
};

export const config = { path: "/api/control" };
