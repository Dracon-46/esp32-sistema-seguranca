// Faz o papel do dashboard durante o cenario test/porta-e-senha.yaml:
// arma a central, desarma apos o primeiro alarme e, na segunda entrada,
// envia 3 senhas erradas pelo topico de comando.
const mqtt = require('mqtt');
const B = 'fatec/seguranca/casa01';
const opts = { username: 'public', password: 'public' };
if (process.env.HTTPS_PROXY) {
  const { HttpsProxyAgent } = require('https-proxy-agent');
  opts.wsOptions = { agent: new HttpsProxyAgent(process.env.HTTPS_PROXY) };
}
const c = mqtt.connect(process.env.BROKER || 'mqtts://public.cloud.shiftr.io', opts);
const cmd = (m) => { console.log('[comando]', m); c.publish(B + '/comando', m); };
let entradas = 0, alarmes = 0;
c.on('connect', () => c.subscribe(B + '/#'));
c.on('message', (t, m) => {
  const s = m.toString();
  if (t.endsWith('/evento')) {
    const e = JSON.parse(s);
    console.log('[evento]', e.tipo, e.origem, '-', e.descricao);
    if (e.tipo === 'BOOT' || (e.tipo === 'DESARMADO' && alarmes === 1)) setTimeout(() => cmd('ARMAR'), 1500);
    if (e.tipo === 'ENTRADA_DETECTADA' && ++entradas === 2) ['DESARMAR:0000', 'DESARMAR:1111', 'DESARMAR:2222'].forEach((x, i) => setTimeout(() => cmd(x), 600 * (i + 1)));
  }
  if (t.endsWith('/alerta')) {
    console.log('[alerta]', s);
    if (s.includes('origem: PORTA')) { alarmes++; setTimeout(() => cmd('DESARMAR:1234'), 1500); }
    if (s.includes('origem: SENHA')) setTimeout(() => process.exit(0), 1000);
  }
});
setTimeout(() => process.exit(1), 300000);
