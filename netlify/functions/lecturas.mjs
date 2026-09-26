// /api/lecturas
//   POST  (ESP8266)  → guarda una lectura. Requiere header X-Token = DEVICE_TOKEN
//   GET   (web)      → ?horas=24  devuelve { ultima, lecturas: [...] }
//
// Guardado en Netlify Blobs: un blob por día ("dia/2026-09-26") con el array de lecturas del día,
// más "ultima" con la última lectura.

import { getStore } from "@netlify/blobs";

const DIAS_RETENCION = 90;
const CAMPOS = ["s1"];

const json = (data, status = 200) =>
  new Response(JSON.stringify(data), {
    status,
    headers: { "content-type": "application/json", "cache-control": "no-store" },
  });

const claveDia = (d) => "dia/" + d.toISOString().slice(0, 10); // día en UTC

function limpiarPct(v) {
  if (v === null || v === undefined || v === "") return null;
  const n = Math.round(Number(v));
  return Number.isFinite(n) ? Math.max(0, Math.min(100, n)) : null;
}
function limpiarNum(v) {
  if (v === null || v === undefined || v === "") return null;
  const n = Number(v);
  return Number.isFinite(n) ? Math.round(n * 10) / 10 : null;
}

export default async (req) => {
  const store = getStore({ name: "riego", consistency: "strong" });

  // ---------- POST: llega una lectura del ESP ----------
  if (req.method === "POST") {
    const token = process.env.DEVICE_TOKEN;
    if (!token || req.headers.get("x-token") !== token) return json({ error: "token inválido" }, 401);

    let body;
    try { body = await req.json(); } catch { return json({ error: "JSON inválido" }, 400); }

    const ahora = new Date();
    const lectura = { created_at: ahora.toISOString() };
    for (const c of CAMPOS) lectura[c] = limpiarPct(body[c]);
    lectura.temperatura = limpiarNum(body.temperatura);
    lectura.humedad_aire = limpiarNum(body.humedad_aire);

    const key = claveDia(ahora);
    const dia = (await store.get(key, { type: "json" })) || [];
    const diaNuevo = dia.length === 0;
    dia.push(lectura);
    await store.setJSON(key, dia);
    await store.setJSON("ultima", lectura);

    // Primer dato del día: borrar días viejos
    if (diaNuevo) {
      const limite = claveDia(new Date(ahora - DIAS_RETENCION * 864e5));
      const { blobs } = await store.list({ prefix: "dia/" });
      await Promise.all(blobs.filter((b) => b.key < limite).map((b) => store.delete(b.key)));
    }
    return json({ ok: true }, 201);
  }

  // ---------- GET: la web pide historial ----------
  if (req.method === "GET") {
    const url = new URL(req.url);
    const horas = Math.max(1, Math.min(24 * 31, Number(url.searchParams.get("horas")) || 24));
    const hasta = new Date();
    const desde = new Date(hasta - horas * 3600e3);

    const claves = [];
    for (let d = new Date(desde); claveDia(d) <= claveDia(hasta); d = new Date(+d + 864e5)) claves.push(claveDia(d));
    if (!claves.includes(claveDia(hasta))) claves.push(claveDia(hasta));

    const dias = await Promise.all(claves.map((k) => store.get(k, { type: "json" })));
    let lecturas = dias.flat().filter((l) => l && new Date(l.created_at) >= desde);

    // Reducir puntos para rangos largos (máx ~600 por serie)
    if (lecturas.length > 600) {
      const paso = Math.ceil(lecturas.length / 600);
      lecturas = lecturas.filter((_, i) => i % paso === 0 || i === lecturas.length - 1);
    }

    const ultima = await store.get("ultima", { type: "json" });
    return json({ ultima, lecturas });
  }

  return json({ error: "método no permitido" }, 405);
};

export const config = { path: "/api/lecturas" };
