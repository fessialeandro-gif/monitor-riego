// /api/sensores
//   GET → [{canal, nombre, umbral}, ...]  (1 sensor)
//   PUT → {canal, nombre, umbral}  Requiere header X-Token = ADMIN_TOKEN (o DEVICE_TOKEN si no hay ADMIN_TOKEN)

import { getStore } from "@netlify/blobs";

const json = (data, status = 200) =>
  new Response(JSON.stringify(data), {
    status,
    headers: { "content-type": "application/json", "cache-control": "no-store" },
  });

const porDefecto = () =>
  [{ canal: 1, nombre: "Mi planta", umbral: 30 }];

export default async (req) => {
  const store = getStore({ name: "riego", consistency: "strong" });
  const sensores = (await store.get("sensores", { type: "json" })) || porDefecto();

  if (req.method === "GET") return json(sensores);

  if (req.method === "PUT") {
    const token = process.env.ADMIN_TOKEN || process.env.DEVICE_TOKEN;
    if (!token || req.headers.get("x-token") !== token) return json({ error: "clave incorrecta" }, 401);

    let b;
    try { b = await req.json(); } catch { return json({ error: "JSON inválido" }, 400); }
    const canal = Number(b.canal);
    if (!(canal === 1)) return json({ error: "canal inválido" }, 400);

    const nombre = String(b.nombre || "").trim().slice(0, 30) || "Mi planta";
    const umbral = Math.max(0, Math.min(100, Math.round(Number(b.umbral)) || 30));
    sensores[canal - 1] = { canal, nombre, umbral };
    await store.setJSON("sensores", sensores);
    return json(sensores[canal - 1]);
  }

  return json({ error: "método no permitido" }, 405);
};

export const config = { path: "/api/sensores" };
