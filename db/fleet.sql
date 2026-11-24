\set ON_ERROR_STOP on
BEGIN;
CREATE TABLE IF NOT EXISTS public.trip_profiles (
    trip_id uuid PRIMARY KEY,
    vehicle text CHECK (vehicle IN ('sienna_2009', 'x3_2017_n55', '335i_2007_n54', 'prius_2005'))
);
INSERT INTO public.trip_profiles (trip_id) SELECT DISTINCT trip_id FROM public.readings
ON CONFLICT DO NOTHING;
GRANT SELECT, INSERT ON public.trip_profiles TO carf_writer;
GRANT SELECT ON public.trip_profiles TO carf_reader;
ALTER TABLE public.readings ADD COLUMN IF NOT EXISTS observation jsonb;
COMMIT;
