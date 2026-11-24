\set ON_ERROR_STOP on
\getenv writer_password WRITER_PASSWORD
\getenv reader_password READER_PASSWORD

BEGIN;
REVOKE ALL ON DATABASE carf FROM PUBLIC;
REVOKE ALL ON SCHEMA public FROM PUBLIC;
CREATE ROLE carf_writer LOGIN PASSWORD :'writer_password';
CREATE ROLE carf_reader LOGIN PASSWORD :'reader_password';
GRANT CONNECT ON DATABASE carf TO carf_writer, carf_reader;
GRANT USAGE ON SCHEMA public TO carf_writer, carf_reader;
CREATE TABLE public.readings (
    trip_id uuid NOT NULL,
    elapsed_ms bigint NOT NULL CHECK (elapsed_ms >= 0),
    utc_s bigint,
    rpm double precision CHECK (rpm BETWEEN 0 AND 16383.75),
    speed_kph smallint CHECK (speed_kph BETWEEN 0 AND 255),
    coolant_c smallint CHECK (coolant_c BETWEEN -40 AND 215),
    received_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (trip_id, elapsed_ms),
    CHECK (num_nonnulls(rpm, speed_kph, coolant_c) > 0)
);
GRANT SELECT, INSERT ON public.readings TO carf_writer;
GRANT SELECT ON public.readings TO carf_reader;
ALTER ROLE carf_reader SET default_transaction_read_only = on;
COMMIT;
