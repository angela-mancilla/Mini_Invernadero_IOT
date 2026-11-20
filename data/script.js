/* ==========================================================
   script.js - Lógica de login.html e index.html
   Mini Invernadero Inteligente IoT (ESP32)

   Rutas que debe ofrecer el ESP32:
     POST /api/login    {usuario, password} -> 200 | 401 | 403
     GET  /logout       cierra la sesión
     GET  /api/data     lecturas y estado (JSON)
     POST /api/control  {ventana|luz|modo: valor}
     GET  /api/logs     bitácora en texto plano

   Este archivo NO guarda claves ni tokens.
   ========================================================== */
(function () {
  'use strict';

  /* ---------- Configuración ---------- */
  const RUTAS = {
    login:   '/api/login',
    logout:  '/logout',
    datos:   '/api/data',
    control: '/api/control',
    logs:    '/api/logs'
  };
  const INACTIVIDAD_SEG = 600;     // 10 minutos (igual que el ESP32)
  const LECTURA_CADA_MS = 5000;    // cada cuánto se piden datos
  const BLOQUEO_SEG = 300;         // 5 minutos (solo si el ESP32 no informa los segundos)

  const $ = (id) => document.getElementById(id);
  const formatoMinSeg = (s) => Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');

  const pagina = document.body.dataset.page;
  if (pagina === 'login') iniciarLogin();
  if (pagina === 'panel') iniciarPanel();

  /* ==========================================================
     LOGIN
     ========================================================== */
  function iniciarLogin() {
    const form = $('formLogin');
    const btn = $('btnLogin');
    const msg = $('mensaje');
    let timerBloqueo = null;

    function mostrar(texto, tipo) {
      msg.textContent = texto;
      msg.className = 'mensaje' + (tipo ? ' ' + tipo : '');
    }

    // Mensaje inicial según el motivo con el que se llegó al login
    const motivos = {
      expirada: 'Tu sesión expiró por inactividad. Inicia sesión de nuevo.',
      cerrada:  'Cerraste sesión correctamente.',
      denegado: 'Acceso denegado. Inicia sesión para continuar.'
    };
    const motivo = new URLSearchParams(location.search).get('motivo');
    if (motivos[motivo]) mostrar(motivos[motivo], motivo === 'cerrada' ? 'ok' : 'error');

    // Cuenta regresiva del bloqueo por fuerza bruta
    function iniciarBloqueo(segundos) {
      let restante = segundos;
      btn.disabled = true;
      const pintar = () => mostrar(
        'Cuenta bloqueada por demasiados intentos fallidos. Intenta de nuevo en ' + formatoMinSeg(restante) + '.',
        'error');
      pintar();
      clearInterval(timerBloqueo);
      timerBloqueo = setInterval(() => {
        restante--;
        if (restante <= 0) {
          clearInterval(timerBloqueo);
          timerBloqueo = null;
          btn.disabled = false;
          mostrar('Ya puedes intentar de nuevo.', '');
        } else {
          pintar();
        }
      }, 1000);
    }

    form.addEventListener('submit', async (e) => {
      e.preventDefault();
      if (timerBloqueo) return;
      btn.disabled = true;
      mostrar('Verificando...', '');
      try {
        const r = await fetch(RUTAS.login, {
          method: 'POST',
          credentials: 'same-origin',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ usuario: $('usuario').value.trim(), password: $('password').value })
        });
        const d = await r.json().catch(() => ({}));

        if (r.ok) { location.href = '/'; return; }     // login correcto

        $('password').value = '';
        if (r.status === 403 || d.bloqueado) {          // bloqueado
          iniciarBloqueo(Number(d.segundos) || BLOQUEO_SEG);
          return;
        }
        let texto = 'Usuario o contraseña incorrectos.';
        if (typeof d.intentos_restantes === 'number') {
          texto += ' Intentos restantes: ' + d.intentos_restantes + '.';
        }
        mostrar(texto, 'error');
      } catch (err) {
        mostrar('No se pudo conectar con el ESP32. Revisa que estés en la misma red WiFi.', 'error');
      }
      if (!timerBloqueo) btn.disabled = false;
    });
  }

  /* ==========================================================
     PANEL (página protegida)
     ========================================================== */
  function iniciarPanel() {
    let ultimaActividad = Date.now();   // para el contador de 10 minutos
    let huboActividad = false;          // ¿el usuario hizo algo desde la última lectura?
    let modoActual = 'auto';

    // El usuario está activo si hace clic, escribe, toca o mueve el mouse
    ['click', 'keydown', 'touchstart', 'mousemove'].forEach((ev) =>
      document.addEventListener(ev, () => { ultimaActividad = Date.now(); huboActividad = true; }, { passive: true })
    );

    /* ---------- Llamadas al ESP32 ----------
       Si la sesión no es válida (401/403 o redirección al login) vuelve al login.
       Las lecturas automáticas llevan el encabezado X-Auto: 1 para que el ESP32
       NO las cuente como actividad (si no, la sesión nunca expiraría). */
    async function api(ruta, opciones = {}, automatica = false) {
      const headers = Object.assign({}, opciones.headers);
      if (automatica) headers['X-Auto'] = '1';
      const r = await fetch(ruta, Object.assign({ credentials: 'same-origin' }, opciones, { headers }));
      if (r.status === 401 || r.status === 403 || (r.redirected && r.url.includes('/login'))) {
        location.href = '/login?motivo=expirada';
        throw new Error('sesion');
      }
      return r;
    }

    function aviso(texto) {
      $('aviso').textContent = texto;
      $('aviso').className = 'mensaje' + (texto ? ' error' : '');
    }

    /* ---------- Mostrar datos ---------- */
    const numero = (v) => (typeof v === 'number' && isFinite(v) ? v : null);

    function pintarDatos(d) {
      const t = numero(d.temperatura);
      const h = numero(d.humedad);
      $('temperatura').textContent = t === null ? '--' : t.toFixed(1);
      $('humedad').textContent = h === null ? '--' : h.toFixed(0);

      // Estado general
      const estado = $('estado');
      if (t === null || h === null) {
        estado.textContent = 'Error de lectura';
        estado.className = 'estado alerta';
        $('motivo').textContent = 'El sensor no entrega datos. Se mantiene el último estado seguro.';
      } else if (d.estado === 'alerta') {
        estado.textContent = 'Alerta';
        estado.className = 'estado alerta';
        $('motivo').textContent = (d.alertas || []).join(', ');
      } else {
        estado.textContent = 'En rango';
        estado.className = 'estado';
        $('motivo').textContent = '';
      }

      // Actuadores
      $('ventana').textContent = d.ventana === 'abierta' ? 'Abierta' : 'Cerrada';
      $('luz').textContent = d.luz === 'on' ? 'Encendida' : 'Apagada';

      // Modo: marca el activo y habilita los botones solo en manual
      modoActual = d.modo === 'manual' ? 'manual' : 'auto';
      document.querySelectorAll('[data-modo]').forEach((b) =>
        b.classList.toggle('activo', b.dataset.modo === modoActual));
      document.querySelectorAll('button.manual').forEach((b) => { b.disabled = modoActual !== 'manual'; });
      $('ayudaModo').textContent = modoActual === 'auto'
        ? 'En modo automático el sistema decide. Cambia a manual para usar la ventana y la luz.'
        : 'En modo manual tú controlas la ventana y la luz.';

      // Supabase
      const s = d.supabase;
      $('supabase').textContent = s && s.ultimo_envio
        ? 'Último envío: ' + s.ultimo_envio + (s.ok === false ? ' (con error)' : ' (correcto)')
        : 'Sin información de envíos todavía.';
    }

    async function cargarDatos() {
      const automatica = !huboActividad;   // sin actividad del usuario = lectura automática
      huboActividad = false;
      try {
        const r = await api(RUTAS.datos, {}, automatica);
        pintarDatos(await r.json());
        aviso('');
      } catch (err) {
        if (err.message !== 'sesion') aviso('No se pudo leer el ESP32. Reintentando...');
      }
    }

    /* ---------- Control de actuadores ---------- */
    document.addEventListener('click', async (e) => {
      const b = e.target.closest('[data-clave]');
      if (!b || b.disabled) return;
      const cuerpo = {};
      cuerpo[b.dataset.clave] = b.dataset.valor;
      try {
        await api(RUTAS.control, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(cuerpo)
        });
        cargarDatos();
      } catch (err) {
        if (err.message !== 'sesion') aviso('No se pudo enviar la orden.');
      }
    });

    /* ---------- Registro de accesos ---------- */
    async function cargarLogs() {
      try {
        const r = await api(RUTAS.logs);
        const lineas = (await r.text()).split('\n').map((l) => l.trim()).filter(Boolean);
        // Se usa textContent (no innerHTML) para que el texto del log nunca se ejecute como HTML
        $('logs').textContent = lineas.length ? lineas.slice(-100).reverse().join('\n') : 'Sin eventos registrados.';
      } catch (err) {
        if (err.message !== 'sesion') $('logs').textContent = 'No se pudo leer el registro.';
      }
    }
    $('btnLogs').addEventListener('click', cargarLogs);

    /* ---------- Logout ---------- */
    async function cerrarSesion(motivo) {
      try { await fetch(RUTAS.logout, { credentials: 'same-origin' }); } catch (e) { /* se ignora */ }
      location.href = '/login?motivo=' + motivo;
    }
    $('btnLogout').addEventListener('click', () => cerrarSesion('cerrada'));

    /* ---------- Contador de inactividad (10 min) ---------- */
    setInterval(() => {
      const restante = INACTIVIDAD_SEG - Math.floor((Date.now() - ultimaActividad) / 1000);
      if (restante <= 0) { cerrarSesion('expirada'); return; }
      $('tiempoSesion').textContent = formatoMinSeg(restante);
    }, 1000);

    /* ---------- Inicio ---------- */
    cargarDatos();
    cargarLogs();
    setInterval(cargarDatos, LECTURA_CADA_MS);
  }
})();
