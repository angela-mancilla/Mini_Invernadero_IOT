-- Ejecutar en Supabase: SQL Editor > New query > Run
-- Tabla donde el ESP32 guarda las lecturas del DHT11
create table sensores (
  id bigint primary key generated always as identity,
  id_dispositivo text not null,
  tipo_sensor text not null,
  valor_sensor numeric not null,
  fecha_hora timestamptz not null default now()   -- la completa Supabase al recibir el dato
);

-- Seguridad: con RLS activado, la clave pública solo puede INSERTAR registros,
-- no leerlos, modificarlos ni borrarlos
alter table sensores enable row level security;
create policy "ESP32 puede insertar" on sensores
  for insert to anon with check (true);

-- Para ver los datos guardados:
-- select * from sensores order by fecha_hora desc;
