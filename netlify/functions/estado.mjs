// /api/estado  (GET)
//   ?esp=1 → { config, control }                         (lo consulta el ESP seguido, respuesta chica)
//   sin parámetro → { config, control, ultima, eventos }  (la web)

import { store, json, leerConfig, leerControl, leerEventos } from "../lib/comun.mjs";

export default async (req) => {
  if (req.method !== "GET") return json({ error: "método no permitido" }, 405);
  const st = store();
  const [config, control] = await Promise.all([leerConfig(st), leerControl(st)]);
  if (new URL(req.url).searchParams.get("esp")) return json({ config, control });

  const [ultima, eventos] = await Promise.all([st.get("ultima", { type: "json" }), leerEventos(st)]);
  return json({ config, control, ultima, eventos: eventos.slice(-15).reverse() });
};

export const config = { path: "/api/estado" };
