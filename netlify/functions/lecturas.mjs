// /api/lecturas
//   POST (ESP8266, header X-Token = DEVICE_TOKEN)
//        body: { s1..s4, temperatura, humedad_aire, rA, rB, eventos:[{zona,tipo,valor}] }
//        responde { ok, config, control } → así el ESP recibe las órdenes en la misma llamada
//   GET  (web) ?horas=24 → { ultima, lecturas }

import { store, json, SENSORES, ZONAS, leerConfig, leerControl, leerEventos } from "../lib/comun.mjs";
import { autorizado } from "../lib/comun.mjs";

const DIAS_RETENCION = 90;
const claveDia = (d) => "dia/" + d.toISOString().slice(0, 10);
const CORTES = ["corte_objetivo", "corte_tiempo", "corte_manual", "corte_sin_senal", "rechazado"];

function pct(v) {
  if (v === null || v === undefined || v === "") return null;
  const n = Math.round(Number(v));
  return Number.isFinite(n) ? Math.max(0, Math.min(100, n)) : null;
}
function num(v) {
  if (v === null || v === undefined || v === "") return null;
  const n = Number(v);
  return Number.isFinite(n) ? Math.round(n * 10) / 10 : null;
}

export default async (req) => {
  const st = store();

  if (req.method === "POST") {
    if (!autorizado(req, "device")) return json({ error: "token inválido" }, 401);
    let body;
    try { body = await req.json(); } catch { return json({ error: "JSON inválido" }, 400); }

    const ahora = new Date();
    const lectura = { created_at: ahora.toISOString() };
    for (const s of SENSORES) lectura[s] = pct(body[s]);
    lectura.temperatura = num(body.temperatura);
    lectura.humedad_aire = num(body.humedad_aire);
    for (const z of ZONAS) lectura["r" + z] = body["r" + z] ? 1 : 0;

    const key = claveDia(ahora);
    const dia = (await st.get(key, { type: "json" })) || [];
    const diaNuevo = dia.length === 0;
    dia.push(lectura);
    await st.setJSON(key, dia);
    await st.setJSON("ultima", lectura);

    // Eventos de riego que informa el ESP (inicio, cortes, rechazos)
    const control = await leerControl(st);
    if (Array.isArray(body.eventos) && body.eventos.length) {
      const eventos = await leerEventos(st);
      let cambioControl = false;
      for (const e of body.eventos.slice(0, 10)) {
        if (!ZONAS.includes(e.zona)) continue;
        eventos.push({ ts: ahora.toISOString(), zona: e.zona, tipo: String(e.tipo).slice(0, 20), valor: pct(e.valor) });
        // si el ESP cortó o rechazó, la orden vigente pasa a "detener" (sin cambiar el id)
        if (CORTES.includes(e.tipo) && control[e.zona].orden === "regar") {
          control[e.zona].orden = "detener";
          cambioControl = true;
        }
      }
      await st.setJSON("eventos", eventos.slice(-40));
      if (cambioControl) await st.setJSON("control", control);
    }

    if (diaNuevo) {
      const limite = claveDia(new Date(ahora - DIAS_RETENCION * 864e5));
      const { blobs } = await st.list({ prefix: "dia/" });
      await Promise.all(blobs.filter((b) => b.key < limite).map((b) => st.delete(b.key)));
    }

    return json({ ok: true, config: await leerConfig(st), control }, 201);
  }

  if (req.method === "GET") {
    const url = new URL(req.url);
    const horas = Math.max(1, Math.min(24 * 31, Number(url.searchParams.get("horas")) || 24));
    const hasta = new Date();
    const desde = new Date(hasta - horas * 3600e3);

    const claves = [];
    for (let d = new Date(desde); claveDia(d) <= claveDia(hasta); d = new Date(+d + 864e5)) claves.push(claveDia(d));
    if (!claves.includes(claveDia(hasta))) claves.push(claveDia(hasta));

    const dias = await Promise.all(claves.map((k) => st.get(k, { type: "json" })));
    let lecturas = dias.flat().filter((l) => l && new Date(l.created_at) >= desde);
    if (lecturas.length > 600) {
      const paso = Math.ceil(lecturas.length / 600);
      lecturas = lecturas.filter((_, i) => i % paso === 0 || i === lecturas.length - 1);
    }
    return json({ ultima: await st.get("ultima", { type: "json" }), lecturas });
  }

  return json({ error: "método no permitido" }, 405);
};

export const config = { path: "/api/lecturas" };
