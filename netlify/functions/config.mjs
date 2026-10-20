// /api/config
//   GET → configuración actual
//   PUT (header X-Token = ADMIN_TOKEN) → { plantas:[...4], zonas:{ A:{nombre,umbral,objetivo,maxMin}, B:{...} } }

import { store, json, ZONAS, leerConfig, autorizado } from "../lib/comun.mjs";

const entero = (v, min, max, def) => {
  const n = Math.round(Number(v));
  return Number.isFinite(n) ? Math.max(min, Math.min(max, n)) : def;
};
const texto = (v, def) => String(v ?? "").trim().slice(0, 30) || def;

export default async (req) => {
  const st = store();
  const actual = await leerConfig(st);

  if (req.method === "GET") return json(actual);

  if (req.method === "PUT") {
    if (!autorizado(req, "admin")) return json({ error: "Clave incorrecta." }, 401);
    let b;
    try { b = await req.json(); } catch { return json({ error: "JSON inválido" }, 400); }

    if (Array.isArray(b.plantas)) actual.plantas = actual.plantas.map((n, i) => texto(b.plantas[i], n));

    for (const z of ZONAS) {
      const nz = b.zonas?.[z];
      if (!nz) continue;
      const zona = actual.zonas[z];
      zona.nombre = texto(nz.nombre, zona.nombre);
      zona.umbral = entero(nz.umbral, 0, 95, zona.umbral);
      zona.objetivo = entero(nz.objetivo, 1, 100, zona.objetivo);
      zona.maxMin = entero(nz.maxMin, 1, 60, zona.maxMin);
      if (zona.objetivo <= zona.umbral) {
        return json({ error: `${zona.nombre}: "cortar al llegar a" tiene que ser mayor que el umbral de seco.` }, 400);
      }
    }
    await st.setJSON("config", { plantas: actual.plantas, zonas: actual.zonas });
    return json(actual);
  }

  return json({ error: "método no permitido" }, 405);
};

export const config = { path: "/api/config" };
